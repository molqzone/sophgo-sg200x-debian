#!/bin/sh

if [ "$1" = "start" ]
then
	KERNELRELEASE=$(uname -r)

	rmmod hynitron_touch 2>/dev/null || true
	rmmod spi_gpio 2>/dev/null || true
	rmmod spi_bitbang 2>/dev/null || true
	rmmod i2c_gpio 2>/dev/null || true
	rmmod i2c_algo_bit 2>/dev/null || true
	rmmod cvitek_remoteproc 2>/dev/null || true
	rmmod cvitek_mailbox 2>/dev/null || true

	printf "load kernel module: "
	cd /mnt/system/ko/${KERNELRELEASE}/
	insmod cv181x_sys.ko
	insmod cv181x_base.ko
	# The vendor RTOS path already exposes rtos_cmdqu on the mailbox block.
	# Do not also load the out-of-tree mailbox/remoteproc stack by default,
	# otherwise both paths compete for the same IRQ/register range.
	# Load the Wi-Fi stack from the same packaged module set to avoid stale
	# auto-loaded copies from /lib/modules racing ahead of boot service order.
	modprobe cfg80211 2>/dev/null || true
	modprobe mac80211 2>/dev/null || true
	[ -f aic8800_bsp.ko ] && insmod aic8800_bsp.ko
	[ -f aic8800_fdrv.ko ] && insmod aic8800_fdrv.ko
	insmod cv181x_rtos_cmdqu.ko
	insmod cv181x_fast_image.ko
	insmod cvi_mipi_rx.ko
	insmod snsr_i2c.ko
	insmod cv181x_vi.ko
	insmod cv181x_vpss.ko
	insmod cv181x_dwa.ko
	# Headless MaixCAM route: do not load VO by default.
	# insmod cv181x_vo.ko
#	insmod cv181x_mipi_tx.ko
	insmod cv181x_rgn.ko
#	insmod cv181x_wdt.ko
#	insmod cv181x_clock_cooling.ko
	# Current scope excludes the TPU/NPU path.
	# insmod cv181x_tpu.ko
	insmod cv181x_vcodec.ko
	insmod cv181x_jpeg.ko
	insmod cvi_vc_driver.ko MaxVencChnNum=9 MaxVdecChnNum=9
#	insmod cv181x_rtc.ko
	insmod cv181x_ive.ko
	# Monitoring helpers are not needed for the headless camera route.
	# insmod cv181x_mon.ko
#	insmod cv181x_pwm.ko
#	insmod cv181x_saradc.ko
	# Wiegand is unrelated to the MaixCAM runtime profile.
	# insmod cvi_wiegand.ko
	echo "OK"
	exit 0
fi
