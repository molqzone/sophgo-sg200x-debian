#include <stdio.h>
#ifdef RUN_IN_SRAM
#include "system_common.h"
#endif

#include "mmio.h"
#include "gpio.h"

static uint32_t gpio_base_from_pin(int pin)
{
	switch (pin >> 8) {
	case 0xA:
		return CVI_GPIOA_BASE;
	case 0xB:
		return CVI_GPIOB_BASE;
	case 0xC:
		return CVI_GPIOC_BASE;
	case 0xD:
		return CVI_GPIOD_BASE;
	default:
		return 0;
	}
}

int gpio_is_valid(int pin)
{
	return pin >= 0 && (pin & 0xff) < 32 && gpio_base_from_pin(pin) != 0;
}

int gpio_get_value(int pin)
{
	uint32_t gpio_base;
	uint32_t pin_mask;

	if (!gpio_is_valid(pin))
		return 0;

	gpio_base = gpio_base_from_pin(pin);
	pin &= 0xff;
	pin_mask = 1U << pin;
	return !!(mmio_read_32(gpio_base + 0x50) & pin_mask);
}

void gpio_direction_output(int pin, int val)
{
	uint32_t gpio_base;
	uint32_t direction;
	uint32_t output;
	uint32_t pin_mask;

	if (!gpio_is_valid(pin))
		return;

	gpio_base = gpio_base_from_pin(pin);
	pin &= 0xff;
	pin_mask = 1U << pin;
	output = mmio_read_32(gpio_base);
	if (val)
		output |= pin_mask;
	else
		output &= ~pin_mask;
	mmio_write_32(gpio_base, output);

	direction = mmio_read_32(gpio_base + 4);
	mmio_write_32(gpio_base + 4, direction | pin_mask);
}
