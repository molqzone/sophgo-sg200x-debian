#!/usr/bin/env bash
# Build the target Linux static library, CLI, and benchmark.
set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: build.sh <out-dir>" >&2
	exit 2
fi
: "${CC:?CC must name the target C compiler}"
: "${AR:?AR must name the target archiver}"

component_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
vendor_include=${VENDOR_INCLUDE:-$component_root/ports/osdrv/include}
out_dir=$1
common_flags=(
	-std=gnu11
	-O2
	-Wall
	-Wextra
	-Werror
	-pthread
	-I"$component_root/include"
	-I"$component_root/linux/include"
	-I"$vendor_include"
)

mkdir -p "$out_dir"
"$CC" "${common_flags[@]}" -c "$component_root/linux/src/sg2002_rtos.c" \
	-o "$out_dir/sg2002_rtos.o"
"$AR" rcs "$out_dir/libsg2002-rtos.a" "$out_dir/sg2002_rtos.o"
"$CC" "${common_flags[@]}" -static \
	"$component_root/tools/rtos-cmd.c" "$out_dir/libsg2002-rtos.a" \
	-o "$out_dir/rtos-cmd"
"$CC" "${common_flags[@]}" -static \
	"$component_root/tools/rtos-bench.c" "$out_dir/libsg2002-rtos.a" \
	-o "$out_dir/rtos-bench"
