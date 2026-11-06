CHIP=cv181x
UBOOT_CHIP=cv181x
UBOOT_BOARD=licheervnano_sd
BOOT_CPU=riscv
ARCH=riscv
DDR_CFG=ddr3_1866_x16
PARTITION_FILE=partition_sd.xml
STORAGE_TYPE=sd

KERNELREV="4"
FSBLVERSION=1.1.0
OSDRVVERSION=2024.10.14
MIDDLEWAREVERSION=2024.10.14

ENABLE_MEDIA_STACK=1
ENABLE_GUIDANCE_FW=1
ENABLE_LOCAL_PINMUX=1

# C906L 启动策略（构建期定死，不在运行时选）：谁放核、门铃归谁。scripts/Makefile 据此
# 选 FSBL 输入，addons/guidance-fw/addon.mk 据此生成 systemd drop-in（rtos-mode 的参数
# 由 LOADER 推导，不是第三个变量），validate.py 再按设备树核对一遍。fsbl 只作 debug
# 变体（README 记着它会挂），生产用默认值。
# C906L boot policy, fixed at build time: who releases the core and who owns the
# doorbell IRQ.  scripts/Makefile turns this into the FSBL input,
# addons/guidance-fw/addon.mk into the systemd drop-in (the rtos-mode argument follows
# from the loader rather than being a third variable), and validate.py re-checks it
# against the device tree.  fsbl is the debug variant only (the README records that it
# hangs); production uses the defaults.
GUIDANCE_FW_LOADER=remoteproc
GUIDANCE_FW_DOORBELL_OWNER=rtos_cmdqu
USE_VENDOR_APT=0
MAIXCAM_SENSOR_CONFIG=
MAIXCAM_SENSOR_FIXED=0
MAIXCAM_SENSOR_PQ=sipeed_gc4653_30fps_202403261356.bin

LINUX_COMMIT=$(MAIXCAM_LINUX_COMMIT)
FSBL_COMMIT=$(MAIXCAM_FSBL_COMMIT)

PACKAGES="busybox-static ca-certificates debian-archive-keyring dosfstools binutils file tree sudo bash-completion u-boot-menu openssh-server network-manager dnsmasq-base libpam-systemd ppp libengine-pkcs11-openssl iptables vim usbutils parted exfatprogs systemd-sysv i2c-tools net-tools ethtool sudo gnupg rsync gpiod u-boot-tools libubootenv-tool git curl gcc g++ xz-utils wget zip unzip make nano gdb cmake ninja-build pkg-config libcap2-bin python3 python3-pip python3-venv python3-tk libwpa-client-dev libnm-dev libudev-dev libjpeg-dev libpng-dev libtiff-dev libavcodec-dev libavformat-dev libswscale-dev libv4l-dev libxvidcore-dev libx264-dev libgtk-3-dev libcanberra-gtk3-module libtbb-dev libdc1394-dev libopenexr-dev libeigen3-dev build-essential libopencv-core-dev libopencv-imgproc-dev libopencv-imgcodecs-dev libopencv-videoio-dev libopencv-highgui-dev fake-hwclock wireless-regdb wpasupplicant"

IMAGE_ADDITIONS="maixcam-runtime maixcam-usb maixcam-sensor-config load-systemko guidance-fw sysctl-compat journald-compact systemd-lean wifi-connect time-sync network-services bootlog"
