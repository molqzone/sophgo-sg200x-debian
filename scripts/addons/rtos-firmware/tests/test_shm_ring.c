#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#include "sg2002_rtos_ring.h"

static unsigned int cpu_sync_count;
static unsigned int device_sync_count;
static unsigned int barrier_count;

#define CONCURRENT_MESSAGES 100000U

struct concurrent_test {
	struct sg2002_rtos_ring producer_ring;
	struct sg2002_rtos_ring consumer_ring;
};

static void sync_for_cpu(void *context, const void *address, size_t size)
{
	(void)context;
	assert(address != NULL);
	assert(size != 0U);
	__atomic_add_fetch(&cpu_sync_count, 1U, __ATOMIC_RELAXED);
}

static void sync_for_device(void *context, const void *address, size_t size)
{
	(void)context;
	assert(address != NULL);
	assert(size != 0U);
	__atomic_add_fetch(&device_sync_count, 1U, __ATOMIC_RELAXED);
}

static void memory_barrier(void *context)
{
	(void)context;
	__sync_synchronize();
	__atomic_add_fetch(&barrier_count, 1U, __ATOMIC_RELAXED);
}

static void fill_message(struct sg2002_rtos_shm_slot *message,
			 uint32_t sequence)
{
	memset(message, 0, sizeof(*message));
	message->sequence = sequence;
	message->length = 4U;
	memcpy(message->payload, &sequence, sizeof(sequence));
}

static void *produce_messages(void *argument)
{
	struct concurrent_test *test = argument;
	struct sg2002_rtos_shm_slot message;
	unsigned int sequence;

	for (sequence = 1U; sequence <= CONCURRENT_MESSAGES; sequence++) {
		for (;;) {
			enum sg2002_rtos_ring_result result;

			fill_message(&message, sequence);
			result = sg2002_rtos_ring_try_push(
				&test->producer_ring, &message);
			if (result == SG2002_RTOS_RING_OK)
				break;
			assert(result == SG2002_RTOS_RING_FULL);
			sched_yield();
		}
	}
	return NULL;
}

static void *consume_messages(void *argument)
{
	struct concurrent_test *test = argument;
	struct sg2002_rtos_shm_slot message;
	uint32_t payload;
	unsigned int expected;

	for (expected = 1U; expected <= CONCURRENT_MESSAGES; expected++) {
		for (;;) {
			enum sg2002_rtos_ring_result result;

			result = sg2002_rtos_ring_try_pop(
				&test->consumer_ring, &message);
			if (result == SG2002_RTOS_RING_OK)
				break;
			assert(result == SG2002_RTOS_RING_EMPTY);
			sched_yield();
		}
		assert(message.sequence == expected);
		assert(message.length == sizeof(payload));
		memcpy(&payload, message.payload, sizeof(payload));
		assert(payload == expected);
	}
	return NULL;
}

int main(void)
{
	static _Alignas(SG2002_RTOS_SHM_CACHE_LINE_SIZE)
		struct sg2002_rtos_shm_control control;
	static _Alignas(SG2002_RTOS_SHM_CACHE_LINE_SIZE)
		struct sg2002_rtos_shm_slot slots[SG2002_RTOS_SHM_SLOT_COUNT];
	static _Alignas(SG2002_RTOS_SHM_CACHE_LINE_SIZE)
		struct sg2002_rtos_shm_control concurrent_control;
	static _Alignas(SG2002_RTOS_SHM_CACHE_LINE_SIZE)
		struct sg2002_rtos_shm_slot
			concurrent_slots[SG2002_RTOS_SHM_SLOT_COUNT];
	const struct sg2002_rtos_ring_cache_ops cache_ops = {
		.sync_for_cpu = sync_for_cpu,
		.sync_for_device = sync_for_device,
		.memory_barrier = memory_barrier,
	};
	struct sg2002_rtos_ring ring;
	struct concurrent_test concurrent;
	struct sg2002_rtos_shm_slot message;
	struct sg2002_rtos_shm_slot received;
	pthread_t producer_thread;
	pthread_t consumer_thread;
	uint32_t value;
	unsigned int i;

	assert(SG2002_RTOS_SHM_REGION_SIZE == 0x11000U);
	assert(SG2002_RTOS_SHM_RESPONSE_OFFSET == 0x9000U);
	assert(sizeof(message) == 512U);

	sg2002_rtos_ring_init(&ring, &control.request_producer,
			       &control.request_consumer, slots, &cache_ops);
	assert(sg2002_rtos_ring_try_pop(&ring, &received) ==
	       SG2002_RTOS_RING_EMPTY);

	for (i = 0; i < SG2002_RTOS_SHM_SLOT_COUNT; i++) {
		fill_message(&message, i + 1U);
		assert(sg2002_rtos_ring_try_push(&ring, &message) ==
		       SG2002_RTOS_RING_OK);
	}
	assert(sg2002_rtos_ring_can_push(&ring) == 0);
	assert(sg2002_rtos_ring_try_push(&ring, &message) ==
	       SG2002_RTOS_RING_FULL);

	for (i = 0; i < SG2002_RTOS_SHM_SLOT_COUNT; i++) {
		assert(sg2002_rtos_ring_try_pop(&ring, &received) ==
		       SG2002_RTOS_RING_OK);
		assert(received.sequence == i + 1U);
		memcpy(&value, received.payload, sizeof(value));
		assert(value == i + 1U);
	}
	assert(control.request_producer.value == SG2002_RTOS_SHM_SLOT_COUNT);
	assert(control.request_consumer.value == SG2002_RTOS_SHM_SLOT_COUNT);

	control.request_producer.value = UINT32_MAX - 31U;
	control.request_consumer.value = UINT32_MAX - 31U;
	for (i = 0; i < SG2002_RTOS_SHM_SLOT_COUNT; i++) {
		fill_message(&message, 0x1000U + i);
		assert(sg2002_rtos_ring_try_push(&ring, &message) ==
		       SG2002_RTOS_RING_OK);
	}
	for (i = 0; i < SG2002_RTOS_SHM_SLOT_COUNT; i++) {
		assert(sg2002_rtos_ring_try_pop(&ring, &received) ==
		       SG2002_RTOS_RING_OK);
		assert(received.sequence == 0x1000U + i);
	}
	assert(control.request_producer.value == 32U);
	assert(control.request_consumer.value == 32U);

	control.request_producer.value = SG2002_RTOS_SHM_SLOT_COUNT + 1U;
	control.request_consumer.value = 0U;
	assert(sg2002_rtos_ring_can_push(&ring) ==
	       -(int)SG2002_RTOS_RING_CORRUPT);

	sg2002_rtos_ring_init(
		&concurrent.producer_ring,
		&concurrent_control.request_producer,
		&concurrent_control.request_consumer, concurrent_slots,
		&cache_ops);
	sg2002_rtos_ring_init(
		&concurrent.consumer_ring,
		&concurrent_control.request_producer,
		&concurrent_control.request_consumer, concurrent_slots,
		&cache_ops);
	assert(pthread_create(&producer_thread, NULL, produce_messages,
			      &concurrent) == 0);
	assert(pthread_create(&consumer_thread, NULL, consume_messages,
			      &concurrent) == 0);
	assert(pthread_join(producer_thread, NULL) == 0);
	assert(pthread_join(consumer_thread, NULL) == 0);
	assert(concurrent_control.request_producer.value ==
	       CONCURRENT_MESSAGES);
	assert(concurrent_control.request_consumer.value ==
	       CONCURRENT_MESSAGES);

	assert(cpu_sync_count > 0U);
	assert(device_sync_count > 0U);
	assert(barrier_count > 0U);
	return 0;
}
