#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sg2002_rtos_protocol.h"
#include "sg2002_rtos_ring.h"
#include "sg2002_rtos_shm.h"
#include "sg2002_rtos_shm_transport.h"

_Alignas(SG2002_RTOS_SHM_CACHE_LINE_SIZE)
unsigned char test_shm_region[SG2002_RTOS_SHM_REGION_SIZE];

void clean_dcache_range(uintptr_t address, size_t size)
{
	assert(address != 0U);
	assert(size != 0U);
}

void inv_dcache_range(uintptr_t address, size_t size)
{
	assert(address != 0U);
	assert(size != 0U);
}

int gpio_is_valid(int pin)
{
	return pin >= 0;
}

void gpio_direction_output(int pin, int value)
{
	(void)pin;
	(void)value;
}

int gpio_get_value(int pin)
{
	(void)pin;
	return 0;
}

static void initialize_linux_generation(uint32_t generation)
{
	struct sg2002_rtos_shm_control *control =
		(struct sg2002_rtos_shm_control *)test_shm_region;

	memset(test_shm_region, 0, sizeof(test_shm_region));
	control->layout.magic = SG2002_RTOS_SHM_MAGIC;
	control->layout.abi_version = SG2002_RTOS_SHM_ABI_VERSION;
	control->layout.control_size = SG2002_RTOS_SHM_CONTROL_SIZE;
	control->layout.region_size = SG2002_RTOS_SHM_REGION_SIZE;
	control->layout.slot_size = SG2002_RTOS_SHM_SLOT_SIZE;
	control->layout.slot_count = SG2002_RTOS_SHM_SLOT_COUNT;
	control->layout.request_offset = SG2002_RTOS_SHM_REQUEST_OFFSET;
	control->layout.response_offset = SG2002_RTOS_SHM_RESPONSE_OFFSET;
	control->layout.generation = generation;
	control->layout.features = SG2002_RTOS_SHM_FEATURES;
	control->linux_peer.generation = generation;
	control->linux_peer.state = SG2002_RTOS_SHM_STATE_READY;
}

static void build_ping(struct sg2002_rtos_shm_slot *message,
		       uint32_t sequence, uint32_t value)
{
	memset(message, 0, sizeof(*message));
	message->sequence = sequence;
	message->length = SG2002_RTOS_VALUE_MESSAGE_SIZE;
	message->payload[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] =
		SG2002_RTOS_CMD_PING;
	memcpy(message->payload + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
	       &value, sizeof(value));
}

int main(void)
{
	struct sg2002_rtos_shm_control *control =
		(struct sg2002_rtos_shm_control *)test_shm_region;
	struct sg2002_rtos_shm_slot *request_slots =
		(struct sg2002_rtos_shm_slot *)(test_shm_region +
					       SG2002_RTOS_SHM_REQUEST_OFFSET);
	struct sg2002_rtos_shm_slot *response_slots =
		(struct sg2002_rtos_shm_slot *)(test_shm_region +
					       SG2002_RTOS_SHM_RESPONSE_OFFSET);
	struct sg2002_rtos_ring request_ring;
	struct sg2002_rtos_ring response_ring;
	struct sg2002_rtos_shm_slot request;
	struct sg2002_rtos_shm_slot response;
	unsigned int processed;
	unsigned int index;
	uint32_t value;

	initialize_linux_generation(7U);
	sg2002_rtos_ring_init(&request_ring, &control->request_producer,
			       &control->request_consumer, request_slots, NULL);
	sg2002_rtos_ring_init(&response_ring, &control->response_producer,
			       &control->response_consumer, response_slots, NULL);
	sg2002_rtos_shm_transport_init();

	build_ping(&request, 0x12345678U, 0x13579bdfU);
	assert(sg2002_rtos_ring_try_push(&request_ring, &request) ==
	       SG2002_RTOS_RING_OK);
	assert(sg2002_rtos_shm_process(&processed) ==
	       SG2002_RTOS_SHM_PROCESS_OK);
	assert(processed == 1U);
	assert(control->rtos_peer.state == SG2002_RTOS_SHM_STATE_READY);
	assert(control->rtos_peer.generation == 7U);
	assert(sg2002_rtos_shm_generation() == 7U);
	assert(sg2002_rtos_ring_try_pop(&response_ring, &response) ==
	       SG2002_RTOS_RING_OK);
	assert(response.sequence == request.sequence);
	assert(response.length == SG2002_RTOS_VALUE_MESSAGE_SIZE);
	assert(response.payload[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] ==
	       SG2002_RTOS_CMD_PING);
	memcpy(&value,
	       response.payload + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
	       sizeof(value));
	assert(value == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));

	initialize_linux_generation(8U);
	assert(sg2002_rtos_shm_process(&processed) ==
	       SG2002_RTOS_SHM_PROCESS_OK);
	assert(processed == 0U);
	assert(control->rtos_peer.generation == 8U);
	assert(sg2002_rtos_shm_generation() == 8U);

	initialize_linux_generation(9U);
	for (index = 0U; index < SG2002_RTOS_SHM_SLOT_COUNT; index++) {
		memset(&response, 0, sizeof(response));
		response.sequence = 0x8000U + index;
		assert(sg2002_rtos_ring_try_push(&response_ring, &response) ==
		       SG2002_RTOS_RING_OK);
	}
	build_ping(&request, 0xabcdef01U, 0x2468ace0U);
	assert(sg2002_rtos_ring_try_push(&request_ring, &request) ==
	       SG2002_RTOS_RING_OK);
	assert(sg2002_rtos_shm_process(&processed) ==
	       SG2002_RTOS_SHM_PROCESS_OK);
	assert(processed == 0U);
	assert(sg2002_rtos_ring_try_pop(&response_ring, &response) ==
	       SG2002_RTOS_RING_OK);
	assert(response.sequence == 0x8000U);
	assert(sg2002_rtos_shm_process(&processed) ==
	       SG2002_RTOS_SHM_PROCESS_OK);
	assert(processed == 1U);
	for (index = 1U; index < SG2002_RTOS_SHM_SLOT_COUNT; index++) {
		assert(sg2002_rtos_ring_try_pop(&response_ring, &response) ==
		       SG2002_RTOS_RING_OK);
		assert(response.sequence == 0x8000U + index);
	}
	assert(sg2002_rtos_ring_try_pop(&response_ring, &response) ==
	       SG2002_RTOS_RING_OK);
	assert(response.sequence == request.sequence);

	memset(&request, 0x5a, sizeof(request));
	request.sequence = 0xabcdef02U;
	request.length = SG2002_RTOS_SHM_PAYLOAD_SIZE;
	request.payload[0] = SG2002_RTOS_CMD_ECHO;
	assert(sg2002_rtos_ring_try_push(&request_ring, &request) ==
	       SG2002_RTOS_RING_OK);
	assert(sg2002_rtos_shm_process(&processed) ==
	       SG2002_RTOS_SHM_PROCESS_OK);
	assert(processed == 1U);
	assert(sg2002_rtos_ring_try_pop(&response_ring, &response) ==
	       SG2002_RTOS_RING_OK);
	assert(response.sequence == request.sequence);
	assert(response.length == request.length);
	assert(memcmp(response.payload, request.payload, request.length) == 0);
	return 0;
}
