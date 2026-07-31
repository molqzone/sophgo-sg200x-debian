#pragma once

#include "rtos_cmdqu.h"

#define SG2002_RTOS_MAILBOX_SLOT_COUNT 8U
#define SG2002_RTOS_MAILBOX_POLL_INTERVAL_US 1000U

/** Initialize the SG2002 mailbox registers and shared hardware spinlock. */
void sg2002_rtos_mailbox_init(void);

/**
 * Copy Linux-owned mailbox slots into caller storage and release the slots.
 *
 * Returns the number of commands copied. The hardware lock is released before
 * the caller dispatches commands to FreeRTOS queues.
 */
unsigned int sg2002_rtos_mailbox_receive(cmdqu_t *commands,
					 unsigned int capacity);

/** Post one RTOS-owned reply to the selected Linux CPU mailbox. */
int sg2002_rtos_mailbox_send(const cmdqu_t *command, int send_to_cpu);
