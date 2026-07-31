#!/usr/bin/env bash
# Purpose: build the Linux SG2002 RTOS library, CLI, and benchmark.
# Usage: CC=<compiler> AR=<archiver> bash build_linux_tools.sh <vendor-include> <out-dir>
# Inputs: the SDK cvi_mpi include directory and a C11-capable Linux toolchain.
# Outputs: libsg2002-rtos.a, rtos-cmd, and rtos-bench in the output directory.
# Side effects: replaces files with those names in the requested output directory.
# Idempotency: repeated runs with identical inputs replace outputs deterministically.

set -euo pipefail

if [ "$#" -ne 2 ]; then
	echo "Usage: build_linux_tools.sh <vendor-include> <out-dir>" >&2
	exit 2
fi
: "${CC:?CC must name the target C compiler}"
: "${AR:?AR must name the target archiver}"

ADDON_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
VENDOR_INCLUDE=$(cd "$1" && pwd)
OUT_DIR=$2
COMMON_FLAGS=(
	-std=gnu11
	-O2
	-Wall
	-Wextra
	-Werror
	-pthread
	-I"$ADDON_ROOT/include"
	-I"$ADDON_ROOT/linux/include"
	-I"$VENDOR_INCLUDE"
)

mkdir -p "$OUT_DIR"
"$CC" "${COMMON_FLAGS[@]}" -c \
	"$ADDON_ROOT/linux/src/sg2002_rtos.c" \
	-o "$OUT_DIR/sg2002_rtos.o"
"$AR" rcs "$OUT_DIR/libsg2002-rtos.a" "$OUT_DIR/sg2002_rtos.o"
"$CC" "${COMMON_FLAGS[@]}" -static \
	"$ADDON_ROOT/tools/rtos-cmd.c" "$OUT_DIR/libsg2002-rtos.a" \
	-o "$OUT_DIR/rtos-cmd"
"$CC" "${COMMON_FLAGS[@]}" -static \
	"$ADDON_ROOT/tools/rtos-bench.c" "$OUT_DIR/libsg2002-rtos.a" \
	-o "$OUT_DIR/rtos-bench"
