#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
board=${1:-maixcam}
output_dir=$(mkdir -p "${2:-$repo_root/output/component}" && cd "${2:-$repo_root/output/component}" && pwd)
image=${BUILDER_IMAGE:?BUILDER_IMAGE is required}

docker pull "$image"
docker run --rm --entrypoint /bin/bash \
	-e BOARD="$board" \
	-v "$repo_root:/workspace:ro" \
	-v "$output_dir:/output" \
	-w /workspace \
	"$image" -lc '
		set -euo pipefail
		make -C components/sg2002-ipc verify OUTPUT_DIR=/output/host-tests
		make -C components/sg2002-ipc firmware linux-tools \
			BOARD="$BOARD" CONFIG_ROOT=/workspace/configs \
			SDK_CACHE=/tmp/sdk-cache BUILD_ROOT=/tmp/sg2002-ipc-build \
			OUTPUT_DIR=/output RTOS_CROSS_COMPILE=/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf- \
			CROSS_COMPILE=/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-
	'
