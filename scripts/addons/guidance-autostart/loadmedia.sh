#!/bin/sh
cd "$(dirname "$(ls /mnt/system/ko/*/cv181x_vi.ko | head -1)")" || exit 1
# 先把视频侧的驱动卸掉再加载：CVI 的 VI/ISP/VB 状态会跨进程存活（应用崩溃或重启之后
# 相机就再也起不来，`SAMPLE_PLAT_VI_INIT` 直接 -1），只有驱动重新加载一次才干净——效果
# 等同于断电上电。`cv181x_rtos_cmdqu`/`cv181x_base`/`cv181x_sys` 保持加载，它们是小核的
# mailbox 和系统时钟，跟视频管线无关。
# Unload the video side before loading it: the CVI VI/ISP/VB state survives the process, so
# after a crash or restart the camera never comes back (`SAMPLE_PLAT_VI_INIT` returns -1) and
# only reloading the drivers clears it -- the same effect as a power cycle.
# `cv181x_rtos_cmdqu`/`cv181x_base`/`cv181x_sys` stay loaded: they are the little core's
# mailbox and the system clocks, unrelated to the video pipeline.
for m in cv181x_ive cv181x_jpeg cv181x_vcodec cv181x_rgn cv181x_dwa cv181x_vpss \
         cv181x_vi snsr_i2c cvi_mipi_rx cv181x_fast_image; do
  grep -q "^$m " /proc/modules && rmmod "$m" 2>/dev/null
done
for m in cv181x_sys cv181x_base cv181x_rtos_cmdqu cv181x_fast_image cvi_mipi_rx \
         snsr_i2c cv181x_vi cv181x_vpss cv181x_dwa cv181x_rgn cv181x_vcodec \
         cv181x_jpeg cv181x_ive; do
  grep -q "^$m " /proc/modules || insmod "$m.ko" 2>/dev/null
done
insmod cvi_vc_driver.ko MaxVencChnNum=9 MaxVdecChnNum=9 2>/dev/null
lsmod | grep -cE "cv181x|cvi_"
