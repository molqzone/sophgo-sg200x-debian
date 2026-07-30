#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rtos_cmdqu.h"

static void usage(void)
{
	fprintf(stderr,
		"Usage:\n"
		"  rtos-cmd ping <hex-value>\n"
		"  rtos-cmd gpio-set <pin-hex> <0|1>\n"
		"  rtos-cmd gpio-get <pin-hex>\n");
	exit(2);
}

static unsigned int parse_u32(const char *s)
{
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 0);

	if (s[0] == '\0' || (end && *end != '\0') || v > 0xFFFFFFFFUL) {
		fprintf(stderr, "invalid value: %s\n", s);
		exit(2);
	}
	return (unsigned int)v;
}

int main(int argc, char **argv)
{
	int fd;
	cmdqu_t cmdq;
	unsigned int pin;
	unsigned int ping_input = 0;
	int verify_ping = 0;

	if (argc < 3)
		usage();

	fd = open("/dev/" RTOS_CMDQU_DEV_NAME, O_RDWR | O_DSYNC);
	if (fd < 0) {
		perror("open /dev/cvi-rtos-cmdqu");
		return 1;
	}

	memset(&cmdq, 0, sizeof(cmdq));
	cmdq.ip_id = IP_SYSTEM;
	cmdq.block = 1;
	cmdq.resv.mstime = 3000;

	if (strcmp(argv[1], "ping") == 0) {
		cmdq.cmd_id = RTOS_USER_CMD_PING;
		ping_input = parse_u32(argv[2]);
		cmdq.param_ptr = ping_input;
		verify_ping = 1;
	} else if (strcmp(argv[1], "gpio-set") == 0) {
		if (argc != 4)
			usage();
		pin = parse_u32(argv[2]);
		cmdq.cmd_id = RTOS_USER_CMD_GPIO_SET;
		cmdq.param_ptr = (pin << 16) | (parse_u32(argv[3]) & 0x1);
	} else if (strcmp(argv[1], "gpio-get") == 0) {
		pin = parse_u32(argv[2]);
		cmdq.cmd_id = RTOS_USER_CMD_GPIO_GET;
		cmdq.param_ptr = (pin << 16);
	} else {
		usage();
	}

	if (ioctl(fd, RTOS_CMDQU_SEND_WAIT, &cmdq) < 0) {
		perror("ioctl RTOS_CMDQU_SEND_WAIT");
		close(fd);
		return 1;
	}

	if (verify_ping && cmdq.param_ptr != (ping_input ^ RTOS_USER_PING_XOR)) {
		fprintf(stderr,
			"RTOS ping mismatch: input=0x%x result=0x%x expected=0x%x\n",
			ping_input, cmdq.param_ptr, ping_input ^ RTOS_USER_PING_XOR);
		close(fd);
		return 1;
	}

	if (verify_ping) {
		printf("cmd_id=0x%x input=0x%x result=0x%x expected=0x%x\n",
		       cmdq.cmd_id, ping_input, cmdq.param_ptr,
		       ping_input ^ RTOS_USER_PING_XOR);
	} else {
		printf("cmd_id=0x%x param=0x%x\n", cmdq.cmd_id, cmdq.param_ptr);
	}
	close(fd);
	return 0;
}
