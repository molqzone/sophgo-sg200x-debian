#!/bin/sh
# 放核前的守卫 / Guard in front of the core release.
#
# 状态目录里的两个标记 / Two markers in the state directory:
#   core-attempt-<bootid>  本次开机已经发出过放核请求 / a release was requested this boot
#   boot-ok                `/usr/local/bin/guidance-core-health.sh` 在 90 秒后写入
#                          written 90 s in by guidance-core-health.sh
#
# 规则：开机时若发现 **上一次** 的 attempt 存在、而 boot-ok 没有在之后更新，判定上次
# 放核把大核带死了；把 `no-auto-core` 写下来，从此不再自动放核（日志里说明怎么恢复）。
# Rule: if the *previous* attempt exists but boot-ok was not updated afterwards, the last
# release took the big core down.  Write `no-auto-core` and stop releasing automatically,
# stating in the log how to recover.
set -u
state=/var/lib/guidance
mkdir -p "$state"
boot_id=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)

if [ -f "$state/no-auto-core" ]; then
  echo "guidance-core-guard: auto release disabled by $state/no-auto-core; leaving the core off"
  echo "guidance-core-guard: remove that file (or run rtos-mode remoteproc by hand) to retry"
  exit 1
fi

prev_attempt=$(cat "$state/last-attempt" 2>/dev/null || true)
if [ -n "$prev_attempt" ] && [ "$prev_attempt" != "$boot_id" ]; then
  ok_boot=$(cat "$state/boot-ok" 2>/dev/null || true)
  if [ "$ok_boot" != "$prev_attempt" ]; then
    echo "guidance-core-guard: the last boot with a core release (${prev_attempt}) never" \
         "reached the 90 s health check; assuming the release broke the big core."
    echo "guidance-core-guard: writing $state/no-auto-core and skipping the release this boot."
    echo "guidance-core-guard: recover with 'rm $state/no-auto-core' once the cause is fixed."
    echo "$prev_attempt" > "$state/no-auto-core"
    exit 1
  fi
fi

echo "$boot_id" > "$state/last-attempt"
echo "guidance-core-guard: previous boot reached the health check; releasing the core"
exit 0
