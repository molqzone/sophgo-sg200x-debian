#!/usr/bin/env bash
# Purpose: stage the maintained SG2002 RTOS communication sources into a pinned SDK.
# Usage: bash prepare_sdk.sh <sdk-dir>
# Inputs: a checked-out duo-buildroot-sdk-v2 tree at the caller-selected revision.
# Outputs: staged FreeRTOS sources plus applied boot/mailbox trace patches.
# Side effects: overwrites only the listed SDK integration files and applies two patches.
# Idempotency: callers must reset tracked SDK files to the pinned commit before each run.

set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: prepare_sdk.sh <sdk-dir>" >&2
	exit 2
fi

ADDON_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SDK_DIR=$(cd "$1" && pwd)

if ! git -C "$SDK_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	echo "Not an SDK git tree: $SDK_DIR" >&2
	exit 1
fi

stage_file() {
	local source=$1
	local destination=$2

	if [ ! -f "$source" ]; then
		echo "Missing maintained source: $source" >&2
		exit 1
	fi
	mkdir -p "$(dirname "$destination")"
	cp -a "$source" "$destination"
}

stage_file \
	"$ADDON_ROOT/patches/freertos/cvitek/task/comm/src/riscv64/comm_main.c" \
	"$SDK_DIR/freertos/cvitek/task/comm/src/riscv64/comm_main.c"
stage_file \
	"$ADDON_ROOT/patches/freertos/cvitek/task/comm/CMakeLists.txt" \
	"$SDK_DIR/freertos/cvitek/task/comm/CMakeLists.txt"
stage_file \
	"$ADDON_ROOT/patches/freertos/cvitek/driver/gpio/include/gpio.h" \
	"$SDK_DIR/freertos/cvitek/driver/gpio/include/gpio.h"
stage_file \
	"$ADDON_ROOT/patches/freertos/cvitek/driver/gpio/src/gpio.c" \
	"$SDK_DIR/freertos/cvitek/driver/gpio/src/gpio.c"

stage_file \
	"$ADDON_ROOT/include/sg2002_rtos_protocol.h" \
	"$SDK_DIR/freertos/cvitek/task/comm/include/sg2002_rtos_protocol.h"
stage_file \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_app.h" \
	"$SDK_DIR/freertos/cvitek/task/comm/include/sg2002_rtos_app.h"
stage_file \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_mailbox.h" \
	"$SDK_DIR/freertos/cvitek/task/comm/include/sg2002_rtos_mailbox.h"
stage_file \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_app.c" \
	"$SDK_DIR/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_app.c"
stage_file \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_mailbox.c" \
	"$SDK_DIR/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_mailbox.c"

rm -f "$SDK_DIR/freertos/cvitek/driver/common/include/boot_trace.h"
git -C "$SDK_DIR" apply --check "$ADDON_ROOT/patches/0001-cvitek-c906l-boot-trace.patch"
git -C "$SDK_DIR" apply "$ADDON_ROOT/patches/0001-cvitek-c906l-boot-trace.patch"
git -C "$SDK_DIR" apply --check "$ADDON_ROOT/patches/0002-cvitek-c906l-mailbox-event-trace.patch"
git -C "$SDK_DIR" apply "$ADDON_ROOT/patches/0002-cvitek-c906l-mailbox-event-trace.patch"

grep -Fq "c906l_mailbox_irq_v2" \
	"$SDK_DIR/freertos/cvitek/task/comm/src/riscv64/comm_main.c"
grep -Fq "sg2002_rtos_mailbox_receive_from_isr" \
	"$SDK_DIR/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_mailbox.c"
grep -Fq "sg2002_rtos_mailbox_send" \
	"$SDK_DIR/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_mailbox.c"
