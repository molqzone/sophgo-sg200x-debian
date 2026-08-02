#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rtos_cmdqu.h"
#include "sg2002_rtos.h"

static unsigned int ioctl_calls;
static unsigned int close_calls;
static unsigned int pending_close_calls;
static int fake_shared_enabled;
static int fake_bad_shm_info;
static int fake_acquire_error;
static int fake_batch_enabled;
static unsigned int fake_batch_submit_calls;
static unsigned int fake_batch_reap_calls;
static unsigned int fake_batch_eagain_calls;
static unsigned long fake_batch_eagain_request;
static uint32_t fake_next_sequence = 1U;
static struct sg2002_rtos_shm_slot
	fake_responses[SG2002_RTOS_SHM_SLOT_COUNT];
static unsigned int fake_response_head;
static unsigned int fake_response_count;
static uint32_t fake_last_send_timeout;
static uint32_t fake_last_receive_timeout;
static pthread_mutex_t fake_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fake_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_gate = PTHREAD_COND_INITIALIZER;
static unsigned int fake_poll_calls;
static uint64_t fake_pending_events;
static int fake_poll_block;
static int fake_poll_entered;
static int fake_poll_waiting;
static int fake_send_while_polling;
static enum {
	FAKE_NORMAL,
	FAKE_IOCTL_ERROR,
	FAKE_BAD_DIRECTION,
	FAKE_BAD_INFO,
} fake_mode;

static int fake_send_slot(struct sg2002_rtos_shm_slot *message)
{
	struct sg2002_rtos_shm_slot response;
	unsigned int tail;
	uint32_t value;

	pthread_mutex_lock(&fake_queue_lock);
	if (fake_response_count == SG2002_RTOS_SHM_SLOT_COUNT) {
		pthread_mutex_unlock(&fake_queue_lock);
		errno = EAGAIN;
		return -1;
	}
	message->sequence = fake_next_sequence++;
	response = *message;
	if (response.length == SG2002_RTOS_VALUE_MESSAGE_SIZE) {
		memcpy(&value,
		       response.payload + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       sizeof(value));
		if (response.payload[0] == SG2002_RTOS_CMD_PING)
			value ^= SG2002_RTOS_PING_XOR;
		memcpy(response.payload + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       &value, sizeof(value));
	}
	tail = (fake_response_head + fake_response_count) %
	       SG2002_RTOS_SHM_SLOT_COUNT;
	fake_responses[tail] = response;
	fake_response_count++;
	pthread_mutex_unlock(&fake_queue_lock);
	return 0;
}

static int fake_receive_slot(struct sg2002_rtos_shm_slot *message)
{
	pthread_mutex_lock(&fake_queue_lock);
	if (fake_response_count == 0U) {
		pthread_mutex_unlock(&fake_queue_lock);
		errno = EAGAIN;
		return -1;
	}
	*message = fake_responses[fake_response_head];
	fake_response_head = (fake_response_head + 1U) %
			     SG2002_RTOS_SHM_SLOT_COUNT;
	fake_response_count--;
	pthread_mutex_unlock(&fake_queue_lock);
	return 0;
}

static struct timespec deadline_after_one_second(void)
{
	struct timespec deadline;

	assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
	deadline.tv_sec++;
	return deadline;
}

static uint64_t fake_pending_event_count(void)
{
	uint64_t count;

	pthread_mutex_lock(&fake_gate_lock);
	count = fake_pending_events;
	pthread_mutex_unlock(&fake_gate_lock);
	return count;
}

int __wrap_open(const char *path, int flags, ...)
{
	assert(strcmp(path, SG2002_RTOS_DEVICE_PATH) == 0);
	assert(flags != 0);
	return 42;
}

int __wrap_close(int fd)
{
	if (fd == 42)
		close_calls++;
	else {
		assert(fd == 43);
		pending_close_calls++;
	}
	return 0;
}

int __wrap_eventfd(unsigned int initial_value, int flags)
{
	assert(initial_value == 0U);
	assert((flags & EFD_CLOEXEC) != 0);
	assert((flags & EFD_NONBLOCK) != 0);
	assert((flags & EFD_SEMAPHORE) != 0);
	return 43;
}

ssize_t __wrap_write(int fd, const void *buffer, size_t count)
{
	uint64_t value;

	assert(fd == 43);
	assert(buffer != NULL);
	assert(count == sizeof(value));
	memcpy(&value, buffer, sizeof(value));
	assert(value == 1U);
	pthread_mutex_lock(&fake_gate_lock);
	fake_pending_events++;
	pthread_cond_broadcast(&fake_gate);
	pthread_mutex_unlock(&fake_gate_lock);
	return (ssize_t)count;
}

ssize_t __wrap_read(int fd, void *buffer, size_t count)
{
	uint64_t value = 1U;

	assert(fd == 43);
	assert(buffer != NULL);
	assert(count == sizeof(value));
	pthread_mutex_lock(&fake_gate_lock);
	if (fake_pending_events == 0U) {
		pthread_mutex_unlock(&fake_gate_lock);
		errno = EAGAIN;
		return -1;
	}
	fake_pending_events--;
	pthread_mutex_unlock(&fake_gate_lock);
	memcpy(buffer, &value, sizeof(value));
	return (ssize_t)count;
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

		if (fake_acquire_error != 0) {
			errno = fake_acquire_error;
			return -1;
		}
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
			info->abi_version = SG2002_RTOS_SHM_ABI_VERSION - 1U;
		return 0;
	}
	if (request == SG2002_RTOS_SHM_SEND) {
		struct sg2002_rtos_shm_transfer *transfer = argument;
		int ret;

		assert(fake_shared_enabled);
		fake_last_send_timeout = transfer->timeout_ms;
		ret = fake_send_slot(&transfer->message);
		if (ret == 0) {
			pthread_mutex_lock(&fake_gate_lock);
			if (fake_poll_waiting)
				fake_send_while_polling = 1;
			pthread_cond_broadcast(&fake_gate);
			pthread_mutex_unlock(&fake_gate_lock);
		}
		return ret;
	}
	if (request == SG2002_RTOS_SHM_RECEIVE) {
		struct sg2002_rtos_shm_transfer *transfer = argument;

		assert(fake_shared_enabled);
		fake_last_receive_timeout = transfer->timeout_ms;
		return fake_receive_slot(&transfer->message);
	}
	if (request == SG2002_RTOS_SHM_SUBMIT_BATCH ||
	    request == SG2002_RTOS_SHM_REAP_BATCH) {
		struct sg2002_rtos_shm_batch *batch = argument;
		struct sg2002_rtos_shm_slot *messages =
			(struct sg2002_rtos_shm_slot *)(uintptr_t)batch->slots_ptr;
		uint16_t completed = 0U;

		assert(fake_shared_enabled);
		if (!fake_batch_enabled) {
			errno = ENOTTY;
			return -1;
		}
		assert(batch->generation == 1U);
		assert(batch->count > 0U);
		assert(batch->count <= SG2002_RTOS_SHM_SLOT_COUNT);
		assert(batch->flags == 0U);
		assert(batch->reserved[0] == 0U);
		assert(batch->reserved[1] == 0U);
		if (request == fake_batch_eagain_request) {
			fake_batch_eagain_calls++;
			errno = EAGAIN;
			return -1;
		}
		if (request == SG2002_RTOS_SHM_SUBMIT_BATCH) {
			fake_batch_submit_calls++;
			while (completed < batch->count &&
			       fake_send_slot(&messages[completed]) == 0)
				completed++;
		} else {
			fake_batch_reap_calls++;
			while (completed < batch->count &&
			       fake_receive_slot(&messages[completed]) == 0)
				completed++;
		}
		if (completed == 0U)
			return -1;
		batch->completed = completed;
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

int __wrap_poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
	struct timespec deadline;
	unsigned int response_count;
	uint64_t pending_events;
	int wait_result = 0;
	int ready = 0;
	int sent_while_polling;

	assert(descriptors != NULL);
	assert(count == 1U || count == 2U);
	assert(descriptors[0].fd == 42);
	if (count == 2U)
		assert(descriptors[1].fd == 43);
	pthread_mutex_lock(&fake_gate_lock);
	fake_poll_calls++;
	if (fake_poll_block) {
		deadline = deadline_after_one_second();
		fake_poll_entered = 1;
		fake_poll_waiting = 1;
		pthread_cond_broadcast(&fake_gate);
		while (!fake_send_while_polling && wait_result == 0) {
			wait_result = pthread_cond_timedwait(
				&fake_gate, &fake_gate_lock, &deadline);
		}
		assert(wait_result == 0 || wait_result == ETIMEDOUT);
		fake_poll_waiting = 0;
		fake_poll_block = 0;
	}
	pending_events = fake_pending_events;
	sent_while_polling = fake_send_while_polling;
	pthread_mutex_unlock(&fake_gate_lock);

	pthread_mutex_lock(&fake_queue_lock);
	response_count = fake_response_count;
	pthread_mutex_unlock(&fake_queue_lock);
	descriptors[0].revents = 0;
	if ((descriptors[0].events & POLLIN) != 0 &&
	    (response_count > 0U || sent_while_polling))
		descriptors[0].revents |= POLLIN;
	if ((descriptors[0].events & POLLOUT) != 0 &&
	    response_count < SG2002_RTOS_SHM_SLOT_COUNT)
		descriptors[0].revents |= POLLOUT;
	if (descriptors[0].revents != 0)
		ready++;
	if (count == 2U) {
		descriptors[1].revents = pending_events > 0U ? POLLIN : 0;
		if (descriptors[1].revents != 0)
			ready++;
	}
	(void)timeout;
	return ready;
}

struct receive_context {
	struct sg2002_rtos *rtos;
	uint8_t response[1];
	uint16_t response_length;
	uint32_t sequence;
	int result;
};

static void *receive_response(void *argument)
{
	struct receive_context *context = argument;

	context->result = sg2002_rtos_message_receive(
		context->rtos, context->response, sizeof(context->response),
		&context->response_length, &context->sequence, 1000U);
	return NULL;
}

int main(void)
{
	struct sg2002_rtos rtos = { .fd = 42 };
	uint32_t result;
	uint32_t info;
	unsigned int calls;
	struct sg2002_rtos shared = SG2002_RTOS_INITIALIZER;
	struct sg2002_rtos legacy = SG2002_RTOS_INITIALIZER;
	struct sg2002_rtos legacy_timeout = SG2002_RTOS_INITIALIZER;
	struct sg2002_rtos bad = SG2002_RTOS_INITIALIZER;
	struct sg2002_rtos busy = SG2002_RTOS_INITIALIZER;
	uint8_t request[SG2002_RTOS_SHM_PAYLOAD_SIZE];
	uint8_t response[SG2002_RTOS_SHM_PAYLOAD_SIZE];
	uint8_t small_response[2];
	struct sg2002_rtos_shm_slot batch_messages[8];
	uint16_t batch_completed;
	uint16_t response_length;
	uint32_t sequence;
	uint32_t async_sequence;
	uint32_t sequences[8];
	uint32_t request_value;
	uint32_t response_value;
	struct receive_context receive_context;
	struct timespec deadline;
	pthread_t receive_thread;
	short revents;
	unsigned int index;
	unsigned int previous;
	int value;

	assert(sg2002_rtos_poll_fd(NULL) == -EINVAL);
	assert(sg2002_rtos_poll_fd(&legacy) == -EBADF);
	assert(sg2002_rtos_poll_fd(&rtos) == -EOPNOTSUPP);
	assert(sg2002_rtos_wait(NULL, POLLIN, &revents, 0U) == -EOPNOTSUPP);
	assert(sg2002_rtos_native_batch_available(NULL) == -EOPNOTSUPP);
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
	assert(sg2002_rtos_poll_fd(&legacy) == -EOPNOTSUPP);
	assert(sg2002_rtos_ping(&legacy, 0x13579bdfU, &result) == 0);
	assert(result == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));
	sg2002_rtos_close(&legacy);

	fake_shared_enabled = 1;
	fake_acquire_error = ETIMEDOUT;
	assert(sg2002_rtos_open(&legacy_timeout, NULL) == 0);
	assert(sg2002_rtos_shared_memory_available(&legacy_timeout) == 0);
	assert(sg2002_rtos_ping(
		       &legacy_timeout, 0x13579bdfU, &result) == 0);
	assert(result == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));
	sg2002_rtos_close(&legacy_timeout);
	fake_acquire_error = EBUSY;
	assert(sg2002_rtos_open(&busy, NULL) == -EBUSY);
	assert(busy.fd == -1);
	fake_acquire_error = 0;
	fake_bad_shm_info = 1;
	assert(sg2002_rtos_open(&bad, NULL) == -EPROTO);
	assert(bad.fd == -1);
	assert(bad.private_data == NULL);
	fake_bad_shm_info = 0;
	assert(sg2002_rtos_open(&shared, NULL) == 0);
	assert(sg2002_rtos_shared_memory_available(&shared) == 1);
	assert(sg2002_rtos_poll_fd(&shared) == 42);
	assert(sg2002_rtos_native_batch_available(&shared) == -EAGAIN);
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

	fake_batch_enabled = 1;
	memset(batch_messages, 0, sizeof(batch_messages));
	batch_messages[0].length = 1U;
	fake_batch_eagain_request = SG2002_RTOS_SHM_SUBMIT_BATCH;
	assert(sg2002_rtos_message_submit_batch(
		       &shared, batch_messages, 1U, &batch_completed, 0U) ==
	       -EAGAIN);
	assert(batch_completed == 0U);
	assert(sg2002_rtos_native_batch_available(&shared) == -EAGAIN);
	fake_batch_eagain_request = SG2002_RTOS_SHM_REAP_BATCH;
	assert(sg2002_rtos_message_reap_batch(
		       &shared, batch_messages, 1U, &batch_completed, 0U) ==
	       -EAGAIN);
	assert(batch_completed == 0U);
	assert(fake_batch_eagain_calls == 2U);
	assert(sg2002_rtos_native_batch_available(&shared) == 1);
	fake_batch_eagain_request = 0U;
	memset(batch_messages, 0, sizeof(batch_messages));
	for (index = 0U; index < 8U; index++) {
		batch_messages[index].length = sizeof(index);
		memcpy(batch_messages[index].payload, &index, sizeof(index));
	}
	assert(sg2002_rtos_message_submit_batch(
		       &shared, batch_messages, 8U, &batch_completed, 0U) == 0);
	assert(batch_completed == 8U);
	assert(fake_batch_submit_calls == 1U);
	for (index = 0U; index < 8U; index++)
		assert(batch_messages[index].sequence != 0U);
	memset(batch_messages, 0, sizeof(batch_messages));
	assert(sg2002_rtos_message_reap_batch(
		       &shared, batch_messages, 8U, &batch_completed, 0U) == 0);
	assert(batch_completed == 8U);
	assert(fake_batch_reap_calls == 1U);
	assert(sg2002_rtos_native_batch_available(&shared) == 1);
	for (index = 0U; index < 8U; index++) {
		assert(batch_messages[index].length == sizeof(index));
		memcpy(&response_value, batch_messages[index].payload,
		       sizeof(response_value));
		assert(response_value == index);
	}
	assert(sg2002_rtos_message_reap_batch(
		       &shared, batch_messages, 8U, &batch_completed, 0U) ==
	       -EAGAIN);
	assert(batch_completed == 0U);
	batch_messages[0].length = SG2002_RTOS_SHM_PAYLOAD_SIZE + 1U;
	assert(sg2002_rtos_message_submit_batch(
		       &shared, batch_messages, 1U, &batch_completed, 0U) ==
	       -EMSGSIZE);
	assert(batch_completed == 0U);
	batch_messages[0].length = 0U;
	assert(sg2002_rtos_message_submit_batch(
		       &shared, batch_messages, 0U, &batch_completed, 0U) ==
	       -EINVAL);

	for (index = 0U; index < 8U; index++) {
		request_value = 0x10203040U + index;
		memcpy(request, &request_value, sizeof(request_value));
		assert(sg2002_rtos_message_send(
			       &shared, request, sizeof(request_value), 0U,
			       &sequences[index]) == 0);
		assert(sequences[index] != 0U);
		for (previous = 0U; previous < index; previous++)
			assert(sequences[index] != sequences[previous]);
	}
	assert(fake_last_send_timeout == 0U);
	for (index = 0U; index < 8U; index++) {
		assert(sg2002_rtos_message_receive(
			       &shared, response, sizeof(response), &response_length,
			       &sequence, 0U) == 0);
		assert(sequence == sequences[index]);
		assert(response_length == sizeof(response_value));
		memcpy(&response_value, response, sizeof(response_value));
		assert(response_value == 0x10203040U + index);
	}
	assert(fake_last_receive_timeout == 0U);

	request[0] = 0xa1U;
	assert(sg2002_rtos_message_send(
		       &shared, request, 1U, 0U, &async_sequence) == 0);
	request[0] = 0xa2U;
	assert(sg2002_rtos_message_send(
		       &shared, request, 1U, 0U, &sequences[0]) == 0);
	request[0] = 0xb2U;
	assert(sg2002_rtos_message_call(
		       &shared, request, 1U, response, sizeof(response),
		       &response_length, SG2002_RTOS_DEFAULT_TIMEOUT_MS) == 0);
	assert(response_length == 1U);
	assert(response[0] == 0xb2U);
	previous = fake_poll_calls;
	assert(sg2002_rtos_wait(
		       &shared, POLLIN | POLLOUT, &revents, 0U) == 1);
	assert((revents & POLLIN) != 0);
	assert((revents & POLLOUT) != 0);
	assert(fake_poll_calls == previous + 1U);
	assert(fake_pending_event_count() == 2U);
	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), &response_length,
		       &sequence, 0U) == 0);
	assert(sequence == async_sequence);
	assert(response_length == 1U);
	assert(response[0] == 0xa1U);
	assert(fake_pending_event_count() == 1U);
	assert(sg2002_rtos_wait(&shared, POLLIN, &revents, 0U) == 1);
	assert((revents & POLLIN) != 0);
	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), &response_length,
		       &sequence, 0U) == 0);
	assert(sequence == sequences[0]);
	assert(response_length == 1U);
	assert(response[0] == 0xa2U);
	assert(fake_pending_event_count() == 0U);
	assert(sg2002_rtos_wait(&shared, POLLIN, &revents, 0U) == 0);
	assert(revents == 0);

	request_value = 0x55667788U;
	memcpy(request, &request_value, sizeof(request_value));
	assert(sg2002_rtos_message_send(
		       &shared, request, sizeof(request_value), 0U,
		       &async_sequence) == 0);
	assert(sg2002_rtos_message_receive(
		       &shared, small_response, sizeof(small_response),
		       &response_length, &sequence, 0U) == -EMSGSIZE);
	assert(sequence == async_sequence);
	assert(response_length == sizeof(request_value));
	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), &response_length,
		       &sequence, 0U) == 0);
	assert(sequence == async_sequence);
	memcpy(&response_value, response, sizeof(response_value));
	assert(response_value == request_value);
	assert(fake_pending_event_count() == 0U);
	assert(sg2002_rtos_wait(&shared, POLLIN, &revents, 0U) == 0);
	assert(revents == 0);

	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), &response_length,
		       &sequence, 0U) == -EAGAIN);
	for (index = 0U; index < SG2002_RTOS_SHM_SLOT_COUNT; index++) {
		request[0] = (uint8_t)index;
		assert(sg2002_rtos_message_send(
			       &shared, request, 1U, 0U, &sequence) == 0);
	}
	assert(sg2002_rtos_message_send(
		       &shared, request, 1U, 0U, &sequence) == -EAGAIN);
	for (index = 0U; index < SG2002_RTOS_SHM_SLOT_COUNT; index++) {
		assert(sg2002_rtos_message_receive(
			       &shared, response, sizeof(response), &response_length,
			       &sequence, 0U) == 0);
		assert(response_length == 1U);
		assert(response[0] == (uint8_t)index);
	}
	assert(sg2002_rtos_message_receive(
		       &shared, response, sizeof(response), &response_length,
		       &sequence, 0U) == -EAGAIN);

	fake_batch_enabled = 0;
	memset(batch_messages, 0, sizeof(batch_messages));
	for (index = 0U; index < 2U; index++) {
		batch_messages[index].length = 1U;
		batch_messages[index].payload[0] = (uint8_t)(0xc0U + index);
	}
	assert(sg2002_rtos_message_submit_batch(
		       &shared, batch_messages, 2U, &batch_completed, 0U) == 0);
	assert(batch_completed == 2U);
	assert(fake_batch_submit_calls == 1U);
	memset(batch_messages, 0, sizeof(batch_messages));
	assert(sg2002_rtos_message_reap_batch(
		       &shared, batch_messages, 2U, &batch_completed, 0U) == 0);
	assert(batch_completed == 2U);
	assert(fake_batch_reap_calls == 2U);
	assert(batch_messages[0].payload[0] == 0xc0U);
	assert(batch_messages[1].payload[0] == 0xc1U);
	assert(sg2002_rtos_native_batch_available(&shared) == 0);

	fake_poll_block = 1;
	fake_poll_entered = 0;
	fake_send_while_polling = 0;
	receive_context.rtos = &shared;
	receive_context.response[0] = 0U;
	receive_context.response_length = 0U;
	receive_context.sequence = 0U;
	receive_context.result = -1;
	assert(pthread_create(
		       &receive_thread, NULL, receive_response,
		       &receive_context) == 0);
	deadline = deadline_after_one_second();
	pthread_mutex_lock(&fake_gate_lock);
	while (!fake_poll_entered) {
		int wait_result = pthread_cond_timedwait(
			&fake_gate, &fake_gate_lock, &deadline);

		assert(wait_result == 0);
	}
	pthread_mutex_unlock(&fake_gate_lock);
	request[0] = 0xd3U;
	assert(sg2002_rtos_message_send(
		       &shared, request, 1U, 0U, &async_sequence) == 0);
	assert(pthread_join(receive_thread, NULL) == 0);
	assert(fake_send_while_polling == 1);
	assert(receive_context.result == 0);
	assert(receive_context.sequence == async_sequence);
	assert(receive_context.response_length == 1U);
	assert(receive_context.response[0] == 0xd3U);

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
	assert(sg2002_rtos_poll_fd(&shared) == -EBADF);
	assert(close_calls == 5U);
	assert(pending_close_calls == 1U);
	return 0;
}
