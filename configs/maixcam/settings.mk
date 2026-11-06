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

# C906L 启动策略（构建期定死，不在运行时选）：**只剩一个旋钮**——谁放核。
# scripts/Makefile 据此选 FSBL 输入，addons/guidance-fw/addon.mk 据此生成 systemd
# drop-in（rtos-mode 的参数由它推导，不是第二个变量），validate.py 再按设备树核对一遍。
# fsbl 只作 debug 变体（README 记着它会挂），生产用默认值。
#
# 曾经还有两个：`GUIDANCE_FW_RPROC_MODE`（load|attach）与 `GUIDANCE_FW_DOORBELL_OWNER`
# （rtos_cmdqu|uio）。前者是 LOADER 的函数，写成独立变量就能配出 rtos-mode 根本不认识的
# 子命令；后者两个取值里只有一个能实现（IRQ 101 被厂商驱动独占）。两个都删了，留下的是
# "谁放核"这一个事实，以及设备树里必须存在的 rtos_cmdqu/IRQ 101。
# C906L boot policy, fixed at build time: **one knob** -- who releases the core.  The
# rtos-mode argument follows from it, and the doorbell is not a choice (IRQ 101 belongs
# to the vendor driver), so neither is a setting any more.
GUIDANCE_FW_LOADER=remoteproc
USE_VENDOR_APT=0
MAIXCAM_SENSOR_CONFIG=
MAIXCAM_SENSOR_FIXED=0
MAIXCAM_SENSOR_PQ=sipeed_gc4653_30fps_202403261356.bin

LINUX_COMMIT=$(MAIXCAM_LINUX_COMMIT)
FSBL_COMMIT=$(MAIXCAM_FSBL_COMMIT)

# 镜像里只放运行时需要的东西。板上有 ssh、网络、厂商模块、parted（首次启动扩容）、
# python3（cop / bootlogd）、u-boot-menu（生成 extlinux）就够了；编译器和 -dev 包由
# PACKAGES_DEV 提供，只有在 `make image ENABLE_DEV_TOOLS=1` 时才装进去。
# 这一条直接决定镜像大小和刷写时间：开发工具链约占 rootfs 的 40%。
# Only what the board needs at runtime; the compiler and -dev packages live in
# PACKAGES_DEV and are installed only when ENABLE_DEV_TOOLS=1.  The toolchain was about
# 40% of the rootfs, and the rootfs size decides both the image size and the flash time.
ENABLE_DEV_TOOLS ?= 0
PACKAGES="busybox-static ca-certificates debian-archive-keyring dosfstools sudo bash-completion u-boot-menu openssh-server network-manager dnsmasq-base libpam-systemd iptables usbutils parted exfatprogs systemd-sysv i2c-tools net-tools ethtool gnupg gpiod u-boot-tools libubootenv-tool curl xz-utils nano libcap2-bin python3 fake-hwclock wireless-regdb wpasupplicant"


PACKAGES_DEV="binutils file tree vim rsync git gcc g++ wget zip unzip make gdb cmake \
 ninja-build pkg-config python3-pip python3-venv python3-tk libwpa-client-dev libnm-dev \
 libudev-dev libjpeg-dev libpng-dev libtiff-dev libavcodec-dev libavformat-dev \
 libswscale-dev libv4l-dev libxvidcore-dev libx264-dev libgtk-3-dev libcanberra-gtk3-module \
 libtbb-dev libdc1394-dev libopenexr-dev libeigen3-dev build-essential \
 libopencv-core-dev libopencv-imgproc-dev libopencv-imgcodecs-dev libopencv-videoio-dev \
 libopencv-highgui-dev"

IMAGE_ADDITIONS="maixcam-runtime maixcam-usb maixcam-sensor-config load-systemko guidance-fw sysctl-compat journald-compact systemd-lean wifi-connect time-sync network-services bootlog entropy"
