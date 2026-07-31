#pragma once

#include <stdint.h>

extern unsigned char test_shm_region[];

#define CVIMMAP_RTOS_SHM_ADDR ((uintptr_t)test_shm_region)
