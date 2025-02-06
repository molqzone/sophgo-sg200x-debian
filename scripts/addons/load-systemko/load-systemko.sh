#!/bin/sh

if [ "$1" = "start" ]
then
	KERNELRELEASE=$(uname -r)

	rmmod hynitron_touch
	rmmod spi_gpio
	rmmod spi_bitbang
	rmmod i2c_gpio
	rmmod i2c_algo_bit
	rmmod cvitek_remoteproc
	rmmod cvitek_mailbox

	. /etc/profile
	printf "load kernel module: "
	cd /mnt/system/ko/${KERNELRELEASE}/
	insmod cv181x_sys.ko
	insmod cv181x_base.ko
	insmod cv181x_rtos_cmdqu.ko
	insmod cv181x_fast_image.ko
	insmod cvi_mipi_rx.ko
	insmod snsr_i2c.ko
	insmod cv181x_vi.ko
	insmod cv181x_vpss.ko
	insmod cv181x_dwa.ko
	insmod cv181x_vo.ko
#	insmod cv181x_mipi_tx.ko
	insmod cv181x_rgn.ko
#	insmod cv181x_wdt.ko
#	insmod cv181x_clock_cooling.ko
	insmod cv181x_tpu.ko
	insmod cv181x_vcodec.ko
	insmod cv181x_jpeg.ko
	insmod cvi_vc_driver.ko MaxVencChnNum=9 MaxVdecChnNum=9
#	insmod cv181x_rtc.ko
	insmod cv181x_ive.ko
	insmod cv181x_mon.ko
#	insmod cv181x_pwm.ko
#	insmod cv181x_saradc.ko
	insmod cvi_wiegand.ko
	echo "OK"
	exit 0
fi
