#include "sg2002_rtos_app.h"

#include <stddef.h>
#include <string.h>

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
	case SG2002_RTOS_CMD_ECHO:
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

enum sg2002_rtos_app_result sg2002_rtos_app_handle_message(
	const uint8_t *request, uint16_t request_length, uint8_t *response,
	uint16_t *response_length)
{
	enum sg2002_rtos_app_result result;
	uint16_t response_capacity;
	uint32_t value;
	uint8_t command;

	if (response_length == NULL)
		return SG2002_RTOS_APP_UNHANDLED;
	response_capacity = *response_length;
	*response_length = 0U;
	if (request == NULL || response == NULL || request_length == 0U)
		return SG2002_RTOS_APP_UNHANDLED;

	command = request[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET];
	if (command < SG2002_RTOS_CMD_FIRST ||
	    command > SG2002_RTOS_CMD_LAST) {
		return SG2002_RTOS_APP_UNHANDLED;
	}
	if (command == SG2002_RTOS_CMD_ECHO) {
		if (request_length > response_capacity)
			return SG2002_RTOS_APP_UNHANDLED;
		memcpy(response, request, request_length);
		*response_length = request_length;
		return SG2002_RTOS_APP_HANDLED;
	}
	if (response_capacity < SG2002_RTOS_VALUE_MESSAGE_SIZE)
		return SG2002_RTOS_APP_UNHANDLED;

	if (request_length == SG2002_RTOS_VALUE_MESSAGE_SIZE) {
		memcpy(&value,
		       request + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
		       sizeof(value));
		result = sg2002_rtos_app_handle(command, &value);
	} else {
		value = SG2002_RTOS_RESPONSE_INVALID_ARGUMENT;
		result = SG2002_RTOS_APP_HANDLED;
	}
	if (result != SG2002_RTOS_APP_HANDLED)
		return result;

	response[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] = command;
	memcpy(response + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET, &value,
	       sizeof(value));
	*response_length = SG2002_RTOS_VALUE_MESSAGE_SIZE;
	return SG2002_RTOS_APP_HANDLED;
}
