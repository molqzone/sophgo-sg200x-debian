#include "sg2002_rtos_app.h"

#include <stddef.h>

#include "gpio.h"
#include "sg2002_rtos_protocol.h"

#ifdef SG2002_RTOS_TRACE_MAILBOX
#include "boot_trace.h"
#endif

enum sg2002_rtos_app_result sg2002_rtos_app_handle(uint8_t command,
						   uint32_t *value)
{
	unsigned int pin;
	unsigned int gpio_value;

	if (value == NULL)
		return SG2002_RTOS_APP_UNHANDLED;

	switch (command) {
	case SG2002_RTOS_CMD_PING:
		*value ^= SG2002_RTOS_PING_XOR;
#ifdef SG2002_RTOS_TRACE_MAILBOX
		cvitek_boot_trace_event(CVITEK_BOOT_TRACE_EVENT_PING_HANDLED,
					*value);
#endif
		return SG2002_RTOS_APP_HANDLED;
	case SG2002_RTOS_CMD_GPIO_SET:
		pin = SG2002_RTOS_GPIO_PIN(*value);
		gpio_value = SG2002_RTOS_GPIO_VALUE(*value);
		if (!gpio_is_valid((int)pin)) {
			*value = SG2002_RTOS_RESPONSE_INVALID_ARGUMENT;
			return SG2002_RTOS_APP_HANDLED;
		}
		gpio_direction_output((int)pin, gpio_value ? 1 : 0);
		*value = gpio_value;
		return SG2002_RTOS_APP_HANDLED;
	case SG2002_RTOS_CMD_GPIO_GET:
		pin = SG2002_RTOS_GPIO_PIN(*value);
		if (!gpio_is_valid((int)pin)) {
			*value = SG2002_RTOS_RESPONSE_INVALID_ARGUMENT;
			return SG2002_RTOS_APP_HANDLED;
		}
		*value = (uint32_t)(gpio_get_value((int)pin) & 0x1);
		return SG2002_RTOS_APP_HANDLED;
	case SG2002_RTOS_CMD_GET_INFO:
		*value = SG2002_RTOS_INFO_VALUE;
		return SG2002_RTOS_APP_HANDLED;
	default:
		if (command >= SG2002_RTOS_CMD_FIRST &&
		    command <= SG2002_RTOS_CMD_LAST) {
			*value = SG2002_RTOS_RESPONSE_UNSUPPORTED;
			return SG2002_RTOS_APP_HANDLED;
		}
		return SG2002_RTOS_APP_UNHANDLED;
	}
}
