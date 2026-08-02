#pragma once

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#include "sg2002_rtos_protocol.h"
#include "sg2002_rtos_shm.h"

#define SG2002_RTOS_DEVICE_PATH "/dev/cvi-rtos-cmdqu"
#define SG2002_RTOS_DEFAULT_TIMEOUT_MS 3000U

/**
 * Linux-side handle for the SG2002 C906L control channel.
 *
 * Calls may be made from multiple threads while the handle remains open.
 * Shared-memory transmit and receive operations progress independently;
 * synchronous calls serialize only the receive side while matching their
 * sequence. Closing a handle concurrently is unsupported.
 */
struct sg2002_rtos {
	int fd;
	void *private_data;
};

#define SG2002_RTOS_INITIALIZER { .fd = -1, .private_data = NULL }

/** Open the default command-queue device, or an explicit test/device path. */
int sg2002_rtos_open(struct sg2002_rtos *rtos, const char *device_path);

/** Close a previously opened handle. A null or already closed handle is safe. */
void sg2002_rtos_close(struct sg2002_rtos *rtos);

/** Return one when the handle owns the shared-memory channel, otherwise zero. */
int sg2002_rtos_shared_memory_available(const struct sg2002_rtos *rtos);

/**
 * Return the borrowed descriptor for raw kernel-ring poll/epoll readiness.
 *
 * POLLIN means the kernel response ring is nonempty and POLLOUT means the
 * request ring has space. Responses retained in the userspace handle are not
 * reflected; use sg2002_rtos_wait() when synchronous and asynchronous calls
 * are mixed. The descriptor is borrowed and must not be closed or modified.
 */
int sg2002_rtos_poll_fd(const struct sg2002_rtos *rtos);

/**
 * Wait for shared-memory readiness while accounting for retained responses.
 *
 * events accepts POLLIN and POLLOUT. The return value follows poll(): one for
 * readiness, zero for timeout, or a negative errno value. Use this helper when
 * synchronous and asynchronous calls share a handle; the raw poll descriptor
 * reflects only the kernel rings.
 */
int sg2002_rtos_wait(struct sg2002_rtos *rtos, short events, short *revents,
	uint32_t timeout_ms);

/**
 * Submit one opaque application message.
 *
 * The kernel assigns a nonzero sequence number. A zero timeout is
 * nonblocking. Shared-memory support must have been acquired by open().
 */
int sg2002_rtos_message_send(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, uint32_t timeout_ms,
	uint32_t *sequence);

/** Receive the oldest response, including one retained by a synchronous call. */
int sg2002_rtos_message_receive(struct sg2002_rtos *rtos,
	void *response, uint16_t response_capacity, uint16_t *response_length,
	uint32_t *sequence, uint32_t timeout_ms);

/**
 * Submit a prefix of messages with one kernel call when batch UAPI is present.
 *
 * Each input slot supplies length and payload; sequence is assigned on
 * success. completed is always set and a zero timeout is nonblocking. Older
 * kernels transparently fall back to the single-message UAPI.
 */
int sg2002_rtos_message_submit_batch(struct sg2002_rtos *rtos,
	struct sg2002_rtos_shm_slot *messages, uint16_t count,
	uint16_t *completed, uint32_t timeout_ms);

/**
 * Reap a prefix of available responses with one kernel call when supported.
 *
 * The response sequence remains the asynchronous RPC ticket. Older kernels
 * transparently fall back to the single-message UAPI.
 */
int sg2002_rtos_message_reap_batch(struct sg2002_rtos *rtos,
	struct sg2002_rtos_shm_slot *messages, uint16_t count,
	uint16_t *completed, uint32_t timeout_ms);

/**
 * Report native kernel batch support after both directions have been probed.
 *
 * Returns one for native submit/reap ioctls, zero when compatibility fallback
 * was detected, -EAGAIN before both directions are known, or another negative
 * errno value on invalid input.
 */
int sg2002_rtos_native_batch_available(struct sg2002_rtos *rtos);

/**
 * Submit one opaque request and wait for the response with the same sequence.
 *
 * Calls on one handle are serialized. Responses for other outstanding
 * sequences are retained for message_receive().
 */
int sg2002_rtos_message_call(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, void *response,
	uint16_t response_capacity, uint16_t *response_length,
	uint32_t timeout_ms);

/**
 * Execute one synchronous 32-bit application request.
 *
 * Returns zero on a transport reply or a negative errno value on failure.
 * Application-specific reserved responses remain visible to the caller.
 */
int sg2002_rtos_call(struct sg2002_rtos *rtos, uint8_t command,
		     uint32_t request, uint16_t timeout_ms, uint32_t *response);

int sg2002_rtos_get_info(struct sg2002_rtos *rtos, uint32_t *info);
int sg2002_rtos_ping(struct sg2002_rtos *rtos, uint32_t input,
		     uint32_t *result);
int sg2002_rtos_gpio_set(struct sg2002_rtos *rtos, uint16_t pin,
			 int value);
int sg2002_rtos_gpio_get(struct sg2002_rtos *rtos, uint16_t pin,
			 int *value);
