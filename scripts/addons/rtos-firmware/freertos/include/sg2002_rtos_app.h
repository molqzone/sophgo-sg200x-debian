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
