#pragma once

#include <stddef.h>
#include <stdint.h>

void clean_dcache_range(uintptr_t address, size_t size);
void inv_dcache_range(uintptr_t address, size_t size);
