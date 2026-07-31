#pragma once

#include <stdint.h>

#include "sg2002_rtos_protocol.h"

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
};

#define SG2002_RTOS_INITIALIZER { .fd = -1 }

/** Open the default command-queue device, or an explicit test/device path. */
int sg2002_rtos_open(struct sg2002_rtos *rtos, const char *device_path);

/** Close a previously opened handle. A null or already closed handle is safe. */
void sg2002_rtos_close(struct sg2002_rtos *rtos);

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
