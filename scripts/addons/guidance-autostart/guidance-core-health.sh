#!/bin/sh
# 90 秒后仍能跑到这里，说明放核没有把大核带死：记下这次 boot id，守卫下次就会放行。
# Reaching this 90 s in means the release did not take the big core down: record this
# boot id so the guard lets the next boot through.
set -u
state=/var/lib/guidance
mkdir -p "$state"
boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
echo "$boot_id" > "$state/boot-ok"
if [ -f "$state/no-auto-core" ]; then
  echo "guidance-core-health: this boot is healthy; auto release stays disabled (marker present)"
else
  echo "guidance-core-health: this boot is healthy (boot_id $boot_id)"
fi
