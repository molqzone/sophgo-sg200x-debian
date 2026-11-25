#!/bin/sh
# 从核看门狗：用**产品自己的门铃**（`cop send --type 4`）做 ping。
# 连续 N 次没有回执 → 认为从核停死 → 通过 remoteproc 重启它。
# Little-core watchdog: ping with the product's own doorbell (`cop send --type 4`).
# N consecutive misses -> the core is considered dead -> restart it through remoteproc.
#
# 为什么用门铃而不是心跳字：门铃走的是**完整的数据通路**（PLIC → M-mode 转发 → S-mode 驱动 →
# 邮箱回执）。从核只要这一路还通，业务就是活的；反过来任何一处断了都应该算"从核不可用"。
# Why the doorbell and not a heartbeat word: the doorbell exercises the **whole path** (PLIC ->
# M-mode forwarding -> S-mode driver -> mailbox reply).  If that path works the core is alive; if any
# part of it breaks, the core should count as unavailable.
#
# **安全前提：先亲眼见到一次回执，之后才可能触发重启。** 启动早期从核还没起来时，ping 一定失败，
# 那时绝不能"顺手重启"它。
# **Safety precondition: only act after the core has answered at least once.**  During early boot the
# core is not up yet, so every ping fails; restarting it then would be wrong.
set -u
CONF=${MCU_WATCHDOG_FAILS:-3}
PERIOD=${MCU_WATCHDOG_PERIOD:-2}
COOLDOWN=${MCU_WATCHDOG_COOLDOWN:-15}
RECOVER=${MCU_WATCHDOG_RECOVER:-0}
CONTRACT=/usr/lib/firmware/guidance_contract.json
PAYLOAD=0000803f000000000000003f0000003f

seen_ok=0
fails=0
while true; do
  if cop send --contract "$CONTRACT" --type 4 --payload "$PAYLOAD" 2>/dev/null | grep -q 'reply='; then
    seen_ok=1
    fails=0
  else
    fails=$((fails + 1))
  fi

  if [ "$seen_ok" = "1" ] && [ "$fails" -ge "$CONF" ]; then
    logger -t mcu-watchdog "no doorbell reply in $fails attempts (recover=$RECOVER)"
    if [ "$RECOVER" = "1" ]; then
      for d in /sys/class/remoteproc/remoteproc*; do
        [ -e "$d/name" ] || continue
        [ "$(cat "$d/name" 2>/dev/null)" = "cv181x-c906_1" ] || continue
        logger -t mcu-watchdog "restarting $d"
        echo stop > "$d/state" 2>/dev/null || true
        sleep 1
        echo start > "$d/state" 2>/dev/null || true
      done
      fails=0
      seen_ok=0          # 重启之后要重新"亲眼见到"，否则会连环重启
      sleep "$COOLDOWN"
    else
      fails=$CONF
    fi
  fi
  sleep "$PERIOD"
done
