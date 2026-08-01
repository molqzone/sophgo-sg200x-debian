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
ENABLE_SG2002_IPC=1
ENABLE_LOCAL_PINMUX=1
USE_VENDOR_APT=0

PACKAGES="busybox-static ca-certificates debian-archive-keyring dosfstools binutils file tree sudo bash-completion u-boot-menu openssh-server network-manager dnsmasq-base libpam-systemd ppp libengine-pkcs11-openssl iptables vim usbutils parted exfatprogs systemd-sysv i2c-tools net-tools ethtool sudo gnupg rsync gpiod u-boot-tools libubootenv-tool git curl gcc g++ xz-utils wget zip unzip make nano gdb cmake ninja-build pkg-config libcap2-bin python3 python3-pip python3-venv python3-tk libwpa-client-dev libnm-dev libudev-dev libjpeg-dev libpng-dev libtiff-dev libavcodec-dev libavformat-dev libswscale-dev libv4l-dev libxvidcore-dev libx264-dev libgtk-3-dev libcanberra-gtk3-module libtbb-dev libdc1394-dev libopenexr-dev libeigen3-dev build-essential libopencv-core-dev libopencv-imgproc-dev libopencv-imgcodecs-dev libopencv-videoio-dev libopencv-highgui-dev fake-hwclock wireless-regdb wpasupplicant"

IMAGE_ADDITIONS="maixcam-runtime maixcam-usb maixcam-sensor-config load-systemko sg2002-ipc maixcam-aic8800-firmware sysctl-compat journald-compact systemd-lean wifi-connect time-sync network-services"
