#!/bin/sh
# 在板上安装从核看门狗（需要 root）。用法：install-mcu-watchdog.sh [recover]
# Install the little-core watchdog on the board (root).  Usage: install-mcu-watchdog.sh [recover]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
RECOVER=${1:-0}
install -m 0755 "$HERE/mcu-watchdog.sh" /usr/local/sbin/mcu-watchdog.sh
install -m 0644 "$HERE/mcu-watchdog.service" /etc/systemd/system/mcu-watchdog.service
mkdir -p /etc/systemd/system/mcu-watchdog.service.d
printf '[Service]\nEnvironment=MCU_WATCHDOG_RECOVER=%s\n' "$RECOVER" \
  > /etc/systemd/system/mcu-watchdog.service.d/10-recover.conf
systemctl daemon-reload
systemctl enable --now mcu-watchdog.service
systemctl --no-pager status mcu-watchdog.service | head -5
