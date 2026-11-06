#!/usr/bin/env bash
# verify-card.sh - check a flashed card before trusting anything the board says.
#
# "The board sometimes does not boot from this card" has three very different causes,
# and they are cheap to tell apart *before* powering the board:
#
#   1. the write did not land (dd interrupted by a timeout, card pulled early);
#   2. the filesystems on the card are already inconsistent;
#   3. the card (or slot) really does fail to read/write.
#
# This script checks 1 and 2 and dumps the boot log ring, so a diagnosis starts from
# facts instead of from which probe file happened to be short this time.  It never
# writes to the card: read-only checks and a read of /bootlog.bin through mtools.
#
# Usage: sudo scripts/ci/verify-card.sh [DEVICE] [IMAGE]
#        DEVICE defaults to /dev/mmcblk0, IMAGE to output/$(plan artifact) for maixcam.
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)

device=${1:-/dev/mmcblk0}
image=${2:-}
if [ -z "$image" ]; then
	image=$repo_root/output/maixcam_sd.img
fi

fail=0
note() { printf '%s\n' "$*"; }
bad() {
	printf 'FAIL %s\n' "$*"
	fail=1
}

[ -b "$device" ] || { bad "$device is not a block device"; exit 1; }
[ -f "$image" ] || { bad "image $image is missing"; exit 1; }

part() {
	local suffix=$1
	if [ -b "${device}${suffix}" ]; then
		printf '%s' "${device}${suffix}"
	else
		printf '%s' "${device}p${suffix#p}"
	fi
}

boot_part=$(part 1)
root_part=$(part 2)

if findmnt -S "$device" -o TARGET -n >/dev/null 2>&1; then
	bad "$device has mounted partitions; unmount before verifying"
	exit 1
fi

# 1. did the write land?  Compare only as many bytes as the image has: the card is
#    larger, and the rest of it is allowed to hold whatever was there before.
size=$(stat -c %s "$image")
note "== card vs image ($size bytes)"
if cmp -s -n "$size" "$image" "$device"; then
	note "PASS the card matches $image byte for byte"
else
	# Report where it diverges instead of just "differs": an interrupted dd shows up
	# as a difference at the very end, a stale image as a difference at the start.
	# 卡上 ext4 的超级块里有 s_mtime/s_wtime/s_last_mounted/s_state：任何一次**读写**
	# 挂载都会改它们。所以“有几个字节不一样、且都落在文件系统元数据里”通常意味着有人在
	# 刷完之后挂载过它，而不是写没写完。这不是无害的差别，但它要的是重刷，不是换卡。
	offset=$(cmp -n "$size" "$image" "$device" 2>/dev/null || true)
	bad "the card differs from $image ($offset); re-flash before booting"
	note "    hint: if only a few bytes differ and they sit inside the root filesystem,"
	note "          something mounted it read-write after flashing (ext4 records"
	note "          s_mtime/s_wtime/s_last_mounted); flash and verify in one step instead"
fi

# 2. are the filesystems self-consistent right now?
check_fat() {
	command -v fsck.fat >/dev/null 2>&1 || { note "SKIP fsck.fat is not installed"; return; }
	local out
	if out=$(fsck.fat -n "$boot_part" 2>&1); then
		note "PASS fsck.fat: $(printf '%s' "$out" | tail -1)"
	else
		printf '%s\n' "$out" | tail -5
		bad "the boot partition is not consistent; bootlogd is supposed to never change its metadata"
	fi
}

check_ext4() {
	local out
	if out=$(e2fsck -fn "$root_part" 2>&1); then
		note "PASS e2fsck: clean"
	else
		printf '%s\n' "$out" | tail -5
		bad "the root filesystem is not clean"
	fi
}

note "== filesystems"
check_fat
check_ext4

# 3. the log from previous boots, without booting anything.
note "== boot log ring"
if command -v mcopy >/dev/null 2>&1; then
	tmp=$(mktemp -d)
	if mcopy -n -i "$boot_part" ::/bootlog.bin "$tmp/bootlog.bin" 2>/dev/null; then
		python3 "$repo_root/scripts/addons/bootlog/bootlogd" --show "$tmp/bootlog.bin" \
			| tail -40 || bad "the ring holds no intact records"
	else
		note "SKIP no /bootlog.bin on the card (image predates the ring)"
	fi
	rm -rf "$tmp"
else
	note "SKIP mtools (mcopy) is not installed"
fi

if [ "$fail" -ne 0 ]; then
	note "== result: FAIL"
	exit 1
fi
note "== result: PASS"
