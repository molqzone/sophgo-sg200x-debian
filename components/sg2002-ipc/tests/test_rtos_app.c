#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "sg2002_rtos_app.h"
#include "sg2002_rtos_protocol.h"

static int last_output_pin = -1;
static int last_output_value = -1;

int gpio_is_valid(int pin)
{
	int bank = pin >> 8;

	return pin >= 0 && (pin & 0xff) < 32 &&
	       bank >= 0x0a && bank <= 0x0d;
}

void gpio_direction_output(int pin, int value)
{
	last_output_pin = pin;
	last_output_value = value;
}

int gpio_get_value(int pin)
{
	return pin == 0x0b03;
}

int main(void)
{
	uint8_t request[SG2002_RTOS_VALUE_MESSAGE_SIZE];
	uint8_t response[SG2002_RTOS_VALUE_MESSAGE_SIZE];
	uint16_t response_length;
	uint8_t echo[32];
	uint32_t value;

	value = 0x13579bdfU;
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_PING, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));

	value = 0;
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_GET_INFO, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == SG2002_RTOS_INFO_VALUE);

	value = SG2002_RTOS_GPIO_ENCODE(0x0b03, 1);
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_GPIO_SET, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == 1U);
	assert(last_output_pin == 0x0b03);
	assert(last_output_value == 1);

	value = SG2002_RTOS_GPIO_ENCODE(0x0b03, 0);
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_GPIO_GET, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == 1U);

	value = SG2002_RTOS_GPIO_ENCODE(0x0102, 0);
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_GPIO_GET, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT);
	value = SG2002_RTOS_GPIO_ENCODE(0x0b20, 0);
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_GPIO_GET, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT);

	value = 0;
	assert(sg2002_rtos_app_handle(SG2002_RTOS_CMD_LAST, &value) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(value == SG2002_RTOS_RESPONSE_UNSUPPORTED);
	assert(sg2002_rtos_app_handle(0x10, &value) ==
	       SG2002_RTOS_APP_UNHANDLED);

	request[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] =
		SG2002_RTOS_CMD_PING;
	value = 0x13579bdfU;
	memcpy(request + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET, &value,
	       sizeof(value));
	response_length = sizeof(response);
	assert(sg2002_rtos_app_handle_message(
		       request, sizeof(request), response, &response_length) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(response_length == sizeof(response));
	assert(response[SG2002_RTOS_VALUE_MESSAGE_COMMAND_OFFSET] ==
	       SG2002_RTOS_CMD_PING);
	memcpy(&value,
	       response + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
	       sizeof(value));
	assert(value == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));

	response_length = sizeof(response);
	assert(sg2002_rtos_app_handle_message(request, 1U, response,
					      &response_length) ==
	       SG2002_RTOS_APP_HANDLED);
	memcpy(&value,
	       response + SG2002_RTOS_VALUE_MESSAGE_VALUE_OFFSET,
	       sizeof(value));
	assert(value == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT);

	memset(echo, 0x5a, sizeof(echo));
	echo[0] = SG2002_RTOS_CMD_ECHO;
	response_length = 1U;
	assert(sg2002_rtos_app_handle_message(
		       echo, 1U, response, &response_length) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(response_length == 1U);
	assert(response[0] == SG2002_RTOS_CMD_ECHO);
	response_length = sizeof(response);
	assert(sg2002_rtos_app_handle_message(
		       echo, sizeof(echo), echo, &response_length) ==
	       SG2002_RTOS_APP_UNHANDLED);
	response_length = sizeof(echo);
	assert(sg2002_rtos_app_handle_message(
		       echo, sizeof(echo), echo, &response_length) ==
	       SG2002_RTOS_APP_HANDLED);
	assert(response_length == sizeof(echo));
	assert(echo[0] == SG2002_RTOS_CMD_ECHO);
	return 0;
}
