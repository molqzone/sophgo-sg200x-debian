#!/usr/bin/env bash
# M2 cold-boot stress: reboot the board N times, wait for the C906L, and stop on
# the first crash record.  The crash record is the first-fault artifact; a
# nonzero magic/valid means the boot exposed a real fault.
set -u
HOST=${BOARD_HOST:-10.42.0.1}
USER=${BOARD_USER:-debian}
PASS=${BOARD_PASS:-rv}
COUNT=${BOOT_COUNT:-100}
START=${BOOT_START:-1}
SSH=(timeout 12 sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
     -o ConnectTimeout=5 "$USER@$HOST")
export SSHPASS=$PASS

wait_up() {
  for _ in $(seq 1 60); do
    "${SSH[@]}" 'true' >/dev/null 2>&1 && return 0
    sleep 5
  done
  return 1
}

wait_rproc() {
  for attempt in $(seq 1 60); do
    st=$("${SSH[@]}" 'cat /sys/class/remoteproc/remoteproc*/state 2>/dev/null | head -1' 2>/dev/null | tr -d '\r')
    [ "$st" = running ] && return 0
    # The board's multi-user target can stay in "starting"; bring the core up
    # explicitly when the automatic release has not run yet.
    if [ "$attempt" -eq 6 ]; then
      # The image's guidance-release/c906-core-run job can stay queued behind
      # the graphical target.  Start the core directly; the test only needs the
      # C906L running, not the vision application.
      "${SSH[@]}" "echo $PASS | sudo -S sh -c 'sync; nohup /usr/bin/rtos-mode remoteproc >/tmp/rtos-direct.log 2>&1 &'" >/dev/null 2>&1 || true
    fi
    sleep 5
  done
  return 1
}

read_crash() {
  "${SSH[@]}" "echo $PASS | sudo -S sh -c 'for a in 0x8FFD8000 0x8FFD8008 0x8FFD8014 0x8FFD8020 0x8FFDCEC0; do printf \"%s=\" \$a; busybox devmem \$a 32 2>/dev/null || echo fail; done'" 2>/dev/null
}

for n in $(seq "$START" $((START + COUNT - 1))); do
  echo "== boot $n =="
  "${SSH[@]}" "echo $PASS | sudo -S busybox reboot -f" >/dev/null 2>&1 || true
  sleep 10
  if ! wait_up; then echo "BOOT_FAIL $n: ssh"; exit 2; fi
  if ! wait_rproc; then echo "BOOT_FAIL $n: rproc"; exit 3; fi
  words=$(read_crash)
  echo "$words"
  magic=$(printf '%s\n' "$words" | sed -n 's/^0x8FFD8000=//p')
  valid=$(printf '%s\n' "$words" | sed -n 's/^0x8FFD8014=//p')
  if [ "$magic" != "0x00000000" ] || [ "$valid" != "0x00000000" ]; then
    echo "CRASH_RECORD boot $n: magic=$magic valid=$valid"
    exit 4
  fi
  sleep 2
done
echo "PASS $COUNT boots ($START..$((START + COUNT - 1))) without a crash record"
