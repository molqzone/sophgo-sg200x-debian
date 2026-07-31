#!/usr/bin/env bash
# Purpose: compile and run deterministic host tests for the shared protocol layers.
# Usage: bash tests/run_host_tests.sh <vendor-include> <out-dir>
# Inputs: the pinned SDK cvi_mpi include directory and a native C compiler.
# Outputs: test executables and host-tests.txt in the requested output directory.
# Side effects: replaces only the requested output directory's test artifacts.
# Idempotency: safe to rerun with the same output directory.

set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "Usage: run_host_tests.sh <vendor-include> <out-dir>" >&2
	exit 2
fi

ADDON_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
VENDOR_INCLUDE=$(cd "$1" && pwd)
OUT_DIR=$2
CC=${CC:-cc}
COMMON_FLAGS=(
	-std=gnu11
	-O2
	-Wall
	-Wextra
	-Werror
	-I"$ADDON_ROOT/tests/stubs"
	-I"$ADDON_ROOT/include"
	-I"$ADDON_ROOT/freertos/include"
	-I"$ADDON_ROOT/linux/include"
	-I"$VENDOR_INCLUDE"
)

mkdir -p "$OUT_DIR"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_protocol.c" \
	-o "$OUT_DIR/test_protocol"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_rtos_app.c" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_app.c" \
	-o "$OUT_DIR/test_rtos_app"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_linux_comm.c" \
	"$ADDON_ROOT/linux/src/sg2002_rtos.c" \
	-Wl,--wrap=ioctl \
	-o "$OUT_DIR/test_linux_comm"

"$OUT_DIR/test_protocol"
"$OUT_DIR/test_rtos_app"
"$OUT_DIR/test_linux_comm"
printf 'protocol=PASS\nrtos_app=PASS\nlinux_comm=PASS\n' > \
	"$OUT_DIR/host-tests.txt"
