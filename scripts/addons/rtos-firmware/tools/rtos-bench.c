#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "sg2002_rtos.h"

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

#define DEFAULT_SAMPLES 2000U
#define DEFAULT_WARMUP 100U
#define MAX_SAMPLES 1000000U
#define CMDQU_WIRE_BYTES_PER_ROUND_TRIP 16U
#define CMDQU_PAYLOAD_BYTES_PER_ROUND_TRIP 8U

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
	uint32_t completed = 0;
	uint32_t failed = 0;
	uint32_t i;
	uint32_t result = 0;
	double seconds;
	double transactions_per_second;
	int ret;

	if (argc > 3) {
		fprintf(stderr, "Usage: rtos-bench [samples [warmup]]\n");
		return 2;
	}
	if (argc >= 2)
		requested = parse_count(argv[1]);
	if (argc == 3)
		warmup = parse_count(argv[2]);

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

	for (i = 0; i < warmup; i++) {
		uint32_t input = 0x13579bdfU + i;

		ret = sg2002_rtos_ping(&rtos, input, &result);
		if (ret || result != (input ^ SG2002_RTOS_PING_XOR)) {
			fprintf(stderr, "warmup failed at %u: ret=%d result=0x%08x\n",
				i, ret, result);
			sg2002_rtos_close(&rtos);
			free(latency);
			return 1;
		}
	}

	clock_gettime(CLOCK_MONOTONIC_RAW, &total_start);
	for (i = 0; i < requested; i++) {
		uint32_t input = 0x2468ace0U + i;

		clock_gettime(CLOCK_MONOTONIC_RAW, &start);
		ret = sg2002_rtos_ping(&rtos, input, &result);
		clock_gettime(CLOCK_MONOTONIC_RAW, &end);
		if (ret || result != (input ^ SG2002_RTOS_PING_XOR)) {
			failed++;
			break;
		}
		latency[completed] = elapsed_ns(&start, &end);
		latency_sum += latency[completed];
		completed++;
	}
	clock_gettime(CLOCK_MONOTONIC_RAW, &total_end);
	sg2002_rtos_close(&rtos);

	total_ns = elapsed_ns(&total_start, &total_end);
	seconds = (double)total_ns / 1000000000.0;
	transactions_per_second = seconds > 0.0 ? completed / seconds : 0.0;
	if (completed > 0)
		qsort(latency, completed, sizeof(*latency), compare_u64);

	printf("{\n");
	printf("  \"path\": \"userspace_ioctl_cmdqu_freertos_round_trip\",\n");
	printf("  \"clock\": \"CLOCK_MONOTONIC_RAW\",\n");
	printf("  \"mailbox_slot_bytes_each_direction\": 8,\n");
	printf("  \"warmup\": %u,\n", warmup);
	printf("  \"samples_requested\": %u,\n", requested);
	printf("  \"samples_completed\": %u,\n", completed);
	printf("  \"failures\": %u,\n", failed);
	printf("  \"elapsed_ns\": %" PRIu64 ",\n", total_ns);
	printf("  \"transactions_per_second\": %.3f,\n",
	       transactions_per_second);
	printf("  \"wire_bytes_per_second\": %.3f,\n",
	       transactions_per_second * CMDQU_WIRE_BYTES_PER_ROUND_TRIP);
	printf("  \"payload_bytes_per_second\": %.3f,\n",
	       transactions_per_second * CMDQU_PAYLOAD_BYTES_PER_ROUND_TRIP);
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
