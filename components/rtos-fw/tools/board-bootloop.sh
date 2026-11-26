#!/bin/sh
# Autonomous M2 cold-boot stress.  Runs once per Linux boot, reads the crash
# page, and reboots until 100 boots or the first crash record.
set -u
# Give load-systemko / remoteproc time to bring the C906L up.
sleep 30
COUNT_FILE=/root/bootloop.count
LOG_FILE=/root/bootloop.log
count=$(cat "$COUNT_FILE" 2>/dev/null || echo 0)
count=$((count + 1))
echo "$count" > "$COUNT_FILE"

magic=$(busybox devmem 0x8FFD8000 32 2>/dev/null || echo fail)
cause=$(busybox devmem 0x8FFD8008 32 2>/dev/null || echo fail)
valid=$(busybox devmem 0x8FFD8014 32 2>/dev/null || echo fail)
stval=$(busybox devmem 0x8FFD8020 32 2>/dev/null || echo fail)
head=$(busybox devmem 0x8FFDCEC0 32 2>/dev/null || echo fail)
printf 'boot %s magic=%s cause=%s valid=%s stval=%s head=%s\n' \
  "$count" "$magic" "$cause" "$valid" "$stval" "$head" >> "$LOG_FILE"

if [ "$magic" != "0x00000000" ] || [ "$valid" != "0x00000000" ]; then
  echo "CRASH_RECORD at boot $count" >> "$LOG_FILE"
  systemctl disable bootloop.service 2>/dev/null || true
  exit 0
fi
if [ "$count" -ge 100 ]; then
  echo "PASS 100 boots without a crash record" >> "$LOG_FILE"
  systemctl disable bootloop.service 2>/dev/null || true
  exit 0
fi
sleep 5
sync
busybox reboot -f
