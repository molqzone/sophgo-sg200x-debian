#!/bin/sh
cd "$(dirname "$(ls /mnt/system/ko/*/cv181x_vi.ko | head -1)")" || exit 1
for m in cv181x_sys cv181x_base cv181x_rtos_cmdqu cv181x_fast_image cvi_mipi_rx \
         snsr_i2c cv181x_vi cv181x_vpss cv181x_dwa cv181x_rgn cv181x_vcodec \
         cv181x_jpeg cv181x_ive; do
  grep -q "^$m " /proc/modules || insmod "$m.ko" 2>/dev/null
done
insmod cvi_vc_driver.ko MaxVencChnNum=9 MaxVdecChnNum=9 2>/dev/null
lsmod | grep -cE "cv181x|cvi_"
