#!/bin/sh

if [ -r /etc/default/maixcam-sensor-config ]
then
	. /etc/default/maixcam-sensor-config
fi

install_sensor_config()
{
	missing_default=$1
	empty_default=$2
	if [ -n "${SENSOR_CONFIG_DEFAULT:-}" ]
	then
		missing_default=$SENSOR_CONFIG_DEFAULT
		empty_default=$SENSOR_CONFIG_DEFAULT
	fi
	if [ ! -e /mnt/data/sensor_cfg.ini ]
	then
		cp "$missing_default" /mnt/data/sensor_cfg.ini
	fi
	if [ ! -s /mnt/data/sensor_cfg.ini ]
	then
		cp "$empty_default" /mnt/data/sensor_cfg.ini
	fi
}

if [ "$1" = "start" ]
then
	printf "copy sensor config file: "
	if [ "${SENSOR_CONFIG_FIXED:-0}" = "1" ]
	then
		if [ -z "${SENSOR_CONFIG_DEFAULT:-}" ]
		then
			echo "fixed sensor profile is missing"
			exit 1
		fi
		cp "$SENSOR_CONFIG_DEFAULT" /mnt/data/sensor_cfg.ini
		# MIPI RX 4N PINMUX MIPI RX 4N
		devmem 0x0300116C 32 0x3
		# MIPI RX 0N PINMUX MCLK1
		devmem 0x0300118C 32 0x5
		echo -n " fixed "
	elif [ -e /boot/alpha ]
	then
		install_sensor_config /mnt/data/sensor_cfg.ini.alpha /mnt/data/sensor_cfg.ini.beta
		# MIPI RX 4N PINMUX MCLK0
		devmem 0x0300116C 32 0x5
		# MIPI RX 0N PINMUX MIPIP RX 0N
		devmem 0x0300118C 32 0x3
		echo " alpha "
	elif [ -e /boot/epsilon ]
	then
		install_sensor_config /mnt/data/sensor_cfg.ini.alpha /mnt/data/sensor_cfg.ini.beta
		# MIPI RX 4N PINMUX no change
		#
		# MIPI RX 0N PINMUX no change
		#
		echo " epsilon "
	else
		install_sensor_config /mnt/data/sensor_cfg.ini.beta /mnt/data/sensor_cfg.ini.beta
		# MIPI RX 4N PINMUX MIPI RX 4N
		devmem 0x0300116C 32 0x3
		# MIPI RX 0N PINMUX MCLK1
		devmem 0x0300118C 32 0x5
		echo -n " beta "
	fi
	if [ "${SENSOR_CONFIG_FIXED:-0}" != "1" ] && [ -e /boot/kvmtest ]
	then
		cp /mnt/data/sensor_cfg.ini.LT /mnt/data/sensor_cfg.ini
	fi
	echo "OK"
fi
