#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>

#include "rtos_cmdqu.h"
#include "sg2002_rtos.h"

static unsigned int ioctl_calls;
static unsigned int close_calls;
static int fake_shared_enabled;
static int fake_bad_shm_info;
static uint32_t fake_next_sequence = 1U;
static int fake_response_ready;
static struct sg2002_rtos_shm_slot fake_response;
static enum {
	FAKE_NORMAL,
	FAKE_IOCTL_ERROR,
	FAKE_BAD_DIRECTION,
	FAKE_BAD_INFO,
} fake_mode;

int __wrap_open(const char *path, int flags, ...)
{
	assert(strcmp(path, SG2002_RTOS_DEVICE_PATH) == 0);
	assert(flags != 0);
	return 42;
}

int __wrap_close(int fd)
{
	assert(fd == 42);
	close_calls++;
	return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	void *argument;
	va_list arguments;

	assert(fd == 42);
	va_start(arguments, request);
	argument = va_arg(arguments, void *);
	va_end(arguments);

	if (request == SG2002_RTOS_SHM_ACQUIRE) {
		struct sg2002_rtos_shm_info *info = argument;

		if (!fake_shared_enabled) {
			errno = ENOTTY;
			return -1;
		}
		info->abi_version = SG2002_RTOS_SHM_ABI_VERSION;
		info->region_size = SG2002_RTOS_SHM_REGION_SIZE;
		info->control_size = SG2002_RTOS_SHM_CONTROL_SIZE;
		info->slot_size = SG2002_RTOS_SHM_SLOT_SIZE;
		info->slot_count = SG2002_RTOS_SHM_SLOT_COUNT;
		info->payload_size = SG2002_RTOS_SHM_PAYLOAD_SIZE;
		info->generation = 1U;
		info->features = SG2002_RTOS_SHM_FEATURES;
		if (fake_bad_shm_info)
			info->abi_version = 0U;
		return 0;
	}
	if (request == SG2002_RTOS_SHM_SEND) {
		struct sg2002_rtos_shm_transfer *transfer = argument;
		uint32_t value;

		assert(fake_shared_enabled);
		transfer->message.sequence = fake_next_sequence++;
		fake_response = transfer->message;
		if (fake_response.length == SG2002_RTOS_VALUE_MESSAGE_SIZE) {
			memcpy(&value,
			       fake_response.payload +
				       SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
			       sizeof(value));
			if (fake_response.payload[0] == SG2002_RTOS_CMD_PING)
				value ^= SG2002_RTOS_PING_XOR;
			memcpy(fake_response.payload +
				       SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
			       &value, sizeof(value));
		}
		fake_response_ready = 1;
		return 0;
	}
	if (request == SG2002_RTOS_SHM_RECEIVE) {
		struct sg2002_rtos_shm_transfer *transfer = argument;

		assert(fake_shared_enabled);
		if (!fake_response_ready) {
			errno = EAGAIN;
			return -1;
		}
		transfer->message = fake_response;
		fake_response_ready = 0;
		return 0;
	}

	assert(request == RTOS_CMDQU_SEND_WAIT);
	{
		cmdqu_t *command = argument;

		ioctl_calls++;
		assert(command->ip_id == IP_SYSTEM);
		assert(command->block == 1);
		assert(command->resv.mstime ==
		       SG2002_RTOS_DEFAULT_TIMEOUT_MS);
		if (fake_mode == FAKE_IOCTL_ERROR) {
			errno = ETIMEDOUT;
			return -1;
		}

		switch (command->cmd_id) {
		case SG2002_RTOS_CMD_PING:
			command->param_ptr ^= SG2002_RTOS_PING_XOR;
			break;
		case SG2002_RTOS_CMD_GET_INFO:
			command->param_ptr = fake_mode == FAKE_BAD_INFO ? 0 :
				SG2002_RTOS_INFO_VALUE;
			break;
		case SG2002_RTOS_CMD_GPIO_SET:
			command->param_ptr =
				SG2002_RTOS_GPIO_VALUE(command->param_ptr);
			break;
		case SG2002_RTOS_CMD_GPIO_GET:
			command->param_ptr = 1;
			break;
		default:
			command->param_ptr = SG2002_RTOS_RESPONSE_UNSUPPORTED;
			break;
		}
		command->resv.valid.linux_valid =
			fake_mode == FAKE_BAD_DIRECTION ? 1 : 0;
		command->resv.valid.rtos_valid = 1;
		return 0;
	}
}

int main(void)
{
	struct sg2002_rtos rtos = { .fd = 42 };
	uint32_t result;
	uint32_t info;
	unsigned int calls;
	struct sg2002_rtos shared = SG2002_RTOS_INITIALIZER;
	struct sg2002_rtos legacy = SG2002_RTOS_INITIALIZER;
	struct sg2002_rtos bad = SG2002_RTOS_INITIALIZER;
	uint8_t request[SG2002_RTOS_SHM_PAYLOAD_SIZE];
	uint8_t response[SG2002_RTOS_SHM_PAYLOAD_SIZE];
	uint16_t response_length;
	uint32_t sequence;
	int value;

	assert(sg2002_rtos_ping(&rtos, 0x13579bdfU, &result) == 0);
	assert(result == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));
	assert(sg2002_rtos_get_info(&rtos, &info) == 0);
	assert(info == SG2002_RTOS_INFO_VALUE);
	assert(sg2002_rtos_gpio_set(&rtos, 0x0b03, 1) == 0);
	assert(sg2002_rtos_gpio_get(&rtos, 0x0b03, &value) == 0);
	assert(value == 1);
	fake_mode = FAKE_IOCTL_ERROR;
	assert(sg2002_rtos_ping(&rtos, 0, &result) == -ETIMEDOUT);
	fake_mode = FAKE_BAD_DIRECTION;
	assert(sg2002_rtos_ping(&rtos, 0, &result) == -EPROTO);
	fake_mode = FAKE_BAD_INFO;
	assert(sg2002_rtos_get_info(&rtos, &info) == -EPROTO);
	fake_mode = FAKE_NORMAL;

	calls = ioctl_calls;
	assert(sg2002_rtos_call(&rtos, SG2002_RTOS_CMD_FIRST - 1U, 0,
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &result) == -EINVAL);
	assert(ioctl_calls == calls);
	assert(sg2002_rtos_open(&legacy, NULL) == 0);
	assert(sg2002_rtos_shared_memory_available(&legacy) == 0);
	assert(sg2002_rtos_ping(&legacy, 0x13579bdfU, &result) == 0);
	assert(result == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));
	sg2002_rtos_close(&legacy);

	fake_shared_enabled = 1;
	fake_bad_shm_info = 1;
	assert(sg2002_rtos_open(&bad, NULL) == -EPROTO);
	assert(bad.fd == -1);
	assert(bad.private_data == NULL);
	fake_bad_shm_info = 0;
	assert(sg2002_rtos_open(&shared, NULL) == 0);
	assert(sg2002_rtos_shared_memory_available(&shared) == 1);
	assert(sg2002_rtos_ping(&shared, 0x13579bdfU, &result) == 0);
	assert(result == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));

	memset(request, 0x5a, sizeof(request));
	assert(sg2002_rtos_message_send(
		       &shared, request, sizeof(request),
		       SG2002_RTOS_DEFAULT_TIMEOUT_MS, &sequence) == 0);
	assert(sequence != 0U);
	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), &response_length,
		       &sequence, SG2002_RTOS_DEFAULT_TIMEOUT_MS) == 0);
	assert(response_length == sizeof(response));
	assert(memcmp(request, response, sizeof(request)) == 0);
	assert(sg2002_rtos_message_receive(
		       &shared, NULL, 1U, &response_length, &sequence, 0U) ==
	       -EINVAL);
	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), NULL, &sequence, 0U) ==
	       -EINVAL);
	assert(sg2002_rtos_message_call(
		       &shared, request, 1U, NULL, 1U, &response_length, 0U) ==
	       -EINVAL);
	sg2002_rtos_close(&shared);
	assert(shared.fd == -1);
	assert(close_calls == 3U);
	return 0;
}
