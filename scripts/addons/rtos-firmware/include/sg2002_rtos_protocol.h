#pragma once

#include <stdint.h>

/** First command ID reserved for the SG2002 application protocol. */
#define SG2002_RTOS_CMD_FIRST 0x60U
/** Last command ID available without changing the mailbox transport. */
#define SG2002_RTOS_CMD_LAST 0x7eU

enum sg2002_rtos_command {
	SG2002_RTOS_CMD_PING = SG2002_RTOS_CMD_FIRST,
	SG2002_RTOS_CMD_GPIO_SET,
	SG2002_RTOS_CMD_GPIO_GET,
	SG2002_RTOS_CMD_GET_INFO,
};

#define SG2002_RTOS_PING_XOR 0xa5a55a5aU

#define SG2002_RTOS_PROTOCOL_MAJOR 1U
#define SG2002_RTOS_PROTOCOL_MINOR 0U
#define SG2002_RTOS_INFO_MAGIC 0xa5U
#define SG2002_RTOS_CAP_GPIO (1U << 0)
#define SG2002_RTOS_CAPABILITIES SG2002_RTOS_CAP_GPIO

#define SG2002_RTOS_INFO_ENCODE(major, minor, capabilities) \
	((((uint32_t)SG2002_RTOS_INFO_MAGIC) << 24) | \
	 (((uint32_t)(major) & 0xffU) << 16) | \
	 (((uint32_t)(minor) & 0xffU) << 8) | \
	 ((uint32_t)(capabilities) & 0xffU))
#define SG2002_RTOS_INFO_VALUE \
	SG2002_RTOS_INFO_ENCODE(SG2002_RTOS_PROTOCOL_MAJOR, \
				SG2002_RTOS_PROTOCOL_MINOR, \
				SG2002_RTOS_CAPABILITIES)
#define SG2002_RTOS_INFO_IS_VALID(value) \
	((((uint32_t)(value) >> 24) & 0xffU) == SG2002_RTOS_INFO_MAGIC)
#define SG2002_RTOS_INFO_MAJOR(value) (((uint32_t)(value) >> 16) & 0xffU)
#define SG2002_RTOS_INFO_MINOR(value) (((uint32_t)(value) >> 8) & 0xffU)
#define SG2002_RTOS_INFO_CAPABILITIES(value) ((uint32_t)(value) & 0xffU)

#define SG2002_RTOS_GPIO_PIN_SHIFT 16U
#define SG2002_RTOS_GPIO_PIN_MASK 0xffffU
#define SG2002_RTOS_GPIO_VALUE_MASK 0x1U
#define SG2002_RTOS_GPIO_ENCODE(pin, value) \
	((((uint32_t)(pin) & SG2002_RTOS_GPIO_PIN_MASK) << \
	  SG2002_RTOS_GPIO_PIN_SHIFT) | \
	 ((uint32_t)(value) & SG2002_RTOS_GPIO_VALUE_MASK))
#define SG2002_RTOS_GPIO_PIN(value) \
	(((uint32_t)(value) >> SG2002_RTOS_GPIO_PIN_SHIFT) & \
	 SG2002_RTOS_GPIO_PIN_MASK)
#define SG2002_RTOS_GPIO_VALUE(value) \
	((uint32_t)(value) & SG2002_RTOS_GPIO_VALUE_MASK)

/* These values are reserved across application commands as error replies. */
#define SG2002_RTOS_RESPONSE_INVALID_ARGUMENT 0xfffffffeU
#define SG2002_RTOS_RESPONSE_UNSUPPORTED 0xffffffffU
