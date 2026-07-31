#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>

#include "rtos_cmdqu.h"
#include "sg2002_rtos.h"

static unsigned int ioctl_calls;
static enum {
	FAKE_NORMAL,
	FAKE_IOCTL_ERROR,
	FAKE_BAD_DIRECTION,
	FAKE_BAD_INFO,
} fake_mode;

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	cmdqu_t *command;
	va_list arguments;

	assert(fd == 42);
	assert(request == RTOS_CMDQU_SEND_WAIT);
	va_start(arguments, request);
	command = va_arg(arguments, cmdqu_t *);
	va_end(arguments);
	ioctl_calls++;
	assert(command->ip_id == IP_SYSTEM);
	assert(command->block == 1);
	assert(command->resv.mstime == SG2002_RTOS_DEFAULT_TIMEOUT_MS);
	if (fake_mode == FAKE_IOCTL_ERROR) {
		errno = ETIMEDOUT;
		return -1;
	}

	switch (command->cmd_id) {
	case SG2002_RTOS_CMD_PING:
		command->param_ptr ^= SG2002_RTOS_PING_XOR;
		break;
	case SG2002_RTOS_CMD_GET_INFO:
		command->param_ptr = fake_mode == FAKE_BAD_INFO ? 0 :
			SG2002_RTOS_INFO_VALUE;
		break;
	case SG2002_RTOS_CMD_GPIO_SET:
		command->param_ptr = SG2002_RTOS_GPIO_VALUE(command->param_ptr);
		break;
	case SG2002_RTOS_CMD_GPIO_GET:
		command->param_ptr = 1;
		break;
	default:
		command->param_ptr = SG2002_RTOS_RESPONSE_UNSUPPORTED;
		break;
	}
	command->resv.valid.linux_valid =
		fake_mode == FAKE_BAD_DIRECTION ? 1 : 0;
	command->resv.valid.rtos_valid = 1;
	return 0;
}

int main(void)
{
	struct sg2002_rtos rtos = { .fd = 42 };
	uint32_t result;
	uint32_t info;
	unsigned int calls;
	int value;

	assert(sg2002_rtos_ping(&rtos, 0x13579bdfU, &result) == 0);
	assert(result == (0x13579bdfU ^ SG2002_RTOS_PING_XOR));
	assert(sg2002_rtos_get_info(&rtos, &info) == 0);
	assert(info == SG2002_RTOS_INFO_VALUE);
	assert(sg2002_rtos_gpio_set(&rtos, 0x0b03, 1) == 0);
	assert(sg2002_rtos_gpio_get(&rtos, 0x0b03, &value) == 0);
	assert(value == 1);
	fake_mode = FAKE_IOCTL_ERROR;
	assert(sg2002_rtos_ping(&rtos, 0, &result) == -ETIMEDOUT);
	fake_mode = FAKE_BAD_DIRECTION;
	assert(sg2002_rtos_ping(&rtos, 0, &result) == -EPROTO);
	fake_mode = FAKE_BAD_INFO;
	assert(sg2002_rtos_get_info(&rtos, &info) == -EPROTO);
	fake_mode = FAKE_NORMAL;

	calls = ioctl_calls;
	assert(sg2002_rtos_call(&rtos, SG2002_RTOS_CMD_FIRST - 1U, 0,
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &result) == -EINVAL);
	assert(ioctl_calls == calls);
	return 0;
}
