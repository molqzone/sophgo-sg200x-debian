#pragma once

#include <stdint.h>

extern unsigned char test_shm_region[];

#define SG2002_RTOS_SHM_BASE ((uintptr_t)test_shm_region)
