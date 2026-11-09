#!/usr/bin/env bash
# prepare-rootfs.sh - produce the Debian rootfs, and remember how to reproduce it fast.
#
# `mmdebstrap --include=<780 packages>` is the slowest step of an image build (10-20 min)
# and it is also the step that has nothing to do with whatever you just changed.  It only
# depends on a handful of inputs: board, storage type, mirror, vendor apt, and the package
# list.  So after a successful bootstrap the result is cached as a tarball keyed by a hash
# of exactly those inputs; when the key still matches, "bootstrapping" is unpacking a
# 2.7 GB tar (~40 s) instead of fetching and installing 780 packages.
#
# The key is the point: a cache that cannot tell whether the package list changed would
# silently produce images from a stale rootfs, which is worse than being slow.  Change any
# input and the key changes, so the next build bootstraps again.
#
# Usage (called by scripts/Makefile; --no-cache forces a real bootstrap):
#   prepare-rootfs.sh --board maixcam --storage sd --rootfs /build/rootfs \
#       --cache-dir /build/rootfs-cache --packages "a b c" --mirror https://deb.debian.org
set -euo pipefail

board=
storage=
rootfs=
cache_dir=
packages=
mirror=https://deb.debian.org/debian
vendor_apt=0
apt_http_proxy=
apt_https_proxy=
use_cache=1
cache_version=1

while [ $# -gt 0 ]; do
	case "$1" in
		--board) board=$2; shift 2 ;;
		--storage) storage=$2; shift 2 ;;
		--rootfs) rootfs=$2; shift 2 ;;
		--cache-dir) cache_dir=$2; shift 2 ;;
		--packages) packages=$2; shift 2 ;;
		--mirror) mirror=$2; shift 2 ;;
		--vendor-apt) vendor_apt=$2; shift 2 ;;
		--apt-http-proxy) apt_http_proxy=$2; shift 2 ;;
		--apt-https-proxy) apt_https_proxy=$2; shift 2 ;;
		--no-cache) use_cache=0; shift ;;
		*) echo "prepare-rootfs: unknown argument: $1" >&2; exit 2 ;;
	esac
done

for required in board storage rootfs cache_dir; do
	eval "value=\$$required"
	[ -n "$value" ] || { echo "prepare-rootfs: --$required is required" >&2; exit 2; }
done

cache_tar="$cache_dir/rootfs-$board-$storage.tar"
cache_key_file="$cache_dir/rootfs-$board-$storage.key"

key() {
	printf '%s\n' \
		"v$cache_version" "board=$board" "storage=$storage" \
		"mirror=$mirror" "vendor_apt=$vendor_apt" "packages=$packages" \
		| sha256sum | cut -d' ' -f1
}

# The vendor repo is signed by a key that does not come from the mirror, so it is
# downloaded here and copied into the chroot by the Makefile.  Fetch it *before* the
# cache check: a cache hit skips bootstrap, and the Makefile's copy then failed with
# "cannot stat .../public-key.asc" for every board with USE_VENDOR_APT=1.
fetch_vendor_key() {
	[ "$vendor_apt" = 1 ] || return 0
	echo "prepare-rootfs: fetching the vendor apt key"
	curl --fail --location --retry 5 --retry-delay 3 --retry-all-errors \
		--connect-timeout 20 https://sophgo.my-ho.st/public-key.asc \
		-o "$cache_dir/public-key.asc"
}

bootstrap() {
	local sources=("deb $mirror sid main")
	echo "prepare-rootfs: bootstrapping $board/$storage (780 packages, the slow path)"
	if [ "$vendor_apt" = 1 ]; then
		sources+=("deb [signed-by=$cache_dir/public-key.asc] https://sophgo.my-ho.st:8443/ debian sophgo")
	fi
	env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u all_proxy \
		http_proxy="$apt_http_proxy" https_proxy="$apt_https_proxy" \
		mmdebstrap -v --architectures=riscv64 --include="$packages" sid \
		"$rootfs" "${sources[@]}"
}

save_cache() {
	mkdir -p "$cache_dir"
	local temp="$cache_tar.tmp"
	echo "prepare-rootfs: saving the rootfs cache ($cache_tar)"
	# `--numeric-owner` matters: debootstrap creates files owned by numeric ids, and a
	# restore that renumbers them would change the image.  Stored uncompressed on
	# purpose -- zstd costs more CPU than the disk write it saves here.
	tar --numeric-owner --xattrs --xattrs-include='*' -cpf "$temp" -C "$rootfs" .
	mv "$temp" "$cache_tar"
	key > "$cache_key_file"
}

restore_cache() {
	echo "prepare-rootfs: restoring $cache_tar (cache hit; no mmdebstrap)"
	tar --numeric-owner --xattrs --xattrs-include='*' -xpf "$cache_tar" -C "$rootfs"
}

mkdir -p "$rootfs" "$cache_dir"
fetch_vendor_key

if [ "$use_cache" = 1 ] && [ -s "$cache_tar" ] \
		&& [ "$(cat "$cache_key_file" 2>/dev/null || true)" = "$(key)" ]; then
	restore_cache
	exit 0
fi

if [ "$use_cache" = 1 ] && [ -s "$cache_tar" ]; then
	echo "prepare-rootfs: cache key changed, bootstrapping from scratch"
fi

rm -rf "$rootfs"
mkdir -p "$rootfs"
bootstrap
[ -n "$(ls -A "$rootfs" 2>/dev/null)" ] || {
	echo "prepare-rootfs: mmdebstrap produced an empty rootfs" >&2
	exit 1
}
[ "$use_cache" = 1 ] && save_cache
exit 0
