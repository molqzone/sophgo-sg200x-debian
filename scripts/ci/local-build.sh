#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
target=${1:-verify}
shift || true
board=maixcam
storage=sd
output=output
inside=0
proxy_http=${http_proxy:-${HTTP_PROXY:-}}
proxy_https=${https_proxy:-${HTTPS_PROXY:-}}

export HTTP_PROXY=$proxy_http
export HTTPS_PROXY=$proxy_https
export http_proxy=$proxy_http
export https_proxy=$proxy_https

while (($#)); do
	case "$1" in
		--board) board=$2; shift 2 ;;
		--storage) storage=$2; shift 2 ;;
		--output) output=$2; shift 2 ;;
		--inside) inside=1; shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

if [[ ${IN_CONTAINER:-0} != 1 && $inside != 1 ]]; then
	command -v docker >/dev/null 2>&1 || { echo "docker is required" >&2; exit 1; }
	# shellcheck disable=SC1091
	source "$repo_root/toolchain.env"
	image=${BUILDER_IMAGE:-$(bash "$script_dir/toolchain-ref.sh")}
	if ! docker image inspect "$image" >/dev/null 2>&1; then
		docker build \
			--build-arg "BUILDER_BASE_IMAGE=$BUILDER_BASE_IMAGE" \
			--build-arg "DEBIAN_SNAPSHOT=$DEBIAN_SNAPSHOT" \
			--build-arg "HOST_TOOLS_REPO=$HOST_TOOLS_REPO" \
			--build-arg "HOST_TOOLS_COMMIT=$HOST_TOOLS_COMMIT" \
			-t "$image" -f "$repo_root/scripts/Dockerfile" "$repo_root"
	fi
	for volume in sg2002-sdk sg2002-build sg2002-ccache; do
		docker volume inspect "$volume" >/dev/null 2>&1 || docker volume create "$volume" >/dev/null
	done
	mkdir -p "$repo_root/$output"
	exec docker run --rm --privileged \
		-e IN_CONTAINER=1 \
		-e CCACHE_DIR=/ccache \
		-e HTTP_PROXY="$proxy_http" \
		-e HTTPS_PROXY="$proxy_https" \
		-e http_proxy="$proxy_http" \
		-e https_proxy="$proxy_https" \
		-v "$repo_root:/workspace:ro" \
		-v "$repo_root/scripts:/builder:ro" \
		-v "$repo_root/configs:/configs:ro" \
		-v "$repo_root/$output:/output" \
		-v sg2002-sdk:/sdk-cache \
		-v sg2002-build:/build-cache \
		-v sg2002-ccache:/ccache \
		-w /workspace --entrypoint /bin/bash \
		"$image" \
		/workspace/scripts/ci/local-build.sh "$target" --inside --board "$board" --storage "$storage" --output /output
fi

case "$board" in
	*[!A-Za-z0-9_.-]*|'') echo "invalid board: $board" >&2; exit 2 ;;
esac
case "$storage" in
	sd|emmc) ;;
	*) echo "invalid storage: $storage" >&2; exit 2 ;;
esac

python3 /workspace/scripts/ci/plan.py validate
expected_artifact=$(python3 /workspace/scripts/ci/plan.py artifact --board "$board" --storage "$storage")
build_root="/build-cache/boards/$board-$storage"
rootfs="$build_root/rootfs"
mkdir -p "$build_root" /output /sdk-cache /ccache
ln -sfn "$rootfs" /rootfs
bash /workspace/scripts/ci/prepare-ccache-toolchains.sh /build-cache/toolchains
export CROSS_COMPILE=/build-cache/toolchains/musl/bin/riscv64-unknown-linux-musl-
export RTOS_CROSS_COMPILE=/build-cache/toolchains/elf/bin/riscv64-unknown-elf-
export BUILDDIR="$build_root"
export ROOTFS="$rootfs"
export SDK_CACHE=/sdk-cache
export OUTPUT_DIR=/output
export CONFIG_ROOT=/configs

run_cache() {
	python3 /workspace/scripts/ci/cache.py \
		--repo /workspace --config-root /configs --build-root "$build_root" \
		--board "$board" --storage "$storage" --layers "$@"
}

run_builder_make() {
	make -C /builder \
		BOARD="$board" STORAGE_TYPE="$storage" \
		BUILDDIR="$build_root" ROOTFS="$rootfs" \
		VERSION_FILE=/workspace/versions.env \
		CONFIG_ROOT=/configs COMPONENTS_ROOT=/workspace/components \
		OUTPUT_DIR=/output CROSS_COMPILE="$CROSS_COMPILE" \
		RTOS_CROSS_COMPILE="$RTOS_CROSS_COMPILE" "$@"
}

case "$target" in
	toolchain)
		ccache --show-stats
		;;
	test)
		make -C /workspace/components/sg2002-ipc test OUTPUT_DIR=/output/host-tests
		;;
	firmware)
		run_cache firmware
	make -C /workspace/components/sg2002-ipc firmware \
		BOARD="$board" CONFIG_ROOT=/configs SDK_CACHE=/sdk-cache \
		BUILD_ROOT="$build_root/components/sg2002-ipc" OUTPUT_DIR=/output \
		HOST_TOOLS=/host-tools RTOS_CROSS_COMPILE="$RTOS_CROSS_COMPILE"
		;;
	modules)
		run_cache linux osdrv
		run_builder_make linux osdrv
		;;
	image)
		run_cache firmware linux osdrv middleware boot rootfs
		run_builder_make image
		test -s "/output/$expected_artifact"
		;;
	verify)
		make -C /workspace/components/sg2002-ipc test OUTPUT_DIR=/output/host-tests
		python3 /workspace/scripts/ci/validate.py --board "$board" --storage "$storage" --output /output --allow-missing-image
		;;
	*)
		echo "unsupported target: $target" >&2
		exit 2
		;;
esac
