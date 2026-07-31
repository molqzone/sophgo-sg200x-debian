#include "sg2002_rtos_ring.h"

#include <string.h>

static void sync_for_cpu(struct sg2002_rtos_ring *ring,
			 const volatile void *address, size_t size)
{
	if (ring->cache_ops != NULL &&
	    ring->cache_ops->sync_for_cpu != NULL) {
		ring->cache_ops->sync_for_cpu(
			ring->cache_ops->context, (const void *)address, size);
	}
}

static void sync_for_device(struct sg2002_rtos_ring *ring,
			    const volatile void *address, size_t size)
{
	if (ring->cache_ops != NULL &&
	    ring->cache_ops->sync_for_device != NULL) {
		ring->cache_ops->sync_for_device(
			ring->cache_ops->context, (const void *)address, size);
	}
}

static void memory_barrier(struct sg2002_rtos_ring *ring)
{
	if (ring->cache_ops != NULL &&
	    ring->cache_ops->memory_barrier != NULL) {
		ring->cache_ops->memory_barrier(ring->cache_ops->context);
	}
}

static enum sg2002_rtos_ring_result read_occupancy(
	struct sg2002_rtos_ring *ring, sg2002_rtos_u32 *producer,
	sg2002_rtos_u32 *consumer, sg2002_rtos_u32 *occupancy)
{
	if (ring == NULL || ring->producer == NULL || ring->consumer == NULL ||
	    ring->slots == NULL || producer == NULL || consumer == NULL ||
	    occupancy == NULL) {
		return SG2002_RTOS_RING_INVALID;
	}

	sync_for_cpu(ring, ring->producer, sizeof(*ring->producer));
	sync_for_cpu(ring, ring->consumer, sizeof(*ring->consumer));
	memory_barrier(ring);
	*producer = ring->producer->value;
	*consumer = ring->consumer->value;
	*occupancy = *producer - *consumer;
	if (*occupancy > SG2002_RTOS_SHM_SLOT_COUNT)
		return SG2002_RTOS_RING_CORRUPT;
	return SG2002_RTOS_RING_OK;
}

void sg2002_rtos_ring_init(
	struct sg2002_rtos_ring *ring,
	volatile struct sg2002_rtos_shm_counter_line *producer,
	volatile struct sg2002_rtos_shm_counter_line *consumer,
	volatile struct sg2002_rtos_shm_slot *slots,
	const struct sg2002_rtos_ring_cache_ops *cache_ops)
{
	if (ring == NULL)
		return;
	ring->producer = producer;
	ring->consumer = consumer;
	ring->slots = slots;
	ring->cache_ops = cache_ops;
}

int sg2002_rtos_ring_can_push(struct sg2002_rtos_ring *ring)
{
	sg2002_rtos_u32 producer;
	sg2002_rtos_u32 consumer;
	sg2002_rtos_u32 occupancy;
	enum sg2002_rtos_ring_result result;

	result = read_occupancy(ring, &producer, &consumer, &occupancy);
	if (result != SG2002_RTOS_RING_OK)
		return -(int)result;
	return occupancy < SG2002_RTOS_SHM_SLOT_COUNT;
}

enum sg2002_rtos_ring_result sg2002_rtos_ring_try_push(
	struct sg2002_rtos_ring *ring,
	const struct sg2002_rtos_shm_slot *message)
{
	volatile struct sg2002_rtos_shm_slot *slot;
	sg2002_rtos_u32 producer;
	sg2002_rtos_u32 consumer;
	sg2002_rtos_u32 occupancy;
	enum sg2002_rtos_ring_result result;

	if (message == NULL || message->length > SG2002_RTOS_SHM_PAYLOAD_SIZE)
		return SG2002_RTOS_RING_INVALID;
	result = read_occupancy(ring, &producer, &consumer, &occupancy);
	if (result != SG2002_RTOS_RING_OK)
		return result;
	if (occupancy == SG2002_RTOS_SHM_SLOT_COUNT)
		return SG2002_RTOS_RING_FULL;

	slot = ring->slots + (producer & (SG2002_RTOS_SHM_SLOT_COUNT - 1U));
	memcpy((void *)slot, message, sizeof(*message));
	sync_for_device(ring, slot, sizeof(*slot));
	memory_barrier(ring);
	ring->producer->value = producer + 1U;
	sync_for_device(ring, ring->producer, sizeof(*ring->producer));
	memory_barrier(ring);
	return SG2002_RTOS_RING_OK;
}

enum sg2002_rtos_ring_result sg2002_rtos_ring_try_pop(
	struct sg2002_rtos_ring *ring,
	struct sg2002_rtos_shm_slot *message)
{
	volatile struct sg2002_rtos_shm_slot *slot;
	sg2002_rtos_u32 producer;
	sg2002_rtos_u32 consumer;
	sg2002_rtos_u32 occupancy;
	enum sg2002_rtos_ring_result result;

	if (message == NULL)
		return SG2002_RTOS_RING_INVALID;
	result = read_occupancy(ring, &producer, &consumer, &occupancy);
	if (result != SG2002_RTOS_RING_OK)
		return result;
	if (occupancy == 0U)
		return SG2002_RTOS_RING_EMPTY;

	slot = ring->slots + (consumer & (SG2002_RTOS_SHM_SLOT_COUNT - 1U));
	sync_for_cpu(ring, slot, sizeof(*slot));
	memory_barrier(ring);
	memcpy(message, (const void *)slot, sizeof(*message));
	memory_barrier(ring);
	ring->consumer->value = consumer + 1U;
	sync_for_device(ring, ring->consumer, sizeof(*ring->consumer));
	memory_barrier(ring);
	if (message->length > SG2002_RTOS_SHM_PAYLOAD_SIZE)
		return SG2002_RTOS_RING_CORRUPT;
	return SG2002_RTOS_RING_OK;
}
