#!/usr/bin/env bash
# Stage maintained SG2002 IPC sources into a reset pinned SDK worktree.
set -euo pipefail

if [ "$#" -ne 1 ]; then
	echo "Usage: prepare.sh <sdk-dir>" >&2
	exit 2
fi

port_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
component_root=$(cd "$port_root/../.." && pwd)
sdk_dir=$(cd "$1" && pwd)

if ! git -C "$sdk_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	echo "Not an SDK git tree: $sdk_dir" >&2
	exit 1
fi

stage_file() {
	local source=$1
	local destination=$2
	test -f "$source" || { echo "Missing maintained source: $source" >&2; exit 1; }
	mkdir -p "$(dirname "$destination")"
	cp -a "$source" "$destination"
}

stage_file "$port_root/overlay/freertos/cvitek/task/comm/src/riscv64/comm_main.c" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/comm_main.c"
stage_file "$port_root/overlay/freertos/cvitek/task/comm/CMakeLists.txt" \
	"$sdk_dir/freertos/cvitek/task/comm/CMakeLists.txt"
stage_file "$port_root/overlay/freertos/cvitek/driver/gpio/include/gpio.h" \
	"$sdk_dir/freertos/cvitek/driver/gpio/include/gpio.h"
stage_file "$port_root/overlay/freertos/cvitek/driver/gpio/src/gpio.c" \
	"$sdk_dir/freertos/cvitek/driver/gpio/src/gpio.c"

stage_file "$component_root/include/sg2002_rtos_protocol.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_protocol.h"
stage_file "$component_root/include/sg2002_rtos_shm.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_shm.h"
stage_file "$component_root/include/sg2002_rtos_ring.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_ring.h"
stage_file "$component_root/firmware/app/include/sg2002_rtos_app.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_app.h"
stage_file "$component_root/firmware/transport/include/sg2002_rtos_mailbox.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_mailbox.h"
stage_file "$component_root/firmware/platform/sg2002/include/sg2002_rtos_platform.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_platform.h"
stage_file "$component_root/firmware/transport/include/sg2002_rtos_shm_transport.h" \
	"$sdk_dir/freertos/cvitek/task/comm/include/sg2002_rtos_shm_transport.h"
stage_file "$component_root/firmware/app/sg2002_rtos_app.c" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_app.c"
stage_file "$component_root/firmware/transport/sg2002_rtos_mailbox.c" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_mailbox.c"
stage_file "$component_root/core/sg2002_rtos_ring.c" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_ring.c"
stage_file "$component_root/firmware/transport/sg2002_rtos_shm_transport.c" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_shm_transport.c"

rm -f "$sdk_dir/freertos/cvitek/driver/common/include/boot_trace.h"
for patch in "$port_root"/patches/*.patch; do
	git -C "$sdk_dir" apply --check "$patch"
	git -C "$sdk_dir" apply "$patch"
done

grep -Fq "c906l_mailbox_irq_v2" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/comm_main.c"
grep -Fq "sg2002_rtos_mailbox_receive_from_isr" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_mailbox.c"
grep -Fq "sg2002_rtos_mailbox_send" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_mailbox.c"
grep -Fq "SG2002_RTOS_SHM_DOORBELL_COMMAND" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/comm_main.c"
grep -Fq "sg2002_rtos_shm_process" \
	"$sdk_dir/freertos/cvitek/task/comm/src/riscv64/sg2002_rtos_shm_transport.c"
