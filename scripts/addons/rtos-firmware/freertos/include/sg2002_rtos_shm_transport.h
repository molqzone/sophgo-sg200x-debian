#pragma once

#include <stdint.h>

enum sg2002_rtos_shm_process_result {
	SG2002_RTOS_SHM_PROCESS_OK = 0,
	SG2002_RTOS_SHM_PROCESS_NOT_READY = 1,
	SG2002_RTOS_SHM_PROCESS_CORRUPT = 2,
};

/** Initialize local C906L shared-memory transport state. */
void sg2002_rtos_shm_transport_init(void);

/**
 * Attach to the Linux generation and drain all currently serviceable requests.
 *
 * This function has one task-context caller. It never runs in the mailbox ISR.
 */
enum sg2002_rtos_shm_process_result sg2002_rtos_shm_process(
	unsigned int *processed);

/** Return the generation currently acknowledged by C906L, or zero. */
uint32_t sg2002_rtos_shm_generation(void);
