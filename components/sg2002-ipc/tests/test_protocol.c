#include <assert.h>
#include <stdint.h>

#include "sg2002_rtos_protocol.h"

int main(void)
{
	uint32_t info = SG2002_RTOS_INFO_VALUE;
	uint32_t gpio = SG2002_RTOS_GPIO_ENCODE(0x1234U, 1U);

	assert(SG2002_RTOS_CMD_PING == 0x60U);
	assert(SG2002_RTOS_CMD_GET_INFO <= SG2002_RTOS_CMD_LAST);
	assert(SG2002_RTOS_INFO_IS_VALID(info));
	assert(SG2002_RTOS_INFO_MAJOR(info) == SG2002_RTOS_PROTOCOL_MAJOR);
	assert(SG2002_RTOS_INFO_MINOR(info) == SG2002_RTOS_PROTOCOL_MINOR);
	assert(SG2002_RTOS_INFO_CAPABILITIES(info) & SG2002_RTOS_CAP_GPIO);
	assert(SG2002_RTOS_INFO_CAPABILITIES(info) & SG2002_RTOS_CAP_ECHO);
	assert(SG2002_RTOS_GPIO_PIN(gpio) == 0x1234U);
	assert(SG2002_RTOS_GPIO_VALUE(gpio) == 1U);
	return 0;
}
