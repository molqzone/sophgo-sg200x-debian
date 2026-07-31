#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sg2002_rtos.h"

static void usage(void)
{
	fprintf(stderr,
		"Usage:\n"
		"  rtos-cmd info\n"
		"  rtos-cmd ping <value>\n"
		"  rtos-cmd gpio-set <pin> <0|1>\n"
		"  rtos-cmd gpio-get <pin>\n");
	exit(2);
}

static uint32_t parse_u32(const char *text)
{
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 0);

	if (text[0] == '\0' || end == NULL || *end != '\0' ||
	    value > UINT32_MAX) {
		fprintf(stderr, "invalid value: %s\n", text);
		exit(2);
	}
	return (uint32_t)value;
}

static int report_error(const char *operation, int error)
{
	fprintf(stderr, "%s failed: %s (%d)\n", operation,
		strerror(-error), error);
	return 1;
}

int main(int argc, char **argv)
{
	struct sg2002_rtos rtos = SG2002_RTOS_INITIALIZER;
	uint32_t input;
	uint32_t result;
	uint32_t info;
	uint16_t pin;
	int value;
	int ret;

	if (argc < 2)
		usage();

	ret = sg2002_rtos_open(&rtos, NULL);
	if (ret)
		return report_error("open " SG2002_RTOS_DEVICE_PATH, ret);

	if (strcmp(argv[1], "info") == 0) {
		if (argc != 2)
			usage();
		ret = sg2002_rtos_get_info(&rtos, &info);
		if (!ret)
			printf("protocol=%u.%u capabilities=0x%02x raw=0x%08x\n",
			       (unsigned int)SG2002_RTOS_INFO_MAJOR(info),
			       (unsigned int)SG2002_RTOS_INFO_MINOR(info),
			       (unsigned int)SG2002_RTOS_INFO_CAPABILITIES(info),
			       info);
	} else if (strcmp(argv[1], "ping") == 0) {
		if (argc != 3)
			usage();
		input = parse_u32(argv[2]);
		ret = sg2002_rtos_ping(&rtos, input, &result);
		if (!ret && result != (input ^ SG2002_RTOS_PING_XOR))
			ret = -EPROTO;
		if (!ret)
			printf("cmd_id=0x%x input=0x%x result=0x%x expected=0x%x\n",
			       SG2002_RTOS_CMD_PING, input, result,
			       input ^ SG2002_RTOS_PING_XOR);
	} else if (strcmp(argv[1], "gpio-set") == 0) {
		if (argc != 4 || parse_u32(argv[2]) > UINT16_MAX)
			usage();
		pin = (uint16_t)parse_u32(argv[2]);
		value = (int)parse_u32(argv[3]);
		ret = sg2002_rtos_gpio_set(&rtos, pin, value);
		if (!ret)
			printf("pin=0x%x value=%d\n", pin, value);
	} else if (strcmp(argv[1], "gpio-get") == 0) {
		if (argc != 3 || parse_u32(argv[2]) > UINT16_MAX)
			usage();
		pin = (uint16_t)parse_u32(argv[2]);
		ret = sg2002_rtos_gpio_get(&rtos, pin, &value);
		if (!ret)
			printf("pin=0x%x value=%d\n", pin, value);
	} else {
		usage();
	}

	sg2002_rtos_close(&rtos);
	return ret ? report_error(argv[1], ret) : 0;
}
