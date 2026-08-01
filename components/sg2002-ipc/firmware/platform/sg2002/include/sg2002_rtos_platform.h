#pragma once

#include <stdint.h>

extern unsigned char __rtos_shm_start[];

#define SG2002_RTOS_SHM_BASE ((uintptr_t)__rtos_shm_start)
