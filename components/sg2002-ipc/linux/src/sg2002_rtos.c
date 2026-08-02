#include "sg2002_rtos.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "rtos_cmdqu.h"

typedef char sg2002_cmdqu_size_must_be_8[(sizeof(cmdqu_t) == 8) ? 1 : -1];

#define SG2002_RTOS_BATCH_UNKNOWN 0
#define SG2002_RTOS_BATCH_SUPPORTED 1
#define SG2002_RTOS_BATCH_UNSUPPORTED (-1)

struct sg2002_rtos_private {
	pthread_mutex_t tx_lock;
	pthread_mutex_t rx_lock;
	int pending_fd;
	int shared_memory_enabled;
	int submit_batch_supported;
	int reap_batch_supported;
	uint32_t generation;
	struct sg2002_rtos_shm_slot pending[SG2002_RTOS_SHM_SLOT_COUNT];
	size_t pending_count;
};

static uint64_t monotonic_milliseconds(void);

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
		int acquire_error = errno;

		if (acquire_error != ENOTTY && acquire_error != ENODEV &&
		    acquire_error != EOPNOTSUPP && acquire_error != ETIMEDOUT) {
			close(fd);
			return -acquire_error;
		}
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
	mutex_result = pthread_mutex_init(&private->tx_lock, NULL);
	if (mutex_result != 0) {
		free(private);
		close(fd);
		return -mutex_result;
	}
	mutex_result = pthread_mutex_init(&private->rx_lock, NULL);
	if (mutex_result != 0) {
		pthread_mutex_destroy(&private->tx_lock);
		free(private);
		close(fd);
		return -mutex_result;
	}
	private->pending_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK |
		EFD_SEMAPHORE);
	if (private->pending_fd < 0) {
		int eventfd_error = errno;

		pthread_mutex_destroy(&private->rx_lock);
		pthread_mutex_destroy(&private->tx_lock);
		free(private);
		close(fd);
		return -eventfd_error;
	}
	private->shared_memory_enabled = 1;
	private->generation = info.generation;
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

		close(private->pending_fd);
		pthread_mutex_destroy(&private->rx_lock);
		pthread_mutex_destroy(&private->tx_lock);
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

int sg2002_rtos_poll_fd(const struct sg2002_rtos *rtos)
{
	if (rtos == NULL)
		return -EINVAL;
	if (rtos->fd < 0)
		return -EBADF;
	if (!sg2002_rtos_shared_memory_available(rtos))
		return -EOPNOTSUPP;
	return rtos->fd;
}

int sg2002_rtos_wait(struct sg2002_rtos *rtos, short events, short *revents,
	uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	const short supported_events = POLLIN | POLLOUT;
	struct pollfd descriptors[2] = {0};
	nfds_t descriptor_count = 1U;
	uint64_t deadline = 0U;
	uint64_t now;
	uint64_t difference;
	uint32_t remaining = timeout_ms;
	int timeout;
	int ret;

	if (revents != NULL)
		*revents = 0;
	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (revents == NULL || events == 0 ||
	    (events & (short)~supported_events) != 0)
		return -EINVAL;
	descriptors[0].fd = rtos->fd;
	descriptors[0].events = events;
	if ((events & POLLIN) != 0) {
		descriptors[1].fd = private->pending_fd;
		descriptors[1].events = POLLIN;
		descriptor_count = 2U;
	}

	if (timeout_ms != UINT32_MAX)
		deadline = monotonic_milliseconds() + timeout_ms;
	for (;;) {
		timeout = remaining == UINT32_MAX ? -1 :
			(remaining > (uint32_t)INT_MAX ? INT_MAX :
			 (int)remaining);
		ret = poll(descriptors, descriptor_count, timeout);
		if (ret != 0 || timeout_ms == UINT32_MAX)
			break;
		now = monotonic_milliseconds();
		if (now >= deadline)
			break;
		difference = deadline - now;
		remaining = difference >= UINT32_MAX ? UINT32_MAX - 1U :
			(uint32_t)difference;
	}
	if (ret < 0)
		return -errno;
	if (ret == 0)
		return 0;
	*revents = descriptors[0].revents;
	if (descriptor_count == 2U) {
		if ((descriptors[1].revents & POLLIN) != 0)
			*revents |= POLLIN;
		if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			*revents |= POLLERR;
	}
	return *revents != 0 ? 1 : 0;
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

static int pending_event_signal(struct sg2002_rtos_private *private)
{
	uint64_t value = 1U;
	ssize_t written;

	do {
		written = write(private->pending_fd, &value, sizeof(value));
	} while (written < 0 && errno == EINTR);
	if (written == (ssize_t)sizeof(value))
		return 0;
	return written < 0 ? -errno : -EIO;
}

static int pending_event_consume(struct sg2002_rtos_private *private)
{
	uint64_t value;
	ssize_t received;

	do {
		received = read(private->pending_fd, &value, sizeof(value));
	} while (received < 0 && errno == EINTR);
	if (received == (ssize_t)sizeof(value) && value == 1U)
		return 0;
	return received < 0 ? -errno : -EIO;
}

static int retain_pending(struct sg2002_rtos_private *private,
	const struct sg2002_rtos_shm_slot *message)
{
	int ret;

	if (private->pending_count >= SG2002_RTOS_SHM_SLOT_COUNT)
		return -ENOBUFS;
	private->pending[private->pending_count] = *message;
	private->pending_count++;
	ret = pending_event_signal(private);
	if (ret) {
		private->pending_count--;
		return ret;
	}
	return 0;
}

static int remove_pending(struct sg2002_rtos_private *private, size_t index)
{
	int ret;

	if (index >= private->pending_count)
		return -ENOENT;
	private->pending_count--;
	if (index < private->pending_count) {
		memmove(&private->pending[index], &private->pending[index + 1U],
			(private->pending_count - index) *
				sizeof(private->pending[0]));
	}
	ret = pending_event_consume(private);
	return ret == -EAGAIN ? -EPROTO : ret;
}

static int take_pending_oldest(struct sg2002_rtos_private *private,
	struct sg2002_rtos_shm_slot *message)
{
	int ret;

	if (private->pending_count == 0U)
		return 0;
	*message = private->pending[0];
	ret = remove_pending(private, 0U);
	if (ret)
		return ret;
	return 1;
}

static int take_pending_sequence(struct sg2002_rtos_private *private,
	uint32_t sequence, struct sg2002_rtos_shm_slot *message)
{
	size_t index;

	for (index = 0; index < private->pending_count; index++) {
		if (private->pending[index].sequence != sequence)
			continue;
		*message = private->pending[index];
		if (remove_pending(private, index))
			return -EPROTO;
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

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0U;
	return (uint64_t)now.tv_sec * 1000U +
	       (uint64_t)now.tv_nsec / 1000000U;
}

static uint64_t timeout_deadline(uint32_t timeout_ms)
{
	if (timeout_ms == 0U || timeout_ms == UINT32_MAX)
		return 0U;
	return monotonic_milliseconds() + timeout_ms;
}

static int update_remaining(uint32_t timeout_ms, uint64_t deadline,
	uint32_t *remaining)
{
	uint64_t now;
	uint64_t difference;

	if (timeout_ms == UINT32_MAX) {
		*remaining = UINT32_MAX;
		return 0;
	}
	now = monotonic_milliseconds();
	if (now >= deadline)
		return -ETIMEDOUT;
	difference = deadline - now;
	*remaining = difference >= UINT32_MAX ? UINT32_MAX - 1U :
		(uint32_t)difference;
	return 0;
}

static int wait_for_events(struct sg2002_rtos *rtos, short events,
	uint32_t timeout_ms, uint64_t deadline, uint32_t *remaining)
{
	short revents;
	int ret;

	if (timeout_ms == 0U)
		return -EAGAIN;
	ret = update_remaining(timeout_ms, deadline, remaining);
	if (ret)
		return ret;
	ret = sg2002_rtos_wait(rtos, events, &revents, *remaining);
	if (ret < 0)
		return ret;
	if (ret == 0)
		return -ETIMEDOUT;
	return 0;
}

static int wait_for_kernel_events(struct sg2002_rtos *rtos, short events,
	uint32_t timeout_ms, uint64_t deadline, uint32_t *remaining)
{
	struct pollfd descriptor = {
		.fd = rtos->fd,
		.events = events,
	};
	int ret;

	if (timeout_ms == 0U)
		return -EAGAIN;
	for (;;) {
		int timeout;

		ret = update_remaining(timeout_ms, deadline, remaining);
		if (ret)
			return ret;
		timeout = *remaining == UINT32_MAX ? -1 :
			(*remaining > (uint32_t)INT_MAX ? INT_MAX :
			 (int)*remaining);
		ret = poll(&descriptor, 1U, timeout);
		if (ret < 0)
			return -errno;
		if (ret > 0)
			return 0;
	}
}

int sg2002_rtos_message_send(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, uint32_t timeout_ms,
	uint32_t *sequence)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	uint64_t deadline = timeout_deadline(timeout_ms);
	uint32_t remaining = timeout_ms;
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	for (;;) {
		ret = pthread_mutex_lock(&private->tx_lock);
		if (ret != 0)
			return -ret;
		ret = message_send_locked(rtos, request, request_length, 0U,
					  sequence);
		pthread_mutex_unlock(&private->tx_lock);
		if (ret != -EAGAIN && ret != -ETIMEDOUT)
			return ret;
		ret = wait_for_events(rtos, POLLOUT, timeout_ms, deadline,
				      &remaining);
		if (ret)
			return ret;
	}
}

int sg2002_rtos_message_receive(struct sg2002_rtos *rtos,
	void *response, uint16_t response_capacity, uint16_t *response_length,
	uint32_t *sequence, uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	struct sg2002_rtos_shm_slot message;
	uint64_t deadline = timeout_deadline(timeout_ms);
	uint32_t remaining = timeout_ms;
	int from_pending;
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (response_length == NULL ||
	    (response_capacity > 0U && response == NULL))
		return -EINVAL;
	for (;;) {
		ret = pthread_mutex_lock(&private->rx_lock);
		if (ret != 0)
			return -ret;
		from_pending = private->pending_count > 0U;
		if (from_pending) {
			message = private->pending[0];
			ret = 0;
		} else {
			ret = receive_from_kernel_locked(rtos, &message, 0U);
		}
		if (!ret) {
			ret = copy_message(&message, response, response_capacity,
					   response_length, sequence);
			if (from_pending && ret != -EMSGSIZE) {
				int remove_result = remove_pending(private, 0U);

				if (remove_result)
					ret = remove_result;
			} else if (!from_pending && ret == -EMSGSIZE) {
				int retain_result = retain_pending(private, &message);

				if (retain_result)
					ret = retain_result;
			}
		}
		pthread_mutex_unlock(&private->rx_lock);
		if (ret != -EAGAIN && ret != -ETIMEDOUT)
			return ret;
		ret = wait_for_events(rtos, POLLIN, timeout_ms, deadline,
				      &remaining);
		if (ret)
			return ret;
	}
}

static int submit_batch_locked(struct sg2002_rtos *rtos,
	struct sg2002_rtos_private *private,
	struct sg2002_rtos_shm_slot *messages, uint16_t count,
	uint16_t *completed, uint32_t timeout_ms)
{
	struct sg2002_rtos_shm_batch batch = {0};
	uint16_t index;
	int ret;

	if (private->submit_batch_supported != SG2002_RTOS_BATCH_UNSUPPORTED) {
		batch.slots_ptr = (uintptr_t)messages;
		batch.timeout_ms = timeout_ms;
		batch.generation = private->generation;
		batch.count = count;
		if (ioctl(rtos->fd, SG2002_RTOS_SHM_SUBMIT_BATCH, &batch) == 0) {
			if (batch.completed == 0U || batch.completed > count)
				return -EPROTO;
			for (index = 0U; index < batch.completed; index++) {
				if (messages[index].sequence == 0U)
					return -EPROTO;
			}
			private->submit_batch_supported = SG2002_RTOS_BATCH_SUPPORTED;
			*completed = batch.completed;
			return 0;
		}
		ret = -errno;
		if (ret != -ENOTTY) {
			private->submit_batch_supported =
				SG2002_RTOS_BATCH_SUPPORTED;
			return ret;
		}
		private->submit_batch_supported = SG2002_RTOS_BATCH_UNSUPPORTED;
	}

	for (index = 0U; index < count; index++) {
		uint32_t sequence;
		uint32_t item_timeout = index == 0U ? timeout_ms : 0U;

		ret = message_send_locked(rtos, messages[index].payload,
			messages[index].length, item_timeout, &sequence);
		if (ret) {
			if (index > 0U && (ret == -EAGAIN || ret == -ETIMEDOUT))
				break;
			*completed = index;
			return ret;
		}
		messages[index].sequence = sequence;
	}
	*completed = index;
	return index > 0U ? 0 : -EAGAIN;
}

int sg2002_rtos_message_submit_batch(struct sg2002_rtos *rtos,
	struct sg2002_rtos_shm_slot *messages, uint16_t count,
	uint16_t *completed, uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	uint64_t deadline = timeout_deadline(timeout_ms);
	uint32_t remaining = timeout_ms;
	uint16_t index;
	int ret;

	if (completed != NULL)
		*completed = 0U;
	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (messages == NULL || completed == NULL || count == 0U ||
	    count > SG2002_RTOS_SHM_SLOT_COUNT)
		return -EINVAL;
	for (index = 0U; index < count; index++) {
		if (messages[index].length > SG2002_RTOS_SHM_PAYLOAD_SIZE)
			return -EMSGSIZE;
	}
	for (;;) {
		ret = pthread_mutex_lock(&private->tx_lock);
		if (ret != 0)
			return -ret;
		*completed = 0U;
		ret = submit_batch_locked(rtos, private, messages, count,
					  completed, 0U);
		pthread_mutex_unlock(&private->tx_lock);
		if (ret != -EAGAIN && ret != -ETIMEDOUT)
			return ret;
		ret = wait_for_events(rtos, POLLOUT, timeout_ms, deadline,
				      &remaining);
		if (ret)
			return ret;
	}
}

static int reap_batch_locked(struct sg2002_rtos *rtos,
	struct sg2002_rtos_private *private,
	struct sg2002_rtos_shm_slot *messages, uint16_t count,
	uint16_t *completed, uint32_t timeout_ms)
{
	struct sg2002_rtos_shm_batch batch = {0};
	uint16_t index = 0U;
	int ret;

	while (index < count) {
		ret = take_pending_oldest(private, &messages[index]);
		if (ret < 0)
			return ret;
		if (ret == 0)
			break;
		index++;
	}
	if (index > 0U) {
		*completed = index;
		return 0;
	}

	if (private->reap_batch_supported != SG2002_RTOS_BATCH_UNSUPPORTED) {
		batch.slots_ptr = (uintptr_t)messages;
		batch.timeout_ms = timeout_ms;
		batch.generation = private->generation;
		batch.count = count;
		if (ioctl(rtos->fd, SG2002_RTOS_SHM_REAP_BATCH, &batch) == 0) {
			if (batch.completed == 0U || batch.completed > count)
				return -EPROTO;
			for (index = 0U; index < batch.completed; index++) {
				if (messages[index].sequence == 0U ||
				    messages[index].length >
					    SG2002_RTOS_SHM_PAYLOAD_SIZE)
					return -EPROTO;
			}
			private->reap_batch_supported = SG2002_RTOS_BATCH_SUPPORTED;
			*completed = batch.completed;
			return 0;
		}
		ret = -errno;
		if (ret != -ENOTTY) {
			private->reap_batch_supported =
				SG2002_RTOS_BATCH_SUPPORTED;
			return ret;
		}
		private->reap_batch_supported = SG2002_RTOS_BATCH_UNSUPPORTED;
	}

	for (index = 0U; index < count; index++) {
		uint32_t item_timeout = index == 0U ? timeout_ms : 0U;

		ret = receive_from_kernel_locked(rtos, &messages[index],
					 item_timeout);
		if (ret) {
			if (index > 0U && (ret == -EAGAIN || ret == -ETIMEDOUT))
				break;
			*completed = index;
			return ret;
		}
	}
	*completed = index;
	return index > 0U ? 0 : -EAGAIN;
}

int sg2002_rtos_message_reap_batch(struct sg2002_rtos *rtos,
	struct sg2002_rtos_shm_slot *messages, uint16_t count,
	uint16_t *completed, uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	uint64_t deadline = timeout_deadline(timeout_ms);
	uint32_t remaining = timeout_ms;
	int ret;

	if (completed != NULL)
		*completed = 0U;
	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (messages == NULL || completed == NULL || count == 0U ||
	    count > SG2002_RTOS_SHM_SLOT_COUNT)
		return -EINVAL;
	for (;;) {
		ret = pthread_mutex_lock(&private->rx_lock);
		if (ret != 0)
			return -ret;
		*completed = 0U;
		ret = reap_batch_locked(rtos, private, messages, count,
					 completed, 0U);
		pthread_mutex_unlock(&private->rx_lock);
		if (ret != -EAGAIN && ret != -ETIMEDOUT)
			return ret;
		ret = wait_for_events(rtos, POLLIN, timeout_ms, deadline,
				      &remaining);
		if (ret)
			return ret;
	}
}

int sg2002_rtos_native_batch_available(struct sg2002_rtos *rtos)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	int submit_status;
	int reap_status;
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	ret = pthread_mutex_lock(&private->rx_lock);
	if (ret != 0)
		return -ret;
	reap_status = private->reap_batch_supported;
	pthread_mutex_unlock(&private->rx_lock);
	ret = pthread_mutex_lock(&private->tx_lock);
	if (ret != 0)
		return -ret;
	submit_status = private->submit_batch_supported;
	pthread_mutex_unlock(&private->tx_lock);
	if (submit_status == SG2002_RTOS_BATCH_UNSUPPORTED ||
	    reap_status == SG2002_RTOS_BATCH_UNSUPPORTED)
		return 0;
	if (submit_status == SG2002_RTOS_BATCH_SUPPORTED &&
	    reap_status == SG2002_RTOS_BATCH_SUPPORTED)
		return 1;
	return -EAGAIN;
}
int sg2002_rtos_message_call(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, void *response,
	uint16_t response_capacity, uint16_t *response_length,
	uint32_t timeout_ms)
{
	struct sg2002_rtos_private *private = private_data(rtos);
	struct sg2002_rtos_shm_slot message;
	uint64_t deadline;
	uint64_t now;
	uint32_t remaining = timeout_ms;
	uint32_t sequence = 0U;
	int pending_result;
	int ret;

	if (rtos == NULL || rtos->fd < 0 || private == NULL ||
	    !private->shared_memory_enabled)
		return -EOPNOTSUPP;
	if (response_length == NULL ||
	    (response_capacity > 0U && response == NULL))
		return -EINVAL;
	deadline = timeout_deadline(timeout_ms);
	for (;;) {
		ret = pthread_mutex_lock(&private->rx_lock);
		if (ret != 0)
			return -ret;
		ret = pthread_mutex_lock(&private->tx_lock);
		if (ret != 0) {
			pthread_mutex_unlock(&private->rx_lock);
			return -ret;
		}
		ret = message_send_locked(rtos, request, request_length, 0U,
					  &sequence);
		pthread_mutex_unlock(&private->tx_lock);
		if (!ret)
			break;
		if (ret != -EAGAIN && ret != -ETIMEDOUT)
			goto out_unlock;
		ret = receive_from_kernel_locked(rtos, &message, 0U);
		if (!ret) {
			ret = retain_pending(private, &message);
			pthread_mutex_unlock(&private->rx_lock);
			if (ret)
				return ret;
			continue;
		}
		if (ret != -EAGAIN && ret != -ETIMEDOUT)
			goto out_unlock;
		pthread_mutex_unlock(&private->rx_lock);
		ret = wait_for_kernel_events(rtos, POLLIN | POLLOUT,
					     timeout_ms, deadline, &remaining);
		if (ret)
			return ret;
	}
	for (;;) {
		pending_result = take_pending_sequence(private, sequence,
					       &message);
		if (pending_result < 0) {
			ret = pending_result;
			goto out_unlock;
		}
		if (pending_result > 0)
			break;
		ret = receive_from_kernel_locked(rtos, &message, 0U);
		if (ret == -EAGAIN || ret == -ETIMEDOUT) {
			ret = wait_for_kernel_events(rtos, POLLIN, timeout_ms,
					     deadline, &remaining);
			if (ret)
				goto out_unlock;
			continue;
		}
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
	pthread_mutex_unlock(&private->rx_lock);
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
