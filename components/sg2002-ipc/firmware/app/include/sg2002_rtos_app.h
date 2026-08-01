#pragma once

#include <stdint.h>

enum sg2002_rtos_app_result {
	SG2002_RTOS_APP_UNHANDLED = 0,
	SG2002_RTOS_APP_HANDLED = 1,
};

/**
 * Handle one application command in task context.
 *
 * Transport and mailbox state are deliberately absent from this interface.
 * Add new application commands here and in the shared protocol header only.
 */
enum sg2002_rtos_app_result sg2002_rtos_app_handle(uint8_t command,
						   uint32_t *value);

/**
 * Handle one application message in task context.
 *
 * The shared-memory transport supplies only opaque payload bytes. This
 * function owns the application framing and writes at most the capacity
 * supplied through response_length. On return, response_length is the number
 * of response bytes produced.
 */
enum sg2002_rtos_app_result sg2002_rtos_app_handle_message(
	const uint8_t *request, uint16_t request_length, uint8_t *response,
	uint16_t *response_length);
