#!/usr/bin/env bash
set -euo pipefail

port_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
component_root=$(cd "$port_root/../.." && pwd)
# shellcheck disable=SC1091
source "$component_root/versions.env"

board=${BOARD:-maixcam}
config_root=${CONFIG_ROOT:-}
sdk_cache=${SDK_CACHE:-$component_root/.cache/sdk}
build_root=${BUILD_ROOT:-$component_root/.cache/build/$board}
output_dir=${OUTPUT_DIR:-$component_root/out}
memmap_file=${MEMMAP_FILE:-}
host_tools=${HOST_TOOLS:-/host-tools}

: "${RTOS_SDK_REPO:?RTOS_SDK_REPO is required}"
: "${RTOS_SDK_COMMIT:?RTOS_SDK_COMMIT is required}"
: "${memmap_file:?MEMMAP_FILE is required}"

board_chain=" $board "
plan_tool=$component_root/../../scripts/ci/plan.py
if [[ -n $config_root && -f $plan_tool ]]; then
	board_chain=" $(python3 "$plan_tool" --config-root "$config_root" \
		board-chain --board "$board" --order precedence) "
fi

case "$board_chain" in
	*" maixcam "*|*" licheervnano "*|*" duo256 "*)
		sdk_board=${SDK_BOARD:-milkv-duo256m-musl-riscv64-sd}
		sdk_memmap=${SDK_MEMMAP:-build/boards/cv181x/sg2002_milkv_duo256m_musl_riscv64_sd/memmap.py}
		;;
	*" duos "*)
		sdk_board=${SDK_BOARD:-milkv-duos-musl-riscv64-sd}
		sdk_memmap=${SDK_MEMMAP:-build/boards/cv181x/sg2000_milkv_duos_musl_riscv64_sd/memmap.py}
		;;
	*)
		: "${SDK_BOARD:?SDK_BOARD is required for board $board}"
		: "${SDK_MEMMAP:?SDK_MEMMAP is required for board $board}"
		sdk_board=$SDK_BOARD
		sdk_memmap=$SDK_MEMMAP
		;;
esac

mkdir -p "$sdk_cache" "$build_root" "$output_dir"
mirror="$sdk_cache/duo-buildroot-sdk-v2.git"
sdk_dir="$build_root/sdk"
if [ ! -d "$mirror/objects" ]; then
	git init --bare "$mirror"
	git --git-dir="$mirror" remote add origin "$RTOS_SDK_REPO"
fi
git --git-dir="$mirror" remote set-url origin "$RTOS_SDK_REPO"
if ! git --git-dir="$mirror" cat-file -e "$RTOS_SDK_COMMIT^{commit}" 2>/dev/null; then
	git --git-dir="$mirror" fetch --depth 1 origin "$RTOS_SDK_COMMIT"
fi
if [ -e "$sdk_dir/.git" ] && [ "$(git -C "$sdk_dir" rev-parse HEAD)" != "$RTOS_SDK_COMMIT" ]; then
	git --git-dir="$mirror" worktree remove --force "$sdk_dir" || rm -rf "$sdk_dir"
	git --git-dir="$mirror" worktree prune
fi
if [ ! -e "$sdk_dir/.git" ]; then
	rm -rf "$sdk_dir"
	git --git-dir="$mirror" worktree add --force --detach "$sdk_dir" "$RTOS_SDK_COMMIT"
fi
test "$(git -C "$sdk_dir" rev-parse HEAD)" = "$RTOS_SDK_COMMIT"
source_date_epoch=$(git -C "$sdk_dir" show -s --format=%ct "$RTOS_SDK_COMMIT")
case "$source_date_epoch" in
	*[!0-9]*|'') echo "Invalid SDK commit timestamp: $source_date_epoch" >&2; exit 1 ;;
esac

source_hash=$(
	{
		find "$component_root/include" "$component_root/core" \
			"$component_root/firmware" "$port_root" -type f -print0
		printf '%s\0' "$component_root/versions.env" "$memmap_file"
	} |
		sort -z |
		xargs -0 sha256sum |
		awk '{print $1}' |
		sha256sum | awk '{print $1}'
)
stamp="$build_root/component-source.sha256"
firmware_elf="$sdk_dir/freertos/cvitek/install/bin/cvirtos.elf"
firmware_bin="$sdk_dir/freertos/cvitek/install/bin/cvirtos.bin"

if [ ! -s "$firmware_elf" ] || [ ! -s "$firmware_bin" ] || \
	[ ! -f "$stamp" ] || [ "$(cat "$stamp")" != "$source_hash" ]; then
	git -C "$sdk_dir" reset --hard "$RTOS_SDK_COMMIT"
	git -C "$sdk_dir" clean -ffd
	rm -rf "$sdk_dir/freertos/cvitek/build" "$sdk_dir/freertos/cvitek/install"
	cp "$memmap_file" "$sdk_dir/$sdk_memmap"
	bash "$port_root/prepare.sh" "$sdk_dir"
	export PATH="$host_tools/gcc/riscv64-elf-x86_64/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
	export SOURCE_DATE_EPOCH="$source_date_epoch"
	pushd "$sdk_dir" >/dev/null
	export TOP="$sdk_dir"
	unset MAKEFLAGS MAKEOVERRIDES MFLAGS
	set +u
	source build/envsetup_milkv.sh "$sdk_board" >/dev/null
	build_rtos
	set -u
	popd >/dev/null
	test -s "$firmware_elf"
	test -s "$firmware_bin"
	printf '%s\n' "$source_hash" >"$stamp"
	cache_result=miss
else
	cache_result=hit
fi

cp "$firmware_elf" "$output_dir/${board}_c906-mcu.elf"
cp "$firmware_bin" "$output_dir/${board}_c906-mcu.bin"
python3 "$component_root/tools/check_trace_layout.py" \
	--readelf "$host_tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-readelf" \
	"$firmware_elf" >"$output_dir/${board}_boot-trace-layout.json"
printf '%s\n' "$RTOS_SDK_COMMIT" >"$output_dir/${board}_rtos-sdk-commit.txt"
(
	cd "$output_dir"
	sha256sum "${board}_c906-mcu.elf" "${board}_c906-mcu.bin" >"${board}_firmware-SHA256SUMS.txt"
)
printf 'firmware-cache=%s source=%s\n' "$cache_result" "$source_hash"
