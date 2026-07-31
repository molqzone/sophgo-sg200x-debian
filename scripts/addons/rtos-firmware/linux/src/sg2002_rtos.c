#include "sg2002_rtos.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rtos_cmdqu.h"

typedef char sg2002_cmdqu_size_must_be_8[(sizeof(cmdqu_t) == 8) ? 1 : -1];

struct sg2002_rtos_private {
	pthread_mutex_t lock;
	int shared_memory_enabled;
	struct sg2002_rtos_shm_slot pending[SG2002_RTOS_SHM_SLOT_COUNT];
	size_t pending_count;
};

static struct sg2002_rtos_private *private_data(struct sg2002_rtos *rtos)
{
	return rtos == NULL ? NULL : rtos->private_data;
}

static int info_is_valid(const struct sg2002_rtos_shm_info *info)
{
	return info->abi_version == SG2002_RTOS_SHM_ABI_VERSION &&
	       info->region_size == SG2002_RTOS_SHM_REGION_SIZE &&
	       info->control_size == SG2002_RTOS_SHM_CONTROL_SIZE &&
	       info->slot_size == SG2002_RTOS_SHM_SLOT_SIZE &&
	       info->slot_count == SG2002_RTOS_SHM_SLOT_COUNT &&
	       info->payload_size == SG2002_RTOS_SHM_PAYLOAD_SIZE &&
	       info->generation != 0U &&
	       (info->features & SG2002_RTOS_SHM_FEATURES) ==
		       SG2002_RTOS_SHM_FEATURES;
}

int sg2002_rtos_open(struct sg2002_rtos *rtos, const char *device_path)
{
	int fd;
	int mutex_result;
	struct sg2002_rtos_private *private;
	struct sg2002_rtos_shm_info info = {0};

	if (rtos == NULL)
		return -EINVAL;
	if (rtos->fd >= 0)
		return -EALREADY;
	if (device_path == NULL)
		device_path = SG2002_RTOS_DEVICE_PATH;

	fd = open(device_path, O_RDWR | O_DSYNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, SG2002_RTOS_SHM_ACQUIRE, &info) < 0) {
		rtos->fd = fd;
		rtos->private_data = NULL;
		return 0;
	}
	if (!info_is_valid(&info)) {
		close(fd);
		return -EPROTO;
	}
	private = calloc(1, sizeof(*private));
	if (private == NULL) {
		close(fd);
		return -ENOMEM;
	}
	mutex_result = pthread_mutex_init(&private->lock, NULL);
	if (mutex_result != 0) {
		free(private);
		close(fd);
		return -mutex_result;
	}
	private->shared_memory_enabled = 1;
	rtos->fd = fd;
	rtos->private_data = private;
	return 0;
}

void sg2002_rtos_close(struct sg2002_rtos *rtos)
{
	if (rtos == NULL || rtos->fd < 0)
		return;
	close(rtos->fd);
	if (rtos->private_data != NULL) {
		struct sg2002_rtos_private *private = rtos->private_data;

		pthread_mutex_destroy(&private->lock);
		free(private);
	}
	rtos->fd = -1;
	rtos->private_data = NULL;
}

int sg2002_rtos_shared_memory_available(const struct sg2002_rtos *rtos)
{
	const struct sg2002_rtos_private *private;

	if (rtos == NULL || rtos->fd < 0)
		return 0;
	private = rtos->private_data;
	return private != NULL && private->shared_memory_enabled;
}

static int message_send_locked(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, uint32_t timeout_ms,
	uint32_t *sequence)
{
	struct sg2002_rtos_shm_transfer transfer = {0};

	if (request_length > SG2002_RTOS_SHM_PAYLOAD_SIZE ||
	    (request_length > 0U && request == NULL) || sequence == NULL)
		return -EINVAL;
	transfer.timeout_ms = timeout_ms;
	transfer.message.length = request_length;
	if (request_length > 0U)
		memcpy(transfer.message.payload, request, request_length);
	if (ioctl(rtos->fd, SG2002_RTOS_SHM_SEND, &transfer) < 0)
		return -errno;
	if (transfer.message.sequence == 0U)
		return -EPROTO;
	*sequence = transfer.message.sequence;
	return 0;
}

static int receive_from_kernel_locked(struct sg2002_rtos *rtos,
	struct sg2002_rtos_shm_slot *message, uint32_t timeout_ms)
{
	struct sg2002_rtos_shm_transfer transfer = {0};

	transfer.timeout_ms = timeout_ms;
	if (ioctl(rtos->fd, SG2002_RTOS_SHM_RECEIVE, &transfer) < 0)
		return -errno;
	if (transfer.message.sequence == 0U ||
	    transfer.message.length > SG2002_RTOS_SHM_PAYLOAD_SIZE)
		return -EPROTO;
	*message = transfer.message;
	return 0;
}

static int retain_pending(struct sg2002_rtos_private *private,
	const struct sg2002_rtos_shm_slot *message)
{
	if (private->pending_count >= SG2002_RTOS_SHM_SLOT_COUNT)
		return -ENOBUFS;
	private->pending[private->pending_count++] = *message;
	return 0;
}

static int take_pending_sequence(struct sg2002_rtos_private *private,
	uint32_t sequence, struct sg2002_rtos_shm_slot *message)
{
	size_t index;

	for (index = 0; index < private->pending_count; index++) {
		if (private->pending[index].sequence != sequence)
			continue;
		*message = private->pending[index];
		private->pending_count--;
		if (index < private->pending_count) {
			memmove(&private->pending[index],
				&private->pending[index + 1U],
				(private->pending_count - index) *
					sizeof(private->pending[0]));
		}
		return 1;
	}
	return 0;
}

static int copy_message(const struct sg2002_rtos_shm_slot *message,
	void *response, uint16_t response_capacity, uint16_t *response_length,
	uint32_t *sequence)
{
	*response_length = message->length;
	if (sequence != NULL)
		*sequence = message->sequence;
	if (message->length > response_capacity)
		return -EMSGSIZE;
	if (message->length > 0U) {
		if (response == NULL)
			return -EINVAL;
		memcpy(response, message->payload, message->length);
	}
	return 0;
}

int sg2002_rtos_message_send(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, uint32_t timeout_ms,
	uint32_t *sequence)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	ret = pthread_mutex_lock(&private->lock);
	if (ret != 0)
		return -ret;
	ret = message_send_locked(rtos, request, request_length, timeout_ms,
				  sequence);
	pthread_mutex_unlock(&private->lock);
	return ret;
}

int sg2002_rtos_message_receive(struct sg2002_rtos *rtos,
	void *response, uint16_t response_capacity, uint16_t *response_length,
	uint32_t *sequence, uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	struct sg2002_rtos_shm_slot message;
	int from_pending = 0;
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (response_length == NULL ||
	    (response_capacity > 0U && response == NULL))
		return -EINVAL;
	ret = pthread_mutex_lock(&private->lock);
	if (ret != 0)
		return -ret;
	if (private->pending_count > 0U) {
		message = private->pending[0];
		from_pending = 1;
	} else {
		ret = receive_from_kernel_locked(rtos, &message, timeout_ms);
		if (ret)
			goto out_unlock;
	}
	ret = copy_message(&message, response, response_capacity,
			   response_length, sequence);
	if (from_pending && ret != -EMSGSIZE) {
		private->pending_count--;
		if (private->pending_count > 0U) {
			memmove(&private->pending[0], &private->pending[1],
				private->pending_count *
					sizeof(private->pending[0]));
		}
	} else if (!from_pending && ret == -EMSGSIZE) {
		ret = retain_pending(private, &message) == 0 ? -EMSGSIZE :
			-ENOBUFS;
	}

out_unlock:
	pthread_mutex_unlock(&private->lock);
	return ret;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0U;
	return (uint64_t)now.tv_sec * 1000U +
	       (uint64_t)now.tv_nsec / 1000000U;
}

int sg2002_rtos_message_call(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, void *response,
	uint16_t response_capacity, uint16_t *response_length,
	uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	struct sg2002_rtos_shm_slot message;
	uint64_t deadline = 0U;
	uint64_t now;
	uint32_t remaining = timeout_ms;
	uint32_t sequence;
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (response_length == NULL ||
	    (response_capacity > 0U && response == NULL))
		return -EINVAL;
	ret = pthread_mutex_lock(&private->lock);
	if (ret != 0)
		return -ret;
	ret = message_send_locked(rtos, request, request_length, timeout_ms,
				  &sequence);
	if (ret)
		goto out_unlock;
	if (timeout_ms != 0U && timeout_ms != UINT32_MAX)
		deadline = monotonic_milliseconds() + timeout_ms;

	for (;;) {
		if (take_pending_sequence(private, sequence, &message))
			break;
		ret = receive_from_kernel_locked(rtos, &message, remaining);
		if (ret)
			goto out_unlock;
		if (message.sequence == sequence)
			break;
		ret = retain_pending(private, &message);
		if (ret)
			goto out_unlock;
		if (deadline != 0U) {
			now = monotonic_milliseconds();
			if (now >= deadline) {
				ret = -ETIMEDOUT;
				goto out_unlock;
			}
			remaining = (uint32_t)(deadline - now);
		}
	}
	ret = copy_message(&message, response, response_capacity,
			   response_length, NULL);

out_unlock:
	pthread_mutex_unlock(&private->lock);
	return ret;
}

int sg2002_rtos_call(struct sg2002_rtos *rtos, uint8_t command,
		     uint32_t request, uint16_t timeout_ms, uint32_t *response)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	uint8_t request_message[SG2002_RTOS_VALUE_MESSAGE_SIZE];
	uint8_t response_message[SG2002_RTOS_VALUE_MESSAGE_SIZE];
	uint16_t response_length;
	cmdqu_t cmdq = {0};
	int ret;

	if (rtos == NULL || rtos->fd < 0 || response == NULL || timeout_ms == 0)
		return -EINVAL;
	if (command < SG2002_RTOS_CMD_FIRST || command > SG2002_RTOS_CMD_LAST)
		return -EINVAL;
	if (private != NULL && private->shared_memory_enabled) {
		request_message[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] =
			command;
		memcpy(request_message + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       &request, sizeof(request));
		ret = sg2002_rtos_message_call(
			rtos, request_message, sizeof(request_message),
			response_message, sizeof(response_message),
			&response_length, timeout_ms);
		if (ret)
			return ret;
		if (response_length != sizeof(response_message) ||
		    response_message[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] !=
			    command) {
			return -EPROTO;
		}
		memcpy(response,
		       response_message + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       sizeof(*response));
		return 0;
	}

	cmdq.ip_id = IP_SYSTEM;
	cmdq.cmd_id = command;
	cmdq.block = 1;
	cmdq.resv.mstime = timeout_ms;
	cmdq.param_ptr = request;

	if (ioctl(rtos->fd, RTOS_CMDQU_SEND_WAIT, &cmdq) < 0)
		return -errno;
	if (cmdq.ip_id != IP_SYSTEM || cmdq.cmd_id != command ||
	    cmdq.resv.valid.linux_valid != 0 ||
	    cmdq.resv.valid.rtos_valid != 1)
		return -EPROTO;

	*response = cmdq.param_ptr;
	return 0;
}

int sg2002_rtos_get_info(struct sg2002_rtos *rtos, uint32_t *info)
{
	uint32_t response = 0;
	int ret;

	if (info == NULL)
		return -EINVAL;
	ret = sg2002_rtos_call(rtos, SG2002_RTOS_CMD_GET_INFO, 0,
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &response);
	if (ret)
		return ret;
	if (!SG2002_RTOS_INFO_IS_VALID(response))
		return -EPROTO;

	*info = response;
	return 0;
}

int sg2002_rtos_ping(struct sg2002_rtos *rtos, uint32_t input,
		     uint32_t *result)
{
	if (result == NULL)
		return -EINVAL;
	return sg2002_rtos_call(rtos, SG2002_RTOS_CMD_PING, input,
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, result);
}

int sg2002_rtos_gpio_set(struct sg2002_rtos *rtos, uint16_t pin, int value)
{
	uint32_t response = 0;
	int ret;

	if (value != 0 && value != 1)
		return -EINVAL;
	ret = sg2002_rtos_call(rtos, SG2002_RTOS_CMD_GPIO_SET,
				SG2002_RTOS_GPIO_ENCODE(pin, value),
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &response);
	if (ret)
		return ret;
	if (response == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT)
		return -EINVAL;
	return response == (uint32_t)value ? 0 : -EPROTO;
}

int sg2002_rtos_gpio_get(struct sg2002_rtos *rtos, uint16_t pin, int *value)
{
	uint32_t response = 0;
	int ret;

	if (value == NULL)
		return -EINVAL;
	ret = sg2002_rtos_call(rtos, SG2002_RTOS_CMD_GPIO_GET,
				SG2002_RTOS_GPIO_ENCODE(pin, 0),
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &response);
	if (ret)
		return ret;
	if (response == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT)
		return -EINVAL;
	if (response > 1U)
		return -EPROTO;

	*value = (int)response;
	return 0;
}
