#!/usr/bin/env bash
# instrument-card.sh - give an already-flashed card the two instruments a hang needs.
#
# 板子挂死之后，我们能问的问题只有两个："复位之后 U-Boot 跑没跑" 和 "关机序列停在哪"。
# 这版镜像（build-9 之前刷的卡）两个都没有。与其为了两个环境变量重编整张镜像，这个脚本
# 直接在卡上做两处外科手术：
#
#   1. FAT 根目录的 uboot.env 里加 `preboot`：U-Boot 在 bootcmd 之前交替写 /stage-a、
#      /stage-b。挂死时**不要断电**，拔卡看标记有没有翻。
#   2. 根分区的 /var/log/journal 建出来 + journald 持久化（16 MiB 上限），这样关机序列
#      会留在卡上，挂死后再上电就能 journalctl -D <p2>/var/log/journal -b -1。
#
# 不需要 mkenvimage：uboot.env 的格式是 [crc32(data)][data]，data 是 NUL 结尾的
# key=value 串、余下用 0xff 填充——脚本里按这个格式重写并重新计算 CRC。
#
# Usage: scripts/ci/instrument-card.sh /dev/mmcblk0
set -euo pipefail

device=${1:?usage: instrument-card.sh <block-device>}
[ -b "$device" ] || { echo "not a block device: $device" >&2; exit 1; }
part_boot="${device}p1"
part_root="${device}p2"
[ -b "$part_boot" ] && [ -b "$part_root" ] || { echo "expected $device"p1/p2 >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root_mount="$work/root"
mkdir -p "$root_mount"

PREBOOT='if test -e mmc 0:1 /stage-a; then fatwrite mmc 0 ${loadaddr} /stage-b 4; fatrm mmc 0 /stage-a; else fatwrite mmc 0 ${loadaddr} /stage-a 4; fatrm mmc 0 /stage-b; fi'

echo "== 1/2 uboot.env: adding the preboot stage marker"
mcopy -n -o -i "$part_boot" ::/uboot.env "$work/uboot.env"
python3 - "$work/uboot.env" "$PREBOOT" <<'PY'
import struct
import sys
import zlib
from pathlib import Path

path = Path(sys.argv[1])
want = sys.argv[2]
blob = path.read_bytes()
size = len(blob)
stored, data = struct.unpack("<I", blob[:4])[0], blob[4:]
if zlib.crc32(data) & 0xFFFFFFFF != stored:
    raise SystemExit("uboot.env CRC does not match the data area; refusing to rewrite it")
# The data area is NUL-terminated strings followed by 0xff filler; strip the filler
# before splitting, or it parses as one more "variable" and the rewrite grows the image.
variables = [item for item in data.rstrip(b"\xff").split(b"\0") if item]
pairs = {}
for item in variables:
    key, _, value = item.partition(b"=")
    pairs[key] = value
pairs[b"preboot"] = want.encode()
serialized = b"\0".join(b"%s=%s" % (key, value) for key, value in pairs.items()) + b"\0"
if len(serialized) > size - 4:
    raise SystemExit("the environment does not fit the %d-byte image" % size)
serialized = serialized.ljust(size - 4, b"\xff")
path.write_bytes(struct.pack("<I", zlib.crc32(serialized) & 0xFFFFFFFF) + serialized)
print("   %d variables, preboot added, %d/%d bytes used" % (len(pairs), len(serialized.rstrip(b"\xff")), size - 4))
PY
mcopy -o -i "$part_boot" "$work/uboot.env" ::/uboot.env
mcopy -n -o -i "$part_boot" ::/uboot.env "$work/check.env"
cmp "$work/uboot.env" "$work/check.env"

echo "== 2/2 rootfs: persistent journal"
sudo mount "$part_root" "$root_mount"
sudo install -d -m 2755 -o systemd-journal -g systemd-journal "$root_mount/var/log/journal"
sudo install -d -m 0755 "$root_mount/etc/systemd/journald.conf.d"
sudo tee "$root_mount/etc/systemd/journald.conf.d/10-persistent.conf" >/dev/null <<'CONF'
[Journal]
Storage=persistent
SystemMaxUse=16M
CONF
if ! grep -q '^preboot=' "$root_mount/etc/u-boot-initial-env"; then
	printf 'preboot=%s\n' "$PREBOOT" | sudo tee -a "$root_mount/etc/u-boot-initial-env" >/dev/null
fi
sudo sync
sudo umount "$root_mount"

echo "== done"
echo "   after a hang: pull the card WITHOUT power cycling and look for a new /stage-a or /stage-b"
echo "   shutdown tail: journalctl -D <p2>/var/log/journal -b -1"
