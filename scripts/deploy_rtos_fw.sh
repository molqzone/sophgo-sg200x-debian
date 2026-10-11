#!/usr/bin/env bash
set -euo pipefail

BOARD_HOST=${BOARD_HOST:-ubuntu24}
FW_PATH=${1:-}

if [ -z "$FW_PATH" ]; then
  echo "Usage: deploy_rtos_fw.sh <path-to-c906-mcu.elf>" >&2
  exit 2
fi

if [ ! -f "$FW_PATH" ]; then
  echo "Firmware not found: $FW_PATH" >&2
  exit 1
fi

REMOTE_TMP=/tmp/c906-mcu.elf

cat "$FW_PATH" | /root/codex/scripts/ssh-rotated-key.sh "$BOARD_HOST" "bash -lc 'cat > $REMOTE_TMP'"

/root/codex/scripts/ssh-rotated-key.sh "$BOARD_HOST" "bash -lc '
  install -D -m 0755 $REMOTE_TMP /lib/firmware/c906-mcu.elf && \
  sync && \
  command -v rtos-mode >/dev/null 2>&1 && rtos-mode remoteproc || true && \
  ls -l /lib/firmware/c906-mcu.elf && \
  ls -l /sys/class/remoteproc 2>/dev/null || true && \
  dmesg | tail -n 80
'"
