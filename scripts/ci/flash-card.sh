#!/usr/bin/env bash
# flash-card.sh - write the image and prove the write landed, in one step.
#
# Flashing and verifying have to be one operation.  `output/*.img` and the card only
# agree until something touches the card: a read-write mount rewrites the ext4
# superblock (s_mtime, s_wtime, s_last_mounted, s_state), and a first boot resizes the
# root filesystem.  Both are normal, and both make a later byte comparison fail for
# reasons that have nothing to do with the write.  So: dd, then compare the exact
# byte count that was written, *before* anything can mount it.
#
# A comparison that fails here is a real finding: an interrupted write, a card that
# drops sectors, or the wrong image.  Those are the cases that used to be diagnosed by
# pulling probe files off a board that would not boot.
#
# Usage: sudo scripts/ci/flash-card.sh [DEVICE] [IMAGE]
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

device=${1:-/dev/mmcblk0}
image=${2:-}
if [ -z "$image" ]; then
	image=$repo_root/output/maixcam_sd.img
fi

[ -b "$device" ] || { echo "not a block device: $device" >&2; exit 1; }
[ -s "$image" ] || { echo "image is missing or empty: $image" >&2; exit 1; }
if [ "$(id -u)" -ne 0 ]; then
	echo "flash-card.sh needs write access to $device (run it with sudo)" >&2
	exit 1
fi
if findmnt -S "$device" -o TARGET -n >/dev/null 2>&1; then
	echo "$device has mounted partitions; unmount them first" >&2
	exit 1
fi

size=$(stat -c %s "$image")
echo "== writing $image ($size bytes) to $device"
dd if="$image" of="$device" bs=4M conv=fsync status=progress

echo "== verifying the $size bytes that were written"
if ! cmp -s -n "$size" "$image" "$device"; then
	offset=$(cmp -n "$size" "$image" "$device" 2>/dev/null || true)
	echo "FAIL the card does not match the image: $offset" >&2
	echo "     the write did not land; do not troubleshoot the board from this card" >&2
	exit 1
fi
echo "PASS the card matches $image byte for byte"

boot_part=${device}p1
[ -b "$boot_part" ] || boot_part=${device}1
root_part=${device}p2
[ -b "$root_part" ] || root_part=${device}2

if command -v fsck.fat >/dev/null 2>&1; then
	if out=$(fsck.fat -n "$boot_part" 2>&1); then
		echo "PASS fsck.fat: $(printf '%s' "$out" | tail -1)"
	else
		printf '%s\n' "$out" | tail -5
		echo "FAIL the boot partition is not consistent" >&2
		exit 1
	fi
fi
if out=$(e2fsck -fn "$root_part" 2>&1); then
	echo "PASS e2fsck: clean"
else
	printf '%s\n' "$out" | tail -5
	echo "FAIL the root filesystem is not clean" >&2
	exit 1
fi

echo "== done: safe to boot"
