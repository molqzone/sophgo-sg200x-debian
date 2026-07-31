#pragma once

int gpio_is_valid(int pin);
void gpio_direction_output(int pin, int value);
int gpio_get_value(int pin);
