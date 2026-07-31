#pragma once

#include <stddef.h>

#include "sg2002_rtos_shm.h"

enum sg2002_rtos_ring_result {
	SG2002_RTOS_RING_OK = 0,
	SG2002_RTOS_RING_EMPTY = 1,
	SG2002_RTOS_RING_FULL = 2,
	SG2002_RTOS_RING_CORRUPT = 3,
	SG2002_RTOS_RING_INVALID = 4,
};

struct sg2002_rtos_ring_cache_ops {
	void (*sync_for_cpu)(void *context, const void *address, size_t size);
	void (*sync_for_device)(void *context, const void *address, size_t size);
	void (*memory_barrier)(void *context);
	void *context;
};

struct sg2002_rtos_ring {
	volatile struct sg2002_rtos_shm_counter_line *producer;
	volatile struct sg2002_rtos_shm_counter_line *consumer;
	volatile struct sg2002_rtos_shm_slot *slots;
	const struct sg2002_rtos_ring_cache_ops *cache_ops;
};

void sg2002_rtos_ring_init(
	struct sg2002_rtos_ring *ring,
	volatile struct sg2002_rtos_shm_counter_line *producer,
	volatile struct sg2002_rtos_shm_counter_line *consumer,
	volatile struct sg2002_rtos_shm_slot *slots,
	const struct sg2002_rtos_ring_cache_ops *cache_ops);

int sg2002_rtos_ring_can_push(struct sg2002_rtos_ring *ring);

enum sg2002_rtos_ring_result sg2002_rtos_ring_try_push(
	struct sg2002_rtos_ring *ring,
	const struct sg2002_rtos_shm_slot *message);

enum sg2002_rtos_ring_result sg2002_rtos_ring_try_pop(
	struct sg2002_rtos_ring *ring,
	struct sg2002_rtos_shm_slot *message);
