#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "sg2002_rtos.h"

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define DEFAULT_WORKERS 1U
#define DEFAULT_SAMPLES 100000U
#define DEFAULT_WARMUP 8000U
#define DEFAULT_ECHO_DATA_BYTES 1017U
#define MAX_WORKERS 8U
#define MAX_SAMPLES 1000000U
#define MAX_ECHO_DATA_BYTES (SG2002_RTOS_SHM_PAYLOAD_SIZE - 1U)
#define SHM_BYTES_PER_ROUND_TRIP (SG2002_RTOS_SHM_SLOT_SIZE * 2U)
#define VALUE_BYTES_PER_ROUND_TRIP 8U

enum worker_state {
	WORKER_STARTING,
	WORKER_IDLE,
	WORKER_READY,
	WORKER_SUBMITTING,
	WORKER_IN_FLIGHT,
	WORKER_COMPLETE,
	WORKER_EXITED,
};

struct thread_phase;

struct worker {
	struct thread_phase *phase;
	pthread_t thread;
	pthread_cond_t completion;
	struct sg2002_rtos_shm_slot request;
	struct timespec start;
	enum worker_state state;
	uint32_t id;
	uint32_t target;
	uint32_t issued;
	uint32_t seed;
	uint32_t sequence;
};

struct phase_stats {
	uint32_t max_ready;
	uint32_t max_in_flight;
	uint32_t max_outstanding;
	uint64_t send_eagain;
	uint64_t receive_eagain;
	uint64_t poll_calls;
	uint64_t poll_wakeups;
	uint64_t dispatcher_notifications_consumed;
	uint64_t submit_calls;
	uint64_t submit_successes;
	uint64_t submitted_messages;
	uint64_t reap_calls;
	uint64_t reap_successes;
	uint64_t reaped_messages;
	uint16_t max_submit_batch;
	uint16_t max_reap_batch;
};

struct thread_phase {
	struct sg2002_rtos *rtos;
	pthread_mutex_t lock;
	pthread_cond_t dispatcher;
	pthread_cond_t start_gate;
	struct worker workers[MAX_WORKERS];
	struct phase_stats stats;
	uint64_t *latency;
	uint64_t latency_sum;
	uint64_t elapsed_ns;
	uint32_t worker_count;
	uint32_t total;
	uint32_t completed;
	uint32_t started;
	uint32_t ready;
	uint32_t submitting;
	uint32_t in_flight;
	uint32_t seed_base;
	uint32_t echo_data_bytes;
	int wake_fd;
	int go;
	int stop;
	int error;
};

static uint32_t parse_range(const char *text, const char *name,
	uint32_t minimum, uint32_t maximum)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 0);

	if (text[0] == '\0' || end == NULL || *end != '\0' ||
	    value < minimum || value > maximum) {
		fprintf(stderr, "invalid %s: %s (expected %u..%u)\n", name,
			text, minimum, maximum);
		exit(2);
	}
	return (uint32_t)value;
}

static uint64_t elapsed_nanoseconds(const struct timespec *start,
	const struct timespec *end)
{
	int64_t seconds = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
	int64_t nanoseconds = (int64_t)end->tv_nsec -
		(int64_t)start->tv_nsec;

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

	if (rank == 0U)
		rank = 1U;
	return samples[rank - 1U];
}

static uint32_t worker_target(uint32_t total, uint32_t workers,
	uint32_t worker)
{
	return total / workers + (worker < total % workers ? 1U : 0U);
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

static int validate_response(const struct sg2002_rtos_shm_slot *response,
	uint32_t echo_data_bytes, uint32_t seed)
{
	uint16_t expected_length;
	uint32_t index;
	uint32_t value;

	if (echo_data_bytes == 0U) {
		if (response->length != SG2002_RTOS_VALUE_MESSAGE_SIZE ||
		    response->payload[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] !=
			    SG2002_RTOS_CMD_PING)
			return -EPROTO;
		memcpy(&value,
		       response->payload + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       sizeof(value));
		return value == (seed ^ SG2002_RTOS_PING_XOR) ? 0 : -EPROTO;
	}

	expected_length = (uint16_t)(echo_data_bytes + 1U);
	if (response->length != expected_length ||
	    response->payload[0] != SG2002_RTOS_CMD_ECHO)
		return -EPROTO;
	for (index = 1U; index < expected_length; index++) {
		if (response->payload[index] !=
		    (uint8_t)(seed + index * 17U))
			return -EPROTO;
	}
	return 0;
}

static void update_depth_stats(struct thread_phase *phase)
{
	uint32_t outstanding = phase->ready + phase->submitting +
		phase->in_flight;

	if (phase->ready > phase->stats.max_ready)
		phase->stats.max_ready = phase->ready;
	if (phase->in_flight > phase->stats.max_in_flight)
		phase->stats.max_in_flight = phase->in_flight;
	if (outstanding > phase->stats.max_outstanding)
		phase->stats.max_outstanding = outstanding;
}

static int signal_dispatcher(struct thread_phase *phase)
{
	uint64_t value = 1U;
	ssize_t written;

	do {
		written = write(phase->wake_fd, &value, sizeof(value));
	} while (written < 0 && errno == EINTR);
	if (written == (ssize_t)sizeof(value) ||
	    (written < 0 && errno == EAGAIN))
		return 0;
	return written < 0 ? -errno : -EIO;
}

static int consume_dispatcher_signal(struct thread_phase *phase,
	uint64_t *notifications)
{
	uint64_t value;
	ssize_t received;

	do {
		received = read(phase->wake_fd, &value, sizeof(value));
	} while (received < 0 && errno == EINTR);
	if (received == (ssize_t)sizeof(value)) {
		*notifications = value;
		return 0;
	}
	if (received < 0 && errno == EAGAIN) {
		*notifications = 0U;
		return 0;
	}
	return received < 0 ? -errno : -EIO;
}

static void fail_phase_locked(struct thread_phase *phase, int error)
{
	uint32_t index;

	if (phase->error == 0)
		phase->error = error != 0 ? error : -EIO;
	phase->stop = 1;
	for (index = 0U; index < phase->worker_count; index++)
		pthread_cond_signal(&phase->workers[index].completion);
	pthread_cond_broadcast(&phase->dispatcher);
	pthread_cond_broadcast(&phase->start_gate);
	if (phase->wake_fd >= 0)
		(void)signal_dispatcher(phase);
}

static void *worker_main(void *argument)
{
	struct worker *worker = argument;
	struct thread_phase *phase = worker->phase;
	uint32_t operation;
	int ret;

	ret = pthread_mutex_lock(&phase->lock);
	if (ret != 0)
		return NULL;
	phase->started++;
	worker->state = WORKER_IDLE;
	pthread_cond_signal(&phase->dispatcher);
	while (!phase->go && !phase->stop)
		pthread_cond_wait(&phase->start_gate, &phase->lock);

	for (operation = 0U; operation < worker->target && !phase->stop;
	     operation++) {
		worker->seed = phase->seed_base +
			operation * phase->worker_count + worker->id;
		memset(&worker->request, 0, sizeof(worker->request));
		worker->request.length = build_request(
			worker->request.payload, phase->echo_data_bytes,
			worker->seed);
		if (clock_gettime(CLOCK_MONOTONIC_RAW, &worker->start) != 0) {
			fail_phase_locked(phase, -errno);
			break;
		}
		worker->state = WORKER_READY;
		worker->issued++;
		phase->ready++;
		update_depth_stats(phase);
		ret = signal_dispatcher(phase);
		if (ret != 0) {
			fail_phase_locked(phase, ret);
			break;
		}
		while (worker->state != WORKER_COMPLETE && !phase->stop)
			pthread_cond_wait(&worker->completion, &phase->lock);
		if (!phase->stop)
			worker->state = WORKER_IDLE;
	}
	worker->state = WORKER_EXITED;
	pthread_cond_signal(&phase->dispatcher);
	pthread_mutex_unlock(&phase->lock);
	return NULL;
}

static struct worker *find_in_flight_worker(struct thread_phase *phase,
	uint32_t sequence)
{
	uint32_t index;

	for (index = 0U; index < phase->worker_count; index++) {
		if (phase->workers[index].state == WORKER_IN_FLIGHT &&
		    phase->workers[index].sequence == sequence)
			return &phase->workers[index];
	}
	return NULL;
}

static int reap_ready_responses(struct thread_phase *phase,
	uint32_t echo_data_bytes, int *progressed)
{
	struct sg2002_rtos_shm_slot messages[MAX_WORKERS];
	struct timespec end;
	uint16_t requested;
	uint16_t reaped = 0U;
	uint16_t index;
	int ret;

	pthread_mutex_lock(&phase->lock);
	requested = (uint16_t)phase->in_flight;
	pthread_mutex_unlock(&phase->lock);
	if (requested == 0U)
		return 0;

	phase->stats.reap_calls++;
	ret = sg2002_rtos_message_reap_batch(
		phase->rtos, messages, requested, &reaped, 0U);
	if (ret == -EAGAIN) {
		phase->stats.receive_eagain++;
		return 0;
	}
	if (ret)
		return ret;
	if (reaped == 0U || reaped > requested)
		return -EPROTO;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0)
		return -errno;

	phase->stats.reap_successes++;
	phase->stats.reaped_messages += reaped;
	if (reaped > phase->stats.max_reap_batch)
		phase->stats.max_reap_batch = reaped;
	pthread_mutex_lock(&phase->lock);
	for (index = 0U; index < reaped; index++) {
		struct worker *worker = find_in_flight_worker(
			phase, messages[index].sequence);
		uint64_t latency;

		if (worker == NULL ||
		    validate_response(&messages[index], echo_data_bytes,
				      worker->seed) != 0) {
			fail_phase_locked(phase, -EPROTO);
			pthread_mutex_unlock(&phase->lock);
			return -EPROTO;
		}
		latency = elapsed_nanoseconds(&worker->start, &end);
		if (phase->latency != NULL) {
			phase->latency[phase->completed] = latency;
			phase->latency_sum += latency;
		}
		worker->state = WORKER_COMPLETE;
		phase->in_flight--;
		phase->completed++;
		pthread_cond_signal(&worker->completion);
	}
	update_depth_stats(phase);
	pthread_mutex_unlock(&phase->lock);
	*progressed = 1;
	return 0;
}

static int submit_ready_workers(struct thread_phase *phase, int *progressed)
{
	struct sg2002_rtos_shm_slot messages[MAX_WORKERS];
	struct worker *selected[MAX_WORKERS];
	uint16_t count = 0U;
	uint16_t submitted = 0U;
	uint16_t index;
	uint32_t worker_index;
	int ret;

	pthread_mutex_lock(&phase->lock);
	for (worker_index = 0U; worker_index < phase->worker_count;
	     worker_index++) {
		struct worker *worker = &phase->workers[worker_index];

		if (worker->state != WORKER_READY)
			continue;
		messages[count] = worker->request;
		selected[count] = worker;
		worker->state = WORKER_SUBMITTING;
		phase->ready--;
		phase->submitting++;
		count++;
	}
	update_depth_stats(phase);
	pthread_mutex_unlock(&phase->lock);
	if (count == 0U)
		return 0;

	phase->stats.submit_calls++;
	ret = sg2002_rtos_message_submit_batch(
		phase->rtos, messages, count, &submitted, 0U);
	pthread_mutex_lock(&phase->lock);
	if (ret != 0 && ret != -EAGAIN) {
		for (index = 0U; index < count; index++) {
			selected[index]->state = WORKER_READY;
			phase->submitting--;
			phase->ready++;
		}
		fail_phase_locked(phase, ret);
		pthread_mutex_unlock(&phase->lock);
		return ret;
	}
	if (ret == -EAGAIN)
		phase->stats.send_eagain++;
	if (ret == 0 && (submitted == 0U || submitted > count)) {
		fail_phase_locked(phase, -EPROTO);
		pthread_mutex_unlock(&phase->lock);
		return -EPROTO;
	}
	for (index = 0U; index < submitted; index++) {
		if (messages[index].sequence == 0U ||
		    find_in_flight_worker(phase,
					 messages[index].sequence) != NULL) {
			fail_phase_locked(phase, -EPROTO);
			pthread_mutex_unlock(&phase->lock);
			return -EPROTO;
		}
		selected[index]->sequence = messages[index].sequence;
		selected[index]->state = WORKER_IN_FLIGHT;
		phase->submitting--;
		phase->in_flight++;
	}
	for (; index < count; index++) {
		selected[index]->state = WORKER_READY;
		phase->submitting--;
		phase->ready++;
	}
	update_depth_stats(phase);
	pthread_mutex_unlock(&phase->lock);
	if (submitted > 0U) {
		phase->stats.submit_successes++;
		phase->stats.submitted_messages += submitted;
		if (submitted > phase->stats.max_submit_batch)
			phase->stats.max_submit_batch = submitted;
		*progressed = 1;
	}
	return 0;
}

static int poll_transport_or_worker(struct thread_phase *phase,
	int transport_fd, short events, int timeout_ms)
{
	struct pollfd descriptors[2] = {0};
	nfds_t descriptor_count = 1U;
	uint64_t notifications = 0U;
	int ret;

	descriptors[0].fd = phase->wake_fd;
	descriptors[0].events = POLLIN;
	if (events != 0) {
		if (transport_fd < 0)
			return -EBADF;
		descriptors[1].fd = transport_fd;
		descriptors[1].events = events;
		descriptor_count = 2U;
	}
	phase->stats.poll_calls++;
	ret = poll(descriptors, descriptor_count, timeout_ms);
	if (ret < 0)
		return errno == EINTR ? 0 : -errno;
	if (ret == 0)
		return -ETIMEDOUT;
	phase->stats.poll_wakeups++;
	if (descriptors[0].revents & POLLNVAL)
		return -EBADF;
	if (descriptors[0].revents & (POLLERR | POLLHUP))
		return -ENOTCONN;
	if (descriptors[0].revents & POLLIN) {
		ret = consume_dispatcher_signal(phase, &notifications);
		if (ret != 0)
			return ret;
		phase->stats.dispatcher_notifications_consumed += notifications;
	}
	if (descriptor_count == 2U) {
		if (descriptors[1].revents & POLLNVAL)
			return -EBADF;
		if (descriptors[1].revents & (POLLERR | POLLHUP))
			return -ENOTCONN;
	}
	return 0;
}

static int wait_for_transport_or_worker(struct thread_phase *phase)
{
	short events = 0;
	int transport_fd = -1;
	int ret;

	pthread_mutex_lock(&phase->lock);
	if (phase->error != 0 || phase->completed == phase->total) {
		ret = phase->error;
		pthread_mutex_unlock(&phase->lock);
		return ret;
	}
	if (phase->in_flight > 0U)
		events |= POLLIN;
	if (phase->ready > 0U)
		events |= POLLOUT;
	pthread_mutex_unlock(&phase->lock);

	if (events != 0) {
		/*
		 * This benchmark is the handle's only receiver, so raw ring readiness
		 * can share one poll call with the worker eventfd.
		 */
		transport_fd = sg2002_rtos_poll_fd(phase->rtos);
		if (transport_fd < 0)
			return transport_fd;
	}
	return poll_transport_or_worker(
		phase, transport_fd, events, SG2002_RTOS_DEFAULT_TIMEOUT_MS);
}

static int dispatch_phase(struct thread_phase *phase,
	uint32_t echo_data_bytes)
{
	int progressed;
	int ret;

	for (;;) {
		pthread_mutex_lock(&phase->lock);
		if (phase->error != 0 || phase->completed == phase->total) {
			ret = phase->error;
			pthread_mutex_unlock(&phase->lock);
			return ret;
		}
		pthread_mutex_unlock(&phase->lock);

		progressed = 0;
		ret = reap_ready_responses(phase, echo_data_bytes, &progressed);
		if (ret)
			return ret;
		ret = submit_ready_workers(phase, &progressed);
		if (ret)
			return ret;
		if (!progressed) {
			ret = wait_for_transport_or_worker(phase);
			if (ret)
				return ret;
		}
	}
}

static int run_phase(struct sg2002_rtos *rtos, uint32_t workers,
	uint32_t total, uint32_t echo_data_bytes, uint32_t seed_base,
	uint64_t *latency, struct phase_stats *stats, uint64_t *latency_sum,
	uint64_t *duration_ns, uint32_t *completed_out)
{
	struct thread_phase phase = {0};
	struct timespec start;
	struct timespec end;
	uint32_t created = 0U;
	uint32_t index;
	int ret;

	memset(stats, 0, sizeof(*stats));
	*latency_sum = 0U;
	*duration_ns = 0U;
	*completed_out = 0U;
	phase.rtos = rtos;
	phase.wake_fd = -1;
	phase.worker_count = workers;
	phase.total = total;
	phase.seed_base = seed_base;
	phase.echo_data_bytes = echo_data_bytes;
	phase.latency = latency;
	ret = pthread_mutex_init(&phase.lock, NULL);
	if (ret != 0)
		return -ret;
	ret = pthread_cond_init(&phase.dispatcher, NULL);
	if (ret != 0) {
		pthread_mutex_destroy(&phase.lock);
		return -ret;
	}
	ret = pthread_cond_init(&phase.start_gate, NULL);
	if (ret != 0) {
		pthread_cond_destroy(&phase.dispatcher);
		pthread_mutex_destroy(&phase.lock);
		return -ret;
	}
	phase.wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	if (phase.wake_fd < 0) {
		ret = -errno;
		pthread_cond_destroy(&phase.start_gate);
		pthread_cond_destroy(&phase.dispatcher);
		pthread_mutex_destroy(&phase.lock);
		return ret;
	}

	for (index = 0U; index < workers; index++) {
		struct worker *worker = &phase.workers[index];

		worker->phase = &phase;
		worker->id = index;
		worker->state = WORKER_STARTING;
		worker->target = worker_target(total, workers, index);
		ret = pthread_cond_init(&worker->completion, NULL);
		if (ret != 0) {
			ret = -ret;
			goto out_stop;
		}
		ret = pthread_create(&worker->thread, NULL, worker_main, worker);
		if (ret != 0) {
			pthread_cond_destroy(&worker->completion);
			ret = -ret;
			goto out_stop;
		}
		created++;
	}

	pthread_mutex_lock(&phase.lock);
	while (phase.started < workers && phase.error == 0)
		pthread_cond_wait(&phase.dispatcher, &phase.lock);
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &start) != 0) {
		ret = -errno;
		fail_phase_locked(&phase, ret);
		pthread_mutex_unlock(&phase.lock);
		goto out_join;
	}
	phase.go = 1;
	pthread_cond_broadcast(&phase.start_gate);
	pthread_mutex_unlock(&phase.lock);

	ret = dispatch_phase(&phase, echo_data_bytes);
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &end) != 0) {
		if (ret == 0)
			ret = -errno;
		*duration_ns = 0U;
	} else {
		*duration_ns = elapsed_nanoseconds(&start, &end);
	}
	if (ret != 0) {
		pthread_mutex_lock(&phase.lock);
		fail_phase_locked(&phase, ret);
		pthread_mutex_unlock(&phase.lock);
	}
out_join:
	for (index = 0U; index < created; index++)
		pthread_join(phase.workers[index].thread, NULL);
	goto out_destroy;

out_stop:
	phase.worker_count = created;
	pthread_mutex_lock(&phase.lock);
	fail_phase_locked(&phase, ret);
	pthread_mutex_unlock(&phase.lock);
	for (index = 0U; index < created; index++)
		pthread_join(phase.workers[index].thread, NULL);

out_destroy:
	for (index = 0U; index < created; index++)
		pthread_cond_destroy(&phase.workers[index].completion);
	pthread_cond_destroy(&phase.start_gate);
	pthread_cond_destroy(&phase.dispatcher);
	pthread_mutex_destroy(&phase.lock);
	close(phase.wake_fd);
	*stats = phase.stats;
	*latency_sum = phase.latency_sum;
	*completed_out = phase.completed;
	if (ret == 0 && phase.completed != total)
		ret = -EIO;
	return ret;
}

static int run_self_test(void)
{
	struct sg2002_rtos_shm_slot message;
	struct thread_phase phase = {0};
	struct phase_stats startup_stats;
	uint64_t latency[] = {5U, 1U, 4U, 2U, 3U};
	uint64_t startup_latency_sum;
	uint64_t startup_duration;
	uint8_t seen[66];
	uint32_t startup_completed;
	uint32_t workers;
	uint32_t total;
	uint32_t worker;
	uint32_t operation;
	uint32_t index;
	int startup_result;
	int transport_fd;
	int wake_result;

	for (workers = 1U; workers <= MAX_WORKERS; workers++) {
		for (total = 0U; total <= 65U; total++) {
			memset(seen, 0, sizeof(seen));
			index = 0U;
			for (worker = 0U; worker < workers; worker++) {
				uint32_t target = worker_target(total, workers,
							worker);

				index += target;
				for (operation = 0U; operation < target;
				     operation++) {
					uint32_t assigned = operation * workers +
							    worker;

					if (assigned >= total || seen[assigned] != 0U)
						return 1;
					seen[assigned] = 1U;
				}
			}
			if (index != total)
				return 1;
			for (index = 0U; index < total; index++) {
				if (seen[index] != 1U)
					return 1;
			}
		}
	}

	memset(&message, 0, sizeof(message));
	message.sequence = 7U;
	message.length = build_request(message.payload,
		DEFAULT_ECHO_DATA_BYTES, 0x12345678U);
	if (validate_response(&message, DEFAULT_ECHO_DATA_BYTES,
			      0x12345678U) != 0)
		return 1;
	message.payload[DEFAULT_ECHO_DATA_BYTES] ^= 1U;
	if (validate_response(&message, DEFAULT_ECHO_DATA_BYTES,
			      0x12345678U) != -EPROTO)
		return 1;

	phase.worker_count = 2U;
	phase.workers[0].state = WORKER_IN_FLIGHT;
	phase.workers[0].sequence = 17U;
	phase.workers[1].state = WORKER_IN_FLIGHT;
	phase.workers[1].sequence = 19U;
	if (find_in_flight_worker(&phase, 17U) != &phase.workers[0] ||
	    find_in_flight_worker(&phase, 19U) != &phase.workers[1] ||
	    find_in_flight_worker(&phase, 23U) != NULL)
		return 1;

	qsort(latency, 5U, sizeof(latency[0]), compare_u64);
	if (percentile(latency, 5U, 50U) != 3U ||
	    percentile(latency, 5U, 95U) != 5U)
		return 1;
	startup_result = run_phase(NULL, MAX_WORKERS, 0U,
		DEFAULT_ECHO_DATA_BYTES, 0x2468ace0U, NULL, &startup_stats,
		&startup_latency_sum, &startup_duration, &startup_completed);
	if (startup_result != 0 || startup_completed != 0U) {
		fprintf(stderr, "startup self-test failed: ret=%d completed=%u\n",
			startup_result, startup_completed);
		return 1;
	}

	memset(&phase, 0, sizeof(phase));
	phase.wake_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	if (phase.wake_fd < 0)
		return 1;
	transport_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
	if (transport_fd < 0) {
		close(phase.wake_fd);
		return 1;
	}
	/* Model READY arriving after a POLLIN-only dispatcher state snapshot. */
	if (signal_dispatcher(&phase) != 0) {
		close(transport_fd);
		close(phase.wake_fd);
		return 1;
	}
	wake_result = poll_transport_or_worker(
		&phase, transport_fd, POLLIN, 0);
	close(transport_fd);
	close(phase.wake_fd);
	if (wake_result != 0 || phase.stats.poll_calls != 1U ||
	    phase.stats.poll_wakeups != 1U ||
	    phase.stats.dispatcher_notifications_consumed != 1U) {
		fprintf(stderr,
			"worker wake self-test failed: ret=%d polls=%" PRIu64
			" wakeups=%" PRIu64 " notifications=%" PRIu64 "\n",
			wake_result, phase.stats.poll_calls,
			phase.stats.poll_wakeups,
			phase.stats.dispatcher_notifications_consumed);
		return 1;
	}
	puts("self_test=PASS");
	return 0;
}

int main(int argc, char **argv)
{
	struct sg2002_rtos rtos = SG2002_RTOS_INITIALIZER;
	struct phase_stats warmup_stats;
	struct phase_stats stats;
	uint64_t warmup_latency_sum = 0U;
	uint64_t warmup_duration = 0U;
	uint64_t latency_sum = 0U;
	uint64_t duration_ns = 0U;
	uint64_t *latency;
	uint32_t workers = DEFAULT_WORKERS;
	uint32_t samples = DEFAULT_SAMPLES;
	uint32_t warmup = DEFAULT_WARMUP;
	uint32_t echo_data_bytes = DEFAULT_ECHO_DATA_BYTES;
	uint32_t warmup_completed = 0U;
	uint32_t completed = 0U;
	uint32_t failures;
	uint32_t message_bytes;
	uint32_t application_bytes;
	double seconds;
	double transactions_per_second;
	int native_batch = -EAGAIN;
	int ret;

	if (argc == 2 && strcmp(argv[1], "--self-test") == 0)
		return run_self_test();
	if (argc > 5) {
		fprintf(stderr,
			"Usage: rtos-thread-bench [workers [samples [warmup "
			"[echo-data-bytes]]]]\n"
			"       rtos-thread-bench --self-test\n");
		return 2;
	}
	if (argc >= 2)
		workers = parse_range(argv[1], "workers", 1U, MAX_WORKERS);
	if (argc >= 3)
		samples = parse_range(argv[2], "samples", 1U, MAX_SAMPLES);
	if (argc >= 4)
		warmup = parse_range(argv[3], "warmup", 0U, MAX_SAMPLES);
	if (argc >= 5)
		echo_data_bytes = parse_range(argv[4], "echo-data-bytes", 0U,
					      MAX_ECHO_DATA_BYTES);

	latency = calloc(samples, sizeof(*latency));
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
	if (!sg2002_rtos_shared_memory_available(&rtos)) {
		fprintf(stderr, "thread benchmark requires shared memory\n");
		ret = -EOPNOTSUPP;
		goto out_close;
	}

	ret = run_phase(&rtos, workers, warmup, echo_data_bytes,
			0x13579bdfU, NULL, &warmup_stats,
			&warmup_latency_sum, &warmup_duration,
			&warmup_completed);
	if (ret || warmup_completed != warmup) {
		fprintf(stderr, "warmup failed after %u/%u: %d\n",
			warmup_completed, warmup, ret);
		goto out_close;
	}
	ret = run_phase(&rtos, workers, samples, echo_data_bytes,
			0x2468ace0U, latency, &stats, &latency_sum,
			&duration_ns, &completed);
	if (ret)
		fprintf(stderr, "benchmark failed after %u/%u: %d\n",
			completed, samples, ret);
	if (ret == 0) {
		native_batch = sg2002_rtos_native_batch_available(&rtos);
		if (native_batch < 0) {
			ret = native_batch;
			fprintf(stderr, "batch capability unresolved: %d\n", ret);
		}
	}
	failures = ret != 0 || completed != samples ? 1U : 0U;

	if (completed > 0U)
		qsort(latency, completed, sizeof(*latency), compare_u64);
	seconds = (double)duration_ns / 1000000000.0;
	transactions_per_second = seconds > 0.0 ? completed / seconds : 0.0;
	message_bytes = echo_data_bytes > 0U ?
		(echo_data_bytes + 1U) * 2U :
		SG2002_RTOS_VALUE_MESSAGE_SIZE * 2U;
	application_bytes = echo_data_bytes > 0U ?
		echo_data_bytes * 2U : VALUE_BYTES_PER_ROUND_TRIP;

	printf("{\n");
	printf("  \"path\": \"userspace_bounded_workers_%s_freertos_"
	       "round_trip\",\n", native_batch == 1 ? "batch_native" :
	       (native_batch == 0 ? "batch_compat" : "batch_unknown"));
	printf("  \"transport\": \"shared_memory\",\n");
	printf("  \"mode\": \"closed_loop_worker_threads\",\n");
	printf("  \"batch_backend\": \"%s\",\n", native_batch == 1 ?
	       "native_ioctl" : (native_batch == 0 ?
	       "single_ioctl_fallback" : "unknown"));
	printf("  \"threads\": %u,\n", workers);
	printf("  \"worker_threads\": %u,\n", workers);
	printf("  \"io_dispatchers\": 1,\n");
	printf("  \"per_worker_outstanding_limit\": 1,\n");
	printf("  \"user_queue_capacity\": %u,\n", workers);
	printf("  \"max_ready\": %u,\n", stats.max_ready);
	printf("  \"max_user_queue_depth_observed\": %u,\n",
	       stats.max_ready);
	printf("  \"max_in_flight\": %u,\n", stats.max_in_flight);
	printf("  \"max_in_flight_observed\": %u,\n",
	       stats.max_in_flight);
	printf("  \"max_outstanding_observed\": %u,\n",
	       stats.max_outstanding);
	printf("  \"queue_overflow\": 0,\n");
	printf("  \"send_eagain\": %" PRIu64 ",\n", stats.send_eagain);
	printf("  \"receive_eagain\": %" PRIu64 ",\n",
	       stats.receive_eagain);
	printf("  \"submit_eagain\": %" PRIu64 ",\n",
	       stats.send_eagain);
	printf("  \"reap_eagain\": %" PRIu64 ",\n",
	       stats.receive_eagain);
	printf("  \"poll_calls\": %" PRIu64 ",\n", stats.poll_calls);
	printf("  \"poll_wakeups\": %" PRIu64 ",\n", stats.poll_wakeups);
	printf("  \"dispatcher_eventfd_notifications_consumed\": %" PRIu64
	       ",\n", stats.dispatcher_notifications_consumed);
	printf("  \"batch_metric_scope\": "
	       "\"userspace_api_prefix_not_kernel_ioctl\",\n");
	printf("  \"api_submit_calls\": %" PRIu64 ",\n", stats.submit_calls);
	printf("  \"api_submit_successes\": %" PRIu64 ",\n",
	       stats.submit_successes);
	printf("  \"api_submitted_messages\": %" PRIu64 ",\n",
	       stats.submitted_messages);
	printf("  \"mean_api_submit_prefix\": %.3f,\n",
	       stats.submit_successes > 0U ?
	       (double)stats.submitted_messages / stats.submit_successes : 0.0);
	printf("  \"max_api_submit_prefix\": %u,\n", stats.max_submit_batch);
	printf("  \"api_reap_calls\": %" PRIu64 ",\n", stats.reap_calls);
	printf("  \"api_reap_successes\": %" PRIu64 ",\n",
	       stats.reap_successes);
	printf("  \"api_reaped_messages\": %" PRIu64 ",\n",
	       stats.reaped_messages);
	printf("  \"mean_api_reap_prefix\": %.3f,\n",
	       stats.reap_successes > 0U ?
	       (double)stats.reaped_messages / stats.reap_successes : 0.0);
	printf("  \"max_api_reap_prefix\": %u,\n", stats.max_reap_batch);
	printf("  \"submit_calls\": %" PRIu64 ",\n", stats.submit_calls);
	printf("  \"submit_successes\": %" PRIu64 ",\n",
	       stats.submit_successes);
	printf("  \"submitted_messages\": %" PRIu64 ",\n",
	       stats.submitted_messages);
	printf("  \"mean_submit_batch\": %.3f,\n",
	       stats.submit_successes > 0U ?
	       (double)stats.submitted_messages / stats.submit_successes : 0.0);
	printf("  \"max_submit_batch\": %u,\n", stats.max_submit_batch);
	printf("  \"reap_calls\": %" PRIu64 ",\n", stats.reap_calls);
	printf("  \"reap_successes\": %" PRIu64 ",\n",
	       stats.reap_successes);
	printf("  \"reaped_messages\": %" PRIu64 ",\n",
	       stats.reaped_messages);
	printf("  \"mean_reap_batch\": %.3f,\n",
	       stats.reap_successes > 0U ?
	       (double)stats.reaped_messages / stats.reap_successes : 0.0);
	printf("  \"max_reap_batch\": %u,\n", stats.max_reap_batch);
	printf("  \"clock\": \"CLOCK_MONOTONIC_RAW\",\n");
	printf("  \"transport_bytes_per_round_trip\": %u,\n",
	       SHM_BYTES_PER_ROUND_TRIP);
	printf("  \"message_bytes_per_round_trip\": %u,\n", message_bytes);
	printf("  \"application_bytes_per_round_trip\": %u,\n",
	       application_bytes);
	printf("  \"echo_data_bytes_each_direction\": %u,\n",
	       echo_data_bytes);
	printf("  \"warmup\": %u,\n", warmup);
	printf("  \"samples\": %u,\n", samples);
	printf("  \"samples_requested\": %u,\n", samples);
	printf("  \"samples_completed\": %u,\n", completed);
	printf("  \"failures\": %u,\n", failures);
	printf("  \"error\": %d,\n", ret);
	printf("  \"elapsed_ns\": %" PRIu64 ",\n", duration_ns);
	printf("  \"transactions_per_second\": %.3f,\n",
	       transactions_per_second);
	printf("  \"wire_bytes_per_second\": %.3f,\n",
	       transactions_per_second * SHM_BYTES_PER_ROUND_TRIP);
	printf("  \"payload_bytes_per_second\": %.3f,\n",
	       transactions_per_second * application_bytes);
	printf("  \"latency_scope\": \"worker_enqueue_to_completion\",\n");
	if (completed > 0U) {
		printf("  \"latency_ns\": {\"min\": %" PRIu64
		       ", \"mean\": %.3f, \"p50\": %" PRIu64
		       ", \"p95\": %" PRIu64 ", \"p99\": %" PRIu64
		       ", \"max\": %" PRIu64 "}\n",
		       latency[0], (double)latency_sum / completed,
		       percentile(latency, completed, 50U),
		       percentile(latency, completed, 95U),
		       percentile(latency, completed, 99U),
		       latency[completed - 1U]);
	} else {
		printf("  \"latency_ns\": null\n");
	}
	printf("}\n");

out_close:
	sg2002_rtos_close(&rtos);
	free(latency);
	return ret == 0 && completed == samples ? 0 : 1;
}
