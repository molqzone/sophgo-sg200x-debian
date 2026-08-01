#!/usr/bin/env bash
# Compile and run deterministic host tests without downloading the vendor SDK.
set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: run_host_tests.sh <out-dir>" >&2
	exit 2
fi

component_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
vendor_include=${VENDOR_INCLUDE:-$component_root/ports/osdrv/include}
out_dir=$1
cc=${CC:-cc}
common_flags=(
	-std=gnu11
	-O2
	-Wall
	-Wextra
	-Werror
	-pthread
	-I"$component_root/tests/stubs"
	-I"$component_root/include"
	-I"$component_root/firmware/app/include"
	-I"$component_root/firmware/transport/include"
	-I"$component_root/firmware/platform/sg2002/include"
	-I"$component_root/linux/include"
	-I"$vendor_include"
)

mkdir -p "$out_dir"

if grep -Eq '#include "(rtos_cmdqu|sg2002_rtos_mailbox|sg2002_rtos_shm|sg2002_rtos_ring)\.h"' \
	"$component_root/firmware/app/include/sg2002_rtos_app.h" \
	"$component_root/firmware/app/sg2002_rtos_app.c"; then
	echo "application layer depends on transport internals" >&2
	exit 1
fi
if grep -Fq '#include "sg2002_rtos_protocol.h"' \
	"$component_root/core/sg2002_rtos_ring.c" \
	"$component_root/include/sg2002_rtos_ring.h"; then
	echo "generic ring depends on the application protocol" >&2
	exit 1
fi
if grep -Fq '#include "rtos_cmdqu.h"' \
	"$component_root/linux/include/sg2002_rtos.h"; then
	echo "public Linux API exposes the vendor CMDQU transport" >&2
	exit 1
fi

"$cc" "${common_flags[@]}" "$component_root/tests/test_protocol.c" \
	-o "$out_dir/test_protocol"
"$cc" "${common_flags[@]}" "$component_root/tests/test_shm_ring.c" \
	"$component_root/core/sg2002_rtos_ring.c" -o "$out_dir/test_shm_ring"
"$cc" "${common_flags[@]}" "$component_root/tests/test_rtos_app.c" \
	"$component_root/firmware/app/sg2002_rtos_app.c" -o "$out_dir/test_rtos_app"
"$cc" "${common_flags[@]}" "$component_root/tests/test_shm_transport.c" \
	"$component_root/core/sg2002_rtos_ring.c" \
	"$component_root/firmware/app/sg2002_rtos_app.c" \
	"$component_root/firmware/transport/sg2002_rtos_shm_transport.c" \
	-o "$out_dir/test_shm_transport"
"$cc" "${common_flags[@]}" "$component_root/tests/test_linux_comm.c" \
	"$component_root/linux/src/sg2002_rtos.c" \
	-Wl,--wrap=ioctl -Wl,--wrap=open -Wl,--wrap=close \
	-o "$out_dir/test_linux_comm"

"$out_dir/test_protocol"
"$out_dir/test_shm_ring"
"$out_dir/test_rtos_app"
"$out_dir/test_shm_transport"
"$out_dir/test_linux_comm"
printf 'layering=PASS\nprotocol=PASS\nshm_ring=PASS\nrtos_app=PASS\nshm_transport=PASS\nlinux_comm=PASS\n' > \
	"$out_dir/host-tests.txt"
