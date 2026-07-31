#include <assert.h>
#include <stdint.h>

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
	return 0;
}
