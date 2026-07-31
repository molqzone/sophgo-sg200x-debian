#include "sg2002_rtos.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rtos_cmdqu.h"

typedef char sg2002_cmdqu_size_must_be_8[(sizeof(cmdqu_t) == 8) ? 1 : -1];

int sg2002_rtos_open(struct sg2002_rtos *rtos, const char *device_path)
{
	int fd;

	if (rtos == NULL)
		return -EINVAL;
	if (rtos->fd >= 0)
		return -EALREADY;
	if (device_path == NULL)
		device_path = SG2002_RTOS_DEVICE_PATH;

	fd = open(device_path, O_RDWR | O_DSYNC | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	rtos->fd = fd;
	return 0;
}

void sg2002_rtos_close(struct sg2002_rtos *rtos)
{
	if (rtos == NULL || rtos->fd < 0)
		return;
	close(rtos->fd);
	rtos->fd = -1;
}

int sg2002_rtos_call(struct sg2002_rtos *rtos, uint8_t command,
		     uint32_t request, uint16_t timeout_ms, uint32_t *response)
{
	cmdqu_t cmdq = {0};

	if (rtos == NULL || rtos->fd < 0 || response == NULL || timeout_ms == 0)
		return -EINVAL;
	if (command < SG2002_RTOS_CMD_FIRST || command > SG2002_RTOS_CMD_LAST)
		return -EINVAL;

	cmdq.ip_id = IP_SYSTEM;
	cmdq.cmd_id = command;
	cmdq.block = 1;
	cmdq.resv.mstime = timeout_ms;
	cmdq.param_ptr = request;

	if (ioctl(rtos->fd, RTOS_CMDQU_SEND_WAIT, &cmdq) < 0)
		return -errno;
	if (cmdq.ip_id != IP_SYSTEM || cmdq.cmd_id != command ||
	    cmdq.resv.valid.linux_valid != 0 ||
	    cmdq.resv.valid.rtos_valid != 1)
		return -EPROTO;

	*response = cmdq.param_ptr;
	return 0;
}

int sg2002_rtos_get_info(struct sg2002_rtos *rtos, uint32_t *info)
{
	uint32_t response = 0;
	int ret;

	if (info == NULL)
		return -EINVAL;
	ret = sg2002_rtos_call(rtos, SG2002_RTOS_CMD_GET_INFO, 0,
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &response);
	if (ret)
		return ret;
	if (!SG2002_RTOS_INFO_IS_VALID(response))
		return -EPROTO;

	*info = response;
	return 0;
}

int sg2002_rtos_ping(struct sg2002_rtos *rtos, uint32_t input,
		     uint32_t *result)
{
	if (result == NULL)
		return -EINVAL;
	return sg2002_rtos_call(rtos, SG2002_RTOS_CMD_PING, input,
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, result);
}

int sg2002_rtos_gpio_set(struct sg2002_rtos *rtos, uint16_t pin, int value)
{
	uint32_t response = 0;
	int ret;

	if (value != 0 && value != 1)
		return -EINVAL;
	ret = sg2002_rtos_call(rtos, SG2002_RTOS_CMD_GPIO_SET,
				SG2002_RTOS_GPIO_ENCODE(pin, value),
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &response);
	if (ret)
		return ret;
	if (response == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT)
		return -EINVAL;
	return response == (uint32_t)value ? 0 : -EPROTO;
}

int sg2002_rtos_gpio_get(struct sg2002_rtos *rtos, uint16_t pin, int *value)
{
	uint32_t response = 0;
	int ret;

	if (value == NULL)
		return -EINVAL;
	ret = sg2002_rtos_call(rtos, SG2002_RTOS_CMD_GPIO_GET,
				SG2002_RTOS_GPIO_ENCODE(pin, 0),
				SG2002_RTOS_DEFAULT_TIMEOUT_MS, &response);
	if (ret)
		return ret;
	if (response == SG2002_RTOS_RESPONSE_INVALID_ARGUMENT)
		return -EINVAL;
	if (response > 1U)
		return -EPROTO;

	*value = (int)response;
	return 0;
}
