#!/usr/bin/env bash
set -euo pipefail

cache_root=${1:-/build-cache/toolchains}
mkdir -p "$cache_root"

make_prefix() {
	local source_prefix=$1
	local destination_prefix=$2
	local source_dir source_base destination_dir tool source target
	source_dir=$(dirname "$source_prefix")
	source_base=$(basename "$source_prefix")
	destination_dir=$(dirname "$destination_prefix")
	mkdir -p "$destination_dir"

	for source in "$source_dir/$source_base"*; do
		[ -e "$source" ] || continue
		tool=${source##*/$source_base}
		target="$destination_prefix$tool"
		case "$tool" in
			gcc|g++|cc|c++)
				cat >"$target" <<EOF
#!/usr/bin/env bash
exec ccache "$source" "\$@"
EOF
				chmod 0755 "$target"
				;;
			*)
				ln -sfn "$source" "$target"
				;;
		esac
	done
}

make_prefix \
	/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl- \
	"$cache_root/musl/bin/riscv64-unknown-linux-musl-"
make_prefix \
	/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf- \
	"$cache_root/elf/bin/riscv64-unknown-elf-"

ccache --set-config=max_size="${CCACHE_MAXSIZE:-20G}"
ccache --set-config=compression=true
ccache --show-stats
