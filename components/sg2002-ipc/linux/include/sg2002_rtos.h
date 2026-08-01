#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sg2002_rtos_protocol.h"
#include "sg2002_rtos_shm.h"

#define SG2002_RTOS_DEVICE_PATH "/dev/cvi-rtos-cmdqu"
#define SG2002_RTOS_DEFAULT_TIMEOUT_MS 3000U

/**
 * Linux-side handle for the SG2002 C906L control channel.
 *
 * Calls may be made from multiple threads while the handle remains open. The
 * kernel transport serializes synchronous transactions because the vendor ABI
 * has no transaction identifier. Closing a handle concurrently is unsupported.
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
 * Submit one opaque application message.
 *
 * The kernel assigns a nonzero sequence number. A zero timeout is
 * nonblocking. Shared-memory support must have been acquired by open().
 */
int sg2002_rtos_message_send(struct sg2002_rtos *rtos,
	const void *request, uint16_t request_length, uint32_t timeout_ms,
	uint32_t *sequence);

/** Receive the oldest response not retained by a synchronous caller. */
int sg2002_rtos_message_receive(struct sg2002_rtos *rtos,
	void *response, uint16_t response_capacity, uint16_t *response_length,
	uint32_t *sequence, uint32_t timeout_ms);

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
