#!/bin/bash
# 交付验收：只跑产品自己能看见的四条，不依赖任何 diag 计数器。
# Delivery acceptance: four checks the product can see for itself, no diagnostic counters.
#
#   1. 控制台出现 `IMU gyro rate: 995–1005 Hz`（BMI088 的 ODR 是 1 kHz，量测窗口会让读数
#      在 997–999 之间跳，卡在 998 会把抖动当成回归）
#   2. 控制台出现制导行 `guide v ... srv ...`
#   3. 门铃一问一答：`cop send --type 4` → `reply=<seq>`，rc=0
#   4. 连续 3 次重启，每次都满足 1–3
#
# 用法：components/guidance-fw/tools/acceptance.sh [board-ip]
# Usage: components/guidance-fw/tools/acceptance.sh [board-ip]
set -u
BOARD=${1:-10.42.0.1}
SSH="sshpass -p rv ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 debian@${BOARD}"
CONTRACT=/lib/firmware/guidance_contract.json
PAYLOAD=0000803f000000000000003f0000003f

fail=0
say() { printf '%s\n' "$*"; }

# 门铃检查必须等从核真的跑起来：从核由 multi-user 之后才放核，太早发命令会读到"没有回执"。
# 判据用产品自己的输出——控制台出现采样率行，就说明 IMU 线程在跑、门铃通路已经通了。
# The doorbell check has to wait for the little core to actually run: it is released only after
# multi-user, so a command sent too early reads as "no reply".  The readiness signal is the product's
# own output -- once the console prints a rate line, the IMU thread runs and the doorbell path is up.
wait_ready() {
  local deadline=$((SECONDS + 150))
  while [ "$SECONDS" -lt "$deadline" ]; do
    if $SSH "echo rv | sudo -S sh -c 'echo dummy; timeout 4 cop log 2>&1'" 2>/dev/null |
       grep -q 'IMU gyro rate:'; then
      say "  (从核就绪 / little core ready)"
      return 0
    fi
    sleep 5
  done
  say "  (等待从核超时 / little core did not become ready)"
  return 1
}

run_checks() {
  local tag=$1
  local ok=0

  # 1) 采样率
  local rate
  rate=$($SSH "echo rv | sudo -S sh -c 'echo dummy; timeout 4 cop log 2>&1'" 2>/dev/null |
         grep -o 'IMU gyro rate: [0-9]* Hz' | tail -1 | grep -o '[0-9]*')
  if [ -n "${rate:-}" ] && [ "$rate" -ge 995 ] && [ "$rate" -le 1005 ]; then
    say "  [${tag}] 1. 采样率 ${rate} Hz            PASS"; ok=$((ok+1))
  else
    say "  [${tag}] 1. 采样率 ${rate:-<无>} Hz        FAIL"; fail=$((fail+1))
  fi

  # 2) 端到端制导行
  if $SSH "echo rv | sudo -S sh -c 'echo dummy; timeout 4 cop log 2>&1'" 2>/dev/null | grep -q 'guide v '; then
    say "  [${tag}] 2. 制导行                     PASS"; ok=$((ok+1))
  else
    say "  [${tag}] 2. 制导行                     FAIL"; fail=$((fail+1))
  fi

  # 3) 门铃
  if $SSH "echo rv | sudo -S sh -c 'echo dummy; cop send --contract ${CONTRACT} --type 4 --payload ${PAYLOAD}'" 2>/dev/null | grep -qE 'reply=0x[0-9a-f]+'; then
    say "  [${tag}] 3. 门铃 reply=seq             PASS"; ok=$((ok+1))
  else
    say "  [${tag}] 3. 门铃 reply=seq             FAIL"; fail=$((fail+1))
  fi

  [ "$ok" -eq 3 ]
}

say "== 当前启动 =="
wait_ready || true
run_checks boot || true

for i in 1 2 3; do
  say "== 重启 ${i}/3 =="
  $SSH "echo rv | sudo -S sh -c 'echo dummy; sync; (reboot &)'" >/dev/null 2>&1
  sleep 95
  wait_ready || true
  run_checks "reboot${i}" || true
done

say ""
if [ "$fail" -eq 0 ]; then say "验收通过（4 条 × 4 次）"; exit 0; fi
say "验收失败：${fail} 项"; exit 1
