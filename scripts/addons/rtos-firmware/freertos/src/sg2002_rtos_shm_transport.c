#include "sg2002_rtos_shm_transport.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arch_helpers.h"
#include "memmap.h"
#include "sg2002_rtos_app.h"
#include "sg2002_rtos_ring.h"
#include "sg2002_rtos_shm.h"

static volatile struct sg2002_rtos_shm_control *const shm_control =
	(volatile struct sg2002_rtos_shm_control *)(uintptr_t)
		CVIMMAP_RTOS_SHM_ADDR;
static volatile struct sg2002_rtos_shm_slot *const request_slots =
	(volatile struct sg2002_rtos_shm_slot *)(uintptr_t)
		(CVIMMAP_RTOS_SHM_ADDR + SG2002_RTOS_SHM_REQUEST_OFFSET);
static volatile struct sg2002_rtos_shm_slot *const response_slots =
	(volatile struct sg2002_rtos_shm_slot *)(uintptr_t)
		(CVIMMAP_RTOS_SHM_ADDR + SG2002_RTOS_SHM_RESPONSE_OFFSET);

static struct sg2002_rtos_ring request_ring;
static struct sg2002_rtos_ring response_ring;
static struct sg2002_rtos_shm_slot request_message;
static struct sg2002_rtos_shm_slot response_message;
static uint32_t active_generation;

static void sync_for_cpu(void *context, const void *address, size_t size)
{
	(void)context;
	inv_dcache_range((uintptr_t)address, size);
}

static void sync_for_device(void *context, const void *address, size_t size)
{
	(void)context;
	clean_dcache_range((uintptr_t)address, size);
}

static void memory_barrier(void *context)
{
	(void)context;
#ifdef __riscv
	__asm__ volatile("fence rw, rw" ::: "memory");
#else
	__sync_synchronize();
#endif
}

static const struct sg2002_rtos_ring_cache_ops cache_ops = {
	.sync_for_cpu = sync_for_cpu,
	.sync_for_device = sync_for_device,
	.memory_barrier = memory_barrier,
};

static void sync_control_for_cpu(const volatile void *address, size_t size)
{
	inv_dcache_range((uintptr_t)address, size);
	memory_barrier(NULL);
}

static void publish_rtos_state(uint32_t state, uint32_t generation)
{
	shm_control->rtos_peer.generation = generation;
	shm_control->rtos_peer.state = state;
	memory_barrier(NULL);
	clean_dcache_range((uintptr_t)&shm_control->rtos_peer,
			   sizeof(shm_control->rtos_peer));
	memory_barrier(NULL);
}

static int layout_is_valid(uint32_t generation)
{
	return shm_control->layout.magic == SG2002_RTOS_SHM_MAGIC &&
	       shm_control->layout.abi_version == SG2002_RTOS_SHM_ABI_VERSION &&
	       shm_control->layout.control_size == SG2002_RTOS_SHM_CONTROL_SIZE &&
	       shm_control->layout.region_size == SG2002_RTOS_SHM_REGION_SIZE &&
	       shm_control->layout.slot_size == SG2002_RTOS_SHM_SLOT_SIZE &&
	       shm_control->layout.slot_count == SG2002_RTOS_SHM_SLOT_COUNT &&
	       shm_control->layout.request_offset ==
		       SG2002_RTOS_SHM_REQUEST_OFFSET &&
	       shm_control->layout.response_offset ==
		       SG2002_RTOS_SHM_RESPONSE_OFFSET &&
	       shm_control->layout.generation == generation &&
	       (shm_control->layout.features & SG2002_RTOS_SHM_FEATURES) ==
		       SG2002_RTOS_SHM_FEATURES;
}

static int attach(void)
{
	uint32_t generation;

	sync_control_for_cpu(&shm_control->layout,
			     sizeof(shm_control->layout));
	sync_control_for_cpu(&shm_control->linux_peer,
			     sizeof(shm_control->linux_peer));
	generation = shm_control->layout.generation;
	if (generation == 0U || !layout_is_valid(generation) ||
	    shm_control->linux_peer.state != SG2002_RTOS_SHM_STATE_READY ||
	    shm_control->linux_peer.generation != generation) {
		return 0;
	}

	if (active_generation == generation)
		return 1;

	sg2002_rtos_ring_init(&request_ring,
			       &shm_control->request_producer,
			       &shm_control->request_consumer,
			       request_slots, &cache_ops);
	sg2002_rtos_ring_init(&response_ring,
			       &shm_control->response_producer,
			       &shm_control->response_consumer,
			       response_slots, &cache_ops);
	active_generation = generation;
	publish_rtos_state(SG2002_RTOS_SHM_STATE_READY, generation);
	return 1;
}

static enum sg2002_rtos_shm_process_result mark_corrupt(void)
{
	publish_rtos_state(SG2002_RTOS_SHM_STATE_ERROR, active_generation);
	return SG2002_RTOS_SHM_PROCESS_CORRUPT;
}

void sg2002_rtos_shm_transport_init(void)
{
	active_generation = 0U;
}

enum sg2002_rtos_shm_process_result sg2002_rtos_shm_process(
	unsigned int *processed)
{
	enum sg2002_rtos_app_result app_result;
	enum sg2002_rtos_ring_result ring_result;
	unsigned int count = 0U;
	uint16_t response_length;
	int can_push;

	if (processed != NULL)
		*processed = 0U;
	if (!attach())
		return SG2002_RTOS_SHM_PROCESS_NOT_READY;

	while (count < SG2002_RTOS_SHM_SLOT_COUNT) {
		can_push = sg2002_rtos_ring_can_push(&response_ring);
		if (can_push < 0)
			return mark_corrupt();
		if (can_push == 0)
			break;

		ring_result = sg2002_rtos_ring_try_pop(&request_ring,
						      &request_message);
		if (ring_result == SG2002_RTOS_RING_EMPTY)
			break;
		if (ring_result != SG2002_RTOS_RING_OK)
			return mark_corrupt();

		memset(&response_message, 0, sizeof(response_message));
		response_message.sequence = request_message.sequence;
		response_length = SG2002_RTOS_SHM_PAYLOAD_SIZE;
		app_result = sg2002_rtos_app_handle_message(
			request_message.payload, request_message.length,
			response_message.payload, &response_length);
		if (app_result != SG2002_RTOS_APP_HANDLED)
			response_length = 0U;
		response_message.length = response_length;

		ring_result = sg2002_rtos_ring_try_push(&response_ring,
						       &response_message);
		if (ring_result != SG2002_RTOS_RING_OK)
			return mark_corrupt();
		count++;
	}

	if (processed != NULL)
		*processed = count;
	return SG2002_RTOS_SHM_PROCESS_OK;
}

uint32_t sg2002_rtos_shm_generation(void)
{
	return active_generation;
}
