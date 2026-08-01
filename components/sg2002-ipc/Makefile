override COMPONENT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BOARD ?= maixcam
CONFIG_ROOT ?= $(COMPONENT_ROOT)/../../configs
SDK_CACHE ?= $(COMPONENT_ROOT)/.cache/sdk
BUILD_ROOT ?= $(COMPONENT_ROOT)/.cache/build/$(BOARD)
OUTPUT_DIR ?= $(COMPONENT_ROOT)/out
DESTDIR ?=
CC ?= cc
CROSS_COMPILE ?= riscv64-unknown-linux-musl-
RTOS_CROSS_COMPILE ?= riscv64-unknown-elf-
HOST_TOOLS ?= /host-tools
VENDOR_INCLUDE ?= $(COMPONENT_ROOT)/ports/osdrv/include

PLAN := $(COMPONENT_ROOT)/../../scripts/ci/plan.py
ifneq ("$(wildcard $(PLAN))","")
MEMMAP_FILE ?= $(shell python3 $(PLAN) --config-root $(CONFIG_ROOT) resolve --board $(BOARD) --path memmap.py 2>/dev/null)
endif

.PHONY: test firmware linux-tools install verify clean

test:
	@CC="$(CC)" VENDOR_INCLUDE="$(VENDOR_INCLUDE)" \
		bash "$(COMPONENT_ROOT)/tests/run_host_tests.sh" "$(OUTPUT_DIR)"

firmware:
	@test -n "$(MEMMAP_FILE)" || { echo "MEMMAP_FILE is required" >&2; exit 2; }
	@BOARD="$(BOARD)" CONFIG_ROOT="$(CONFIG_ROOT)" MEMMAP_FILE="$(MEMMAP_FILE)" \
		SDK_CACHE="$(SDK_CACHE)" BUILD_ROOT="$(BUILD_ROOT)" \
		OUTPUT_DIR="$(OUTPUT_DIR)" HOST_TOOLS="$(HOST_TOOLS)" \
		RTOS_CROSS_COMPILE="$(RTOS_CROSS_COMPILE)" \
		bash "$(COMPONENT_ROOT)/ports/duo-sdk/build.sh"

linux-tools:
	@CC="$(CROSS_COMPILE)gcc" AR="$(CROSS_COMPILE)ar" \
		VENDOR_INCLUDE="$(VENDOR_INCLUDE)" \
		bash "$(COMPONENT_ROOT)/linux/build.sh" "$(OUTPUT_DIR)/linux-tools"

install: firmware linux-tools
	@test -n "$(DESTDIR)" || { echo "DESTDIR is required" >&2; exit 2; }
	@install -d "$(DESTDIR)/lib/firmware" "$(DESTDIR)/usr/bin" \
		"$(DESTDIR)/usr/include" "$(DESTDIR)/usr/lib"
	@install -m 0644 "$(OUTPUT_DIR)/$(BOARD)_c906-mcu.elf" \
		"$(DESTDIR)/lib/firmware/c906-mcu.elf"
	@install -m 0755 "$(OUTPUT_DIR)/linux-tools/rtos-cmd" \
		"$(OUTPUT_DIR)/linux-tools/rtos-bench" "$(COMPONENT_ROOT)/tools/rtos-mode" \
		"$(DESTDIR)/usr/bin/"
	@install -m 0644 "$(OUTPUT_DIR)/linux-tools/libsg2002-rtos.a" \
		"$(DESTDIR)/usr/lib/"
	@install -m 0644 "$(COMPONENT_ROOT)/linux/include/sg2002_rtos.h" \
		"$(COMPONENT_ROOT)/include/sg2002_rtos_protocol.h" \
		"$(COMPONENT_ROOT)/include/sg2002_rtos_shm.h" \
		"$(DESTDIR)/usr/include/"
	@cp "$(OUTPUT_DIR)/linux-tools/rtos-cmd" "$(OUTPUT_DIR)/$(BOARD)_rtos-cmd"
	@cp "$(OUTPUT_DIR)/linux-tools/rtos-bench" "$(OUTPUT_DIR)/$(BOARD)_rtos-bench"
	@cp "$(OUTPUT_DIR)/linux-tools/libsg2002-rtos.a" \
		"$(OUTPUT_DIR)/$(BOARD)_libsg2002-rtos.a"
	@cp "$(COMPONENT_ROOT)/linux/include/sg2002_rtos.h" \
		"$(COMPONENT_ROOT)/include/sg2002_rtos_protocol.h" \
		"$(COMPONENT_ROOT)/include/sg2002_rtos_shm.h" "$(OUTPUT_DIR)/"

verify: test
	@python3 "$(COMPONENT_ROOT)/tools/verify_layout.py"

clean:
	@test -n "$(BUILD_ROOT)" && test "$(BUILD_ROOT)" != "/"
	@rm -rf "$(BUILD_ROOT)" "$(OUTPUT_DIR)"
