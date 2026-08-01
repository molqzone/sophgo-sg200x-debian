#!/usr/bin/env bash
# Compatibility entrypoint; the maintained build now lives in the component.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
component="$repo_root/components/sg2002-ipc"

exec make -C "$component" firmware \
	BOARD="${BOARD:-maixcam}" \
	CONFIG_ROOT="${CONFIG_ROOT:-$repo_root/configs}" \
	SDK_CACHE="${SDK_CACHE:-${SDK_DIR:-$repo_root/.cache/sg2002-sdk}}" \
	BUILD_ROOT="${BUILD_ROOT:-$repo_root/.cache/sg2002-build/${BOARD:-maixcam}}" \
	OUTPUT_DIR="${OUT_DIR:-$repo_root/output}" \
	RTOS_CROSS_COMPILE="${RTOS_CROSS_COMPILE:-riscv64-unknown-elf-}"
