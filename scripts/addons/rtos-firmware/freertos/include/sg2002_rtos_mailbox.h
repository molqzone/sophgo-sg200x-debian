#pragma once

#include "rtos_cmdqu.h"

#define SG2002_RTOS_MAILBOX_SLOT_COUNT 8U
#define SG2002_RTOS_MAILBOX_RECOVERY_INTERVAL_MS 100U

struct sg2002_rtos_mailbox_diagnostics {
	unsigned char enabled;
	unsigned char raw;
	unsigned char mask;
	unsigned char pending;
};

/** Initialize the SG2002 mailbox registers and shared hardware spinlock. */
void sg2002_rtos_mailbox_init(void);

/** Clear stale receiver state and explicitly unmask the C906L mailbox. */
void sg2002_rtos_mailbox_enable_receiver(void);

/** Read the C906L mailbox receiver registers without changing them. */
void sg2002_rtos_mailbox_get_diagnostics(
	struct sg2002_rtos_mailbox_diagnostics *diagnostics);

/**
 * Copy Linux-owned mailbox slots into caller storage and release the slots.
 *
 * Returns the number of commands copied. The hardware lock is released before
 * the caller dispatches commands to FreeRTOS queues.
 */
unsigned int sg2002_rtos_mailbox_receive(cmdqu_t *commands,
					 unsigned int capacity);

/**
 * Copy pending Linux-owned slots from the mailbox interrupt handler.
 *
 * The producer publishes each complete slot before ringing the doorbell, so
 * the ISR does not take the cross-core hardware spinlock.
 */
unsigned int sg2002_rtos_mailbox_receive_from_isr(cmdqu_t *commands,
						  unsigned int capacity);

/** Post one RTOS-owned reply to the selected Linux CPU mailbox. */
int sg2002_rtos_mailbox_send(const cmdqu_t *command, int send_to_cpu);
