#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sg2002_rtos.h"

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define DEFAULT_SAMPLES 2000U
#define DEFAULT_WARMUP 100U
#define MAX_SAMPLES 1000000U
#define CMDQU_BYTES_PER_ROUND_TRIP 16U
#define VALUE_BYTES_PER_ROUND_TRIP 8U
#define SHM_BYTES_PER_ROUND_TRIP (SG2002_RTOS_SHM_SLOT_SIZE * 2U)
#define MAX_ECHO_DATA_BYTES (SG2002_RTOS_SHM_PAYLOAD_SIZE - 1U)

struct window_ticket {
	uint32_t sequence;
	uint32_t seed;
	struct timespec start;
	int active;
};

struct window_stats {
	uint32_t max_in_flight;
	uint64_t send_eagain;
	uint64_t receive_eagain;
	uint64_t poll_wakeups;
	uint64_t submit_calls;
	uint64_t reap_calls;
};

static uint32_t parse_count(const char *text)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 0);

	if (text[0] == '\0' || end == NULL || *end != '\0' || value == 0 ||
	    value > MAX_SAMPLES) {
		fprintf(stderr, "invalid count: %s\n", text);
		exit(2);
	}
	return (uint32_t)value;
}

static uint32_t parse_payload_size(const char *text)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 0);

	if (text[0] == '\0' || end == NULL || *end != '\0' ||
	    value > MAX_ECHO_DATA_BYTES) {
		fprintf(stderr, "invalid payload size: %s\n", text);
		exit(2);
	}
	return (uint32_t)value;
}

static uint32_t parse_window(const char *text)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 0);

	if (text[0] == '\0' || end == NULL || *end != '\0' || value == 0U ||
	    value > SG2002_RTOS_SHM_SLOT_COUNT) {
		fprintf(stderr, "invalid window: %s (expected 1..%u)\n", text,
			SG2002_RTOS_SHM_SLOT_COUNT);
		exit(2);
	}
	return (uint32_t)value;
}

static uint64_t elapsed_ns(const struct timespec *start,
			   const struct timespec *end)
{
	int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
	int64_t nanoseconds = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;

	return (uint64_t)(seconds * 1000000000LL + nanoseconds);
}

static int compare_u64(const void *left, const void *right)
{
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *samples, uint32_t count,
			   uint32_t percent)
{
	uint64_t rank = ((uint64_t)percent * count + 99U) / 100U;

	if (rank == 0)
		rank = 1;
	return samples[rank - 1U];
}

static uint16_t build_request(uint8_t *request, uint32_t echo_data_bytes,
			      uint32_t seed)
{
	uint16_t request_length;
	uint32_t index;

	if (echo_data_bytes == 0U) {
		request[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] =
			SG2002_RTOS_CMD_PING;
		memcpy(request + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET, &seed,
		       sizeof(seed));
		return SG2002_RTOS_VALUE_MESSAGE_SIZE;
	}

	request_length = (uint16_t)(echo_data_bytes + 1U);
	request[0] = SG2002_RTOS_CMD_ECHO;
	for (index = 1U; index < request_length; index++)
		request[index] = (uint8_t)(seed + index * 17U);
	return request_length;
}

static int validate_response(const uint8_t *response, uint16_t response_length,
			     uint32_t echo_data_bytes, uint32_t seed)
{
	uint16_t expected_length;
	uint32_t index;
	uint32_t value;

	if (echo_data_bytes == 0U) {
		if (response_length != SG2002_RTOS_VALUE_MESSAGE_SIZE ||
		    response[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] !=
			    SG2002_RTOS_CMD_PING)
			return -EPROTO;
		memcpy(&value,
		       response + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       sizeof(value));
		return value == (seed ^ SG2002_RTOS_PING_XOR) ? 0 : -EPROTO;
	}

	expected_length = (uint16_t)(echo_data_bytes + 1U);
	if (response_length != expected_length ||
	    response[0] != SG2002_RTOS_CMD_ECHO)
		return -EPROTO;
	for (index = 1U; index < expected_length; index++) {
		if (response[index] != (uint8_t)(seed + index * 17U))
			return -EPROTO;
	}
	return 0;
}

static struct window_ticket *find_ticket(struct window_ticket *tickets,
					 uint32_t sequence)
{
	uint32_t index;

	for (index = 0U; index < SG2002_RTOS_SHM_SLOT_COUNT; index++) {
		if (tickets[index].active &&
		    tickets[index].sequence == sequence)
			return &tickets[index];
	}
	return NULL;
}

static struct window_ticket *find_free_ticket(struct window_ticket *tickets)
{
	uint32_t index;

	for (index = 0U; index < SG2002_RTOS_SHM_SLOT_COUNT; index++) {
		if (!tickets[index].active)
			return &tickets[index];
	}
	return NULL;
}

static int complete_response(struct window_ticket *tickets,
	const struct sg2002_rtos_shm_slot *message, uint32_t echo_data_bytes,
	uint64_t *latency, uint64_t *latency_sum, uint32_t *completed,
	uint32_t *in_flight)
{
	struct window_ticket *ticket;
	struct timespec end;
	int ret;

	if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0)
		return -errno;
	ticket = find_ticket(tickets, message->sequence);
	if (ticket == NULL)
		return -EPROTO;
	ret = validate_response(message->payload, message->length,
				echo_data_bytes, ticket->seed);
	if (ret)
		return ret;
	if (latency != NULL) {
		latency[*completed] = elapsed_ns(&ticket->start, &end);
		*latency_sum += latency[*completed];
	}
	ticket->active = 0;
	(*in_flight)--;
	(*completed)++;
	return 0;
}

static int run_windowed_phase(struct sg2002_rtos *rtos, uint32_t count,
			      uint32_t echo_data_bytes, uint32_t seed_base,
			      uint32_t window, int use_batch, uint64_t *latency,
			      uint64_t *latency_sum,
			      uint32_t *completed_out,
			      struct window_stats *stats)
{
	struct window_ticket tickets[SG2002_RTOS_SHM_SLOT_COUNT] = {0};
	struct sg2002_rtos_shm_slot messages[SG2002_RTOS_SHM_SLOT_COUNT] = {0};
	short events;
	short revents;
	struct window_ticket *ticket;
	struct timespec start;
	uint32_t submitted = 0U;
	uint32_t completed = 0U;
	uint32_t in_flight = 0U;
	int progressed;
	int ret;

	if (latency_sum == NULL || completed_out == NULL || stats == NULL)
		return -EINVAL;
	memset(stats, 0, sizeof(*stats));
	*latency_sum = 0U;
	*completed_out = 0U;
	while (completed < count) {
		progressed = 0;
		while (in_flight > 0U) {
			uint16_t reaped = 0U;
			uint16_t reap_count = use_batch ?
				(uint16_t)in_flight : 1U;
			uint16_t index;

			stats->reap_calls++;
			if (use_batch) {
				ret = sg2002_rtos_message_reap_batch(
					rtos, messages, reap_count, &reaped, 0U);
			} else {
				ret = sg2002_rtos_message_receive(
					rtos, messages[0].payload,
					sizeof(messages[0].payload),
					&messages[0].length,
					&messages[0].sequence, 0U);
				if (ret == 0)
					reaped = 1U;
			}
			if (ret == -EAGAIN) {
				stats->receive_eagain++;
				break;
			}
			if (ret)
				goto out;
			for (index = 0U; index < reaped; index++) {
				ret = complete_response(
					tickets, &messages[index], echo_data_bytes,
					latency, latency_sum, &completed, &in_flight);
				if (ret)
					goto out;
			}
			progressed = 1;
		}

		while (submitted < count && in_flight < window) {
			uint16_t submit_count = use_batch ?
				(uint16_t)(window - in_flight) : 1U;
			uint32_t remaining = count - submitted;
			uint16_t submitted_now = 0U;
			uint16_t index;

			if ((uint32_t)submit_count > remaining)
				submit_count = (uint16_t)remaining;
			for (index = 0U; index < submit_count; index++) {
				uint32_t seed = seed_base + submitted + index;

				memset(&messages[index], 0, sizeof(messages[index]));
				messages[index].length = build_request(
					messages[index].payload, echo_data_bytes, seed);
			}
			if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
				ret = -errno;
				goto out;
			}
			stats->submit_calls++;
			if (use_batch) {
				ret = sg2002_rtos_message_submit_batch(
					rtos, messages, submit_count, &submitted_now, 0U);
			} else {
				ret = sg2002_rtos_message_send(
					rtos, messages[0].payload, messages[0].length,
					0U, &messages[0].sequence);
				if (ret == 0)
					submitted_now = 1U;
			}
			if (ret == -EAGAIN) {
				stats->send_eagain++;
				break;
			}
			if (ret)
				goto out;
			for (index = 0U; index < submitted_now; index++) {
				if (find_ticket(tickets, messages[index].sequence) !=
				    NULL) {
					ret = -EPROTO;
					goto out;
				}
				ticket = find_free_ticket(tickets);
				if (ticket == NULL) {
					ret = -EOVERFLOW;
					goto out;
				}
				ticket->sequence = messages[index].sequence;
				ticket->seed = seed_base + submitted + index;
				ticket->start = start;
				ticket->active = 1;
			}
			submitted += submitted_now;
			in_flight += submitted_now;
			if (in_flight > stats->max_in_flight)
				stats->max_in_flight = in_flight;
			progressed = 1;
		}

		if (completed == count)
			break;
		if (progressed)
			continue;

		events = 0;
		revents = 0;
		if (in_flight > 0U)
			events |= POLLIN;
		if (submitted < count && in_flight < window)
			events |= POLLOUT;
		if (events == 0) {
			ret = -EDEADLK;
			goto out;
		}
		ret = sg2002_rtos_wait(rtos, events, &revents,
			SG2002_RTOS_DEFAULT_TIMEOUT_MS);
		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			goto out;
		}
		if (ret == 0) {
			ret = -ETIMEDOUT;
			goto out;
		}
		stats->poll_wakeups++;
		if (revents & POLLNVAL) {
			ret = -EBADF;
			goto out;
		}
		if (revents & (POLLERR | POLLHUP)) {
			ret = -ENOTCONN;
			goto out;
		}
		if ((revents & events) == 0) {
			ret = -EIO;
			goto out;
		}
	}
	ret = 0;

out:
	*completed_out = completed;
	return ret;
}

static int run_transaction(struct sg2002_rtos *rtos,
			   uint32_t echo_data_bytes, uint32_t seed,
			   uint32_t *ping_result)
{
	uint8_t request[SG2002_RTOS_SHM_PAYLOAD_SIZE];
	uint8_t response[SG2002_RTOS_SHM_PAYLOAD_SIZE];
	uint16_t response_length;
	uint16_t request_length;
	uint32_t index;
	int ret;

	if (echo_data_bytes == 0U) {
		ret = sg2002_rtos_ping(rtos, seed, ping_result);
		if (ret)
			return ret;
		return *ping_result == (seed ^ SG2002_RTOS_PING_XOR) ? 0 :
			-EPROTO;
	}

	request_length = (uint16_t)(echo_data_bytes + 1U);
	request[0] = SG2002_RTOS_CMD_ECHO;
	for (index = 1U; index < request_length; index++)
		request[index] = (uint8_t)(seed + index * 17U);
	ret = sg2002_rtos_message_call(
		rtos, request, request_length, response, sizeof(response),
		&response_length, SG2002_RTOS_DEFAULT_TIMEOUT_MS);
	if (ret)
		return ret;
	if (response_length != request_length ||
	    memcmp(request, response, request_length) != 0)
		return -EPROTO;
	*ping_result = 0U;
	return 0;
}

int main(int argc, char **argv)
{
	struct sg2002_rtos rtos = SG2002_RTOS_INITIALIZER;
	struct timespec total_start;
	struct timespec total_end;
	struct timespec start;
	struct timespec end;
	uint64_t *latency;
	uint64_t total_ns;
	uint64_t latency_sum = 0;
	uint32_t requested = DEFAULT_SAMPLES;
	uint32_t warmup = DEFAULT_WARMUP;
	uint32_t echo_data_bytes = 0U;
	uint32_t window = 0U;
	uint32_t completed = 0;
	uint32_t failed = 0;
	uint32_t i;
	uint32_t result = 0;
	double seconds;
	double transactions_per_second;
	uint32_t transport_bytes_per_round_trip;
	uint32_t message_bytes_per_round_trip;
	uint32_t application_bytes_per_round_trip;
	struct window_stats window_stats = {0};
	int shared_memory;
	int native_batch = -EAGAIN;
	int use_batch = 0;
	int ret;

	if (argc > 6) {
		fprintf(stderr,
			"Usage: rtos-bench [samples [warmup [echo-data-bytes "
			"[window [split|batch]]]]]\n");
		return 2;
	}
	if (argc >= 2)
		requested = parse_count(argv[1]);
	if (argc >= 3)
		warmup = parse_count(argv[2]);
	if (argc >= 4)
		echo_data_bytes = parse_payload_size(argv[3]);
	if (argc >= 5)
		window = parse_window(argv[4]);
	if (argc == 6) {
		if (strcmp(argv[5], "batch") == 0) {
			use_batch = 1;
		} else if (strcmp(argv[5], "split") != 0) {
			fprintf(stderr, "invalid window mode: %s\n", argv[5]);
			return 2;
		}
	}

	latency = calloc(requested, sizeof(*latency));
	if (latency == NULL) {
		perror("calloc");
		return 1;
	}
	ret = sg2002_rtos_open(&rtos, NULL);
	if (ret) {
		fprintf(stderr, "open failed: %d\n", ret);
		free(latency);
		return 1;
	}
	shared_memory = sg2002_rtos_shared_memory_available(&rtos);
	if (echo_data_bytes > 0U && !shared_memory) {
		fprintf(stderr, "echo payload mode requires shared memory\n");
		sg2002_rtos_close(&rtos);
		free(latency);
		return 1;
	}
	if (window > 0U && !shared_memory) {
		fprintf(stderr, "window mode requires shared memory\n");
		sg2002_rtos_close(&rtos);
		free(latency);
		return 1;
	}

	if (window > 0U) {
		struct window_stats warmup_stats;
		uint64_t warmup_latency_sum;
		uint32_t warmup_completed;

		ret = run_windowed_phase(
			&rtos, warmup, echo_data_bytes, 0x13579bdfU, window,
			use_batch, NULL, &warmup_latency_sum, &warmup_completed,
			&warmup_stats);
		if (ret || warmup_completed != warmup) {
			fprintf(stderr,
				"windowed warmup failed after %u/%u: ret=%d\n",
				warmup_completed, warmup, ret);
			sg2002_rtos_close(&rtos);
			free(latency);
			return 1;
		}
	} else {
		for (i = 0; i < warmup; i++) {
			uint32_t input = 0x13579bdfU + i;

			ret = run_transaction(&rtos, echo_data_bytes, input,
					      &result);
			if (ret) {
				fprintf(stderr,
					"warmup failed at %u: ret=%d result=0x%08x\n",
					i, ret, result);
				sg2002_rtos_close(&rtos);
				free(latency);
				return 1;
			}
		}
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &total_start);
	if (window > 0U) {
		ret = run_windowed_phase(
			&rtos, requested, echo_data_bytes, 0x2468ace0U, window,
			use_batch, latency, &latency_sum, &completed,
			&window_stats);
		if (ret) {
			failed++;
			fprintf(stderr,
				"windowed benchmark failed after %u/%u: ret=%d\n",
				completed, requested, ret);
		}
	} else {
		for (i = 0; i < requested; i++) {
			uint32_t input = 0x2468ace0U + i;

			clock_gettime(CLOCK_MONOTONIC_RAW, &start);
			ret = run_transaction(&rtos, echo_data_bytes, input,
					      &result);
			clock_gettime(CLOCK_MONOTONIC_RAW, &end);
			if (ret) {
				failed++;
				break;
			}
			latency[completed] = elapsed_ns(&start, &end);
			latency_sum += latency[completed];
			completed++;
		}
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &total_end);
	if (window > 0U && use_batch && failed == 0U) {
		native_batch = sg2002_rtos_native_batch_available(&rtos);
		if (native_batch < 0) {
			failed++;
			fprintf(stderr, "batch capability unresolved: %d\n",
				native_batch);
		}
	}
	sg2002_rtos_close(&rtos);

	total_ns = elapsed_ns(&total_start, &total_end);
	seconds = (double)total_ns / 1000000000.0;
	transactions_per_second = seconds > 0.0 ? completed / seconds : 0.0;
	transport_bytes_per_round_trip = shared_memory ?
		SHM_BYTES_PER_ROUND_TRIP : CMDQU_BYTES_PER_ROUND_TRIP;
	message_bytes_per_round_trip = shared_memory ?
		(echo_data_bytes > 0U ? (echo_data_bytes + 1U) * 2U :
		 SG2002_RTOS_VALUE_MESSAGE_SIZE * 2U) :
		VALUE_BYTES_PER_ROUND_TRIP;
	application_bytes_per_round_trip = echo_data_bytes > 0U ?
		echo_data_bytes * 2U : VALUE_BYTES_PER_ROUND_TRIP;
	if (completed > 0)
		qsort(latency, completed, sizeof(*latency), compare_u64);

	printf("{\n");
	if (window > 0U) {
		printf("  \"path\": \"userspace_wait_shared_memory_windowed_%s_"
		       "freertos_round_trip\",\n",
		       use_batch ? (native_batch == 1 ? "batch_native" :
				      (native_batch == 0 ? "batch_compat" :
				       "batch_unknown")) : "split");
	} else {
		printf("  \"path\": \"userspace_ioctl_%s_freertos_round_trip\",\n",
		       shared_memory ? "shared_memory" : "cmdqu");
	}
	printf("  \"transport\": \"%s\",\n",
	       shared_memory ? "shared_memory" : "cmdqu");
	printf("  \"mode\": \"%s\",\n", window == 0U ? "synchronous" :
	       (use_batch ? "windowed_batch" : "windowed_split"));
	printf("  \"batch_backend\": \"%s\",\n", !use_batch ? "not_requested" :
	       (native_batch == 1 ? "native_ioctl" :
	       (native_batch == 0 ? "single_ioctl_fallback" : "unknown")));
	printf("  \"window_requested\": %u,\n", window);
	printf("  \"max_in_flight_observed\": %u,\n",
	       window > 0U ? window_stats.max_in_flight :
			       (completed > 0U ? 1U : 0U));
	printf("  \"send_eagain\": %" PRIu64 ",\n",
	       window_stats.send_eagain);
	printf("  \"receive_eagain\": %" PRIu64 ",\n",
	       window_stats.receive_eagain);
	printf("  \"poll_wakeups\": %" PRIu64 ",\n",
	       window_stats.poll_wakeups);
	printf("  \"call_metric_scope\": "
	       "\"userspace_api_not_kernel_ioctl\",\n");
	printf("  \"api_submit_calls\": %" PRIu64 ",\n",
	       window_stats.submit_calls);
	printf("  \"api_reap_calls\": %" PRIu64 ",\n",
	       window_stats.reap_calls);
	printf("  \"submit_calls\": %" PRIu64 ",\n",
	       window_stats.submit_calls);
	printf("  \"reap_calls\": %" PRIu64 ",\n",
	       window_stats.reap_calls);
	printf("  \"clock\": \"CLOCK_MONOTONIC_RAW\",\n");
	printf("  \"transport_bytes_per_round_trip\": %u,\n",
	       transport_bytes_per_round_trip);
	printf("  \"message_bytes_per_round_trip\": %u,\n",
	       message_bytes_per_round_trip);
	printf("  \"application_bytes_per_round_trip\": %u,\n",
	       application_bytes_per_round_trip);
	printf("  \"echo_data_bytes_each_direction\": %u,\n",
	       echo_data_bytes);
	printf("  \"warmup\": %u,\n", warmup);
	printf("  \"samples_requested\": %u,\n", requested);
	printf("  \"samples_completed\": %u,\n", completed);
	printf("  \"failures\": %u,\n", failed);
	printf("  \"elapsed_ns\": %" PRIu64 ",\n", total_ns);
	printf("  \"transactions_per_second\": %.3f,\n",
	       transactions_per_second);
	printf("  \"wire_bytes_per_second\": %.3f,\n",
	       transactions_per_second * transport_bytes_per_round_trip);
	printf("  \"payload_bytes_per_second\": %.3f,\n",
	       transactions_per_second * application_bytes_per_round_trip);
	if (completed > 0) {
		printf("  \"latency_ns\": {\"min\": %" PRIu64
		       ", \"mean\": %.3f, \"p50\": %" PRIu64
		       ", \"p95\": %" PRIu64 ", \"p99\": %" PRIu64
		       ", \"max\": %" PRIu64 "}\n",
		       latency[0], (double)latency_sum / completed,
		       percentile(latency, completed, 50),
		       percentile(latency, completed, 95),
		       percentile(latency, completed, 99), latency[completed - 1]);
	} else {
		printf("  \"latency_ns\": null\n");
	}
	printf("}\n");

	free(latency);
	return failed == 0 && completed == requested ? 0 : 1;
}
