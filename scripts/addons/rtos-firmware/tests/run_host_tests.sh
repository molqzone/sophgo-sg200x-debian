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
	-pthread
	-I"$ADDON_ROOT/tests/stubs"
	-I"$ADDON_ROOT/include"
	-I"$ADDON_ROOT/freertos/include"
	-I"$ADDON_ROOT/linux/include"
	-I"$VENDOR_INCLUDE"
)

mkdir -p "$OUT_DIR"

if grep -Eq '#include "(rtos_cmdqu|sg2002_rtos_mailbox|sg2002_rtos_shm|sg2002_rtos_ring)\.h"' \
	"$ADDON_ROOT/freertos/include/sg2002_rtos_app.h" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_app.c"; then
	echo "application layer depends on transport internals" >&2
	exit 1
fi
if grep -Fq '#include "sg2002_rtos_protocol.h"' \
	"$ADDON_ROOT/common/sg2002_rtos_ring.c" \
	"$ADDON_ROOT/include/sg2002_rtos_ring.h"; then
	echo "generic ring depends on the application protocol" >&2
	exit 1
fi
if grep -Fq '#include "rtos_cmdqu.h"' \
	"$ADDON_ROOT/linux/include/sg2002_rtos.h"; then
	echo "public Linux API exposes the vendor CMDQU transport" >&2
	exit 1
fi
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_protocol.c" \
	-o "$OUT_DIR/test_protocol"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_shm_ring.c" \
	"$ADDON_ROOT/common/sg2002_rtos_ring.c" \
	-o "$OUT_DIR/test_shm_ring"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_rtos_app.c" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_app.c" \
	-o "$OUT_DIR/test_rtos_app"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_shm_transport.c" \
	"$ADDON_ROOT/common/sg2002_rtos_ring.c" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_app.c" \
	"$ADDON_ROOT/freertos/src/sg2002_rtos_shm_transport.c" \
	-o "$OUT_DIR/test_shm_transport"
"$CC" "${COMMON_FLAGS[@]}" \
	"$ADDON_ROOT/tests/test_linux_comm.c" \
	"$ADDON_ROOT/linux/src/sg2002_rtos.c" \
	-Wl,--wrap=ioctl -Wl,--wrap=open -Wl,--wrap=close \
	-o "$OUT_DIR/test_linux_comm"

"$OUT_DIR/test_protocol"
"$OUT_DIR/test_shm_ring"
"$OUT_DIR/test_rtos_app"
"$OUT_DIR/test_shm_transport"
"$OUT_DIR/test_linux_comm"
printf 'layering=PASS\nprotocol=PASS\nshm_ring=PASS\nrtos_app=PASS\nshm_transport=PASS\nlinux_comm=PASS\n' > \
	"$OUT_DIR/host-tests.txt"
