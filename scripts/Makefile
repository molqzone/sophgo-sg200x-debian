COLOUR_GREEN=\n\033[0;32m
COLOUR_RED=\033[0;31m
COLOUR_BLUE=\033[0;34m
END_COLOUR=\033[0m\n

red:=$(shell tput setaf 1)
blue:=$(shell tput setaf 4)
reset:=$(shell tput sgr0)

BUILDDIR := /build/

DEB_ARCH = riscv64

GIT_CLONE_OPTS = --shallow-since=2020-03-12

OSDRV_ENV = CHIP_ARCH=CV181X \
CVIARCH=CV181X \
SDK_VER=musl_riscv64 \
CROSS_COMPILE_MUSL_RISCV64=/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl- \
CONFIG_ARCH=riscv \
CONFIG_CROSS_COMPILE_KERNEL="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" \
CONFIG_CP_EXT_WIRELESS=y

ifneq ($(findstring # CONFIG_FB is not set,$(shell cat /configs/$(BOARD)/linux/defconfig 2>/dev/null)),)
OSDRV_ENV += CONFIG_NO_FB=1
endif

SENSOR_ENV = CONFIG_SENSOR_GCORE_GC2083=y \
CONFIG_SENSOR_GCORE_GC4653=y \
CONFIG_SENSOR_OV_OS04A10=y \
CONFIG_SENSOR_OV_OV2685=y \
CONFIG_SENSOR_OV_OV5647=y \
CONFIG_SENSOR_SMS_SC035GS=y \
CONFIG_SENSOR_LONTIUM_LT6911=y

MIDDLEWARE_ENV = $(OSDRV_ENV) $(SENSOR_ENV)

KO_DIRS = drivers/i2c \
drivers/input/touchscreen \
drivers/mailbox \
drivers/media/usb/gspca \
drivers/net/usb \
drivers/net/wireless \
drivers/pwm \
drivers/remoteproc \
drivers/rtc \
drivers/spi \
drivers/video/backlight \
fs/efivarfs \
net/ipv4 \
net/wireless

ifndef BOARD
$(error $(red)BOARD is not set$(reset))
endif

include /configs/settings.mk

ifneq ("$(wildcard /configs/$(BOARD)/settings.mk)","")
include /configs/$(BOARD)/settings.mk
else
$(error $(red)Board $(BOARD) not supported$(reset))
endif

include $(wildcard /builder/addons/*/addon.mk)

addon-targets = $(patsubst "%,$(BUILDDIR)/%-stamp,$(patsubst %",%,$(IMAGE_ADDITIONS)))
_PACKAGES = $(patsubst "%,%,$(patsubst %",%,$(PACKAGES)))

FSBL_BLCP_2ND_PATH := $(BUILDDIR)/fsbl/blank.bin
ifneq ($(filter $(BUILDDIR)/rtos-firmware-stamp,$(addon-targets)),)
FSBL_RTOS_TARGET := $(BUILDDIR)/rtos-firmware-build-stamp
FSBL_BLCP_2ND_PATH := $(RTOS_FIRMWARE_BIN)
endif


$(info $(blue)Board: $(BOARD)$(reset))
$(info $(blue)Storage: $(STORAGE_TYPE)$(reset))
$(info $(blue)Image Addons: $(IMAGE_ADDITIONS)$(reset))
$(info $(blue)Packages: $(_PACKAGES)$(reset))

NPROCS := $(shell nproc)

PINMUX_COMMIT = ec3fd56cef75a5ee1d7b485e0e8f3df1eb546cd1
PINMUX_VERSION = 1.0.0+gitec3fd56
PINMUX_SRC = $(BUILDDIR)/cvi-pinmux-cv181x
PINMUX_PKG = $(BUILDDIR)/package/cvi-pinmux-cv181x-$(PINMUX_VERSION)
PINMUX_CC = /host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc

ifeq ($(BOARD),licheervnano)
LOCAL_PINMUX_TARGET = $(BUILDDIR)/cvi-pinmux-package-stamp
endif


define copy_header_action
	@cp -r $(BUILDDIR)/osdrv/interdrv/include/chip/cv181x/uapi/linux/* ${1}/linux/
	@cp -r $(BUILDDIR)/osdrv/interdrv/include/common/uapi/linux/* ${1}/linux/
	@cp $(BUILDDIR)/kernel/drivers/staging/android/uapi/ion.h ${1}/linux/
	@cp $(BUILDDIR)/kernel/drivers/staging/android/uapi/ion_cvitek.h ${1}/linux/
	@cp $(BUILDDIR)/kernel/include/uapi/linux/dma-buf.h ${1}/linux/
endef

define copy_ko_action
	@mkdir -p $(BUILDDIR)/osdrv/ko
	$(foreach kodir, $(KO_DIRS), find ${1}/lib/modules/*/kernel/$(kodir) -name '*.ko' -exec cp -f {} $(BUILDDIR)/osdrv/ko/ \; ;)
endef

$(BUILDDIR)/linux-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out Kernel for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b sg200x-dev $(GIT_CLONE_OPTS) https://github.com/sophgo/linux_5.10.git /build/kernel
	@cd $(BUILDDIR)/kernel && git checkout e6ead53
	@touch $@

$(BUILDDIR)/linux-prepare-patch-stamp: $(BUILDDIR)/linux-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching Kernel for $(BOARD)$(END_COLOUR)"
	@$(foreach file, $(wildcard /configs/common/patches/linux/*.patch), cd $(BUILDDIR)/kernel && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/$(BOARD)/patches/linux/*.patch), cd $(BUILDDIR)/kernel && git apply --ignore-whitespace $(file);)
	@grep -Fq "firmware has no resource table; using mailbox-only mode" $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c
	@grep -Fq "failed to %s C906L via SBI: %d" $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c
	@grep -Fq "boot trace host sentinel at 0x%llx" $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c
	@cp /configs/$(BOARD)/linux/defconfig $(BUILDDIR)/kernel/arch/riscv/configs/${BOARD}_defconfig
	@$(foreach file, $(wildcard /configs/common/dts/$(CHIP)/*), cp $(file) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/;)
	@$(foreach file, $(wildcard /configs/common/dts/$(CHIP)_$(ARCH)/*), cp $(file) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/;)
	@$(foreach file, $(wildcard /configs/$(BOARD)/dts/*), cp $(file) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/;)
	@python3 /builder/python/mmap_conv.py --type h /configs/$(BOARD)/memmap.py $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/cvi_board_memmap.h
	@touch $@

$(BUILDDIR)/linux-prepare-configure-stamp: $(BUILDDIR)/linux-prepare-patch-stamp
	@echo "$(COLOUR_GREEN)Configuring Kernel for $(BOARD)$(END_COLOUR)"
	@cd $(BUILDDIR)/kernel KDEB_SOURCENAME=linux-sophgo  && $(MAKE) -j$(NPROCS) O=$(BUILDDIR)/kernel/ ARCH=riscv CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" ${BOARD}_defconfig
	@$(eval LV=$(shell cd $(BUILDDIR)/kernel && git log -1 --format="%at" | xargs -I{} date -d @{} +-%Y%m%d-${KERNELREV}))
	@sed -i 's/CONFIG_LOCALVERSION=""/CONFIG_LOCALVERSION="$(LV)"/' $(BUILDDIR)/kernel/.config
	@touch $@

$(BUILDDIR)/linux-compile-stamp: $(BUILDDIR)/linux-prepare-configure-stamp
	@echo "$(COLOUR_GREEN)Building Kernel for $(BOARD)$(END_COLOUR)"
	@sed -i 's/packagename=linux-image.*/packagename=linux-image-$(BOARD)/' $(BUILDDIR)/kernel/scripts/package/mkdebian
	@sed -i 's/packagename=linux-image-$$version/packagename=linux-image-$(BOARD)-$$version/' $(BUILDDIR)/kernel/scripts/package/builddeb
	@cd $(BUILDDIR)/kernel && KCFLAGS=-Wno-attribute-alias ARCH=riscv CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" KDEB_SOURCENAME=linux-$(BOARD) $(MAKE) -j$(NPROCS) O=$(BUILDDIR)/kernel/ bindeb-pkg
	@cd $(BUILDDIR)/kernel && KCFLAGS=-Wno-attribute-alias ARCH=riscv CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" KDEB_SOURCENAME=linux-$(BOARD) $(MAKE) -j$(NPROCS) O=$(BUILDDIR)/kernel/ modules_install INSTALL_MOD_PATH=$(BUILDDIR)/kernel/modules headers_install INSTALL_HDR_PATH=$(BUILDDIR)/kernel/riscv/usr
	@cp $(BUILDDIR)/*.deb /output/
	@touch $@

linux: $(BUILDDIR)/linux-compile-stamp

linux-clean:
	@rm -rf $(BUILDDIR)/kernel
	@rm -f $(BUILDDIR)/linux-*-stamp
	@rm -f $(BUILDDIR)/linux-*.deb


$(BUILDDIR)/osdrv-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out OSdrv for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b sg200x-dev $(GIT_CLONE_OPTS) https://github.com/sophgo/osdrv.git /build/osdrv
	@cd $(BUILDDIR)/osdrv && git checkout 28215f2
	@touch $@

$(BUILDDIR)/osdrv-prepare-patch-stamp: $(BUILDDIR)/osdrv-prepare-checkout-stamp $(BUILDDIR)/linux-compile-stamp
	@echo "$(COLOUR_GREEN)Patching OSdrv for $(BOARD)$(END_COLOUR)"
	$(call copy_ko_action, $(BUILDDIR)/kernel/modules)
	@$(foreach file, $(wildcard /configs/common/patches/osdrv/*.patch), cd $(BUILDDIR)/osdrv && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/$(BOARD)/patches/osdrv/*.patch), cd $(BUILDDIR)/osdrv && git apply --ignore-whitespace $(file);)
	@touch $@

$(BUILDDIR)/osdrv-prepare-configure-stamp: $(BUILDDIR)/osdrv-prepare-patch-stamp
	@echo "$(COLOUR_GREEN)Configuring OSdrv for $(BOARD)$(END_COLOUR)"
	@touch $@

$(BUILDDIR)/osdrv-compile-stamp: $(BUILDDIR)/osdrv-prepare-configure-stamp
	@echo "$(COLOUR_GREEN)Building OSdrv for $(BOARD)$(END_COLOUR)"
	@cd $(BUILDDIR)/osdrv && $(OSDRV_ENV) $(MAKE) KERNEL_DIR=$(BUILDDIR)/kernel INSTALL_DIR=$(BUILDDIR)/osdrv/ko all
	@touch $@

$(BUILDDIR)/osdrv-package-stamp: $(BUILDDIR)/osdrv-compile-stamp
	@echo "$(COLOUR_GREEN)Packaging OSdrv for $(BOARD)$(END_COLOUR)"
	@$(eval KERNELRELEASE=$(shell basename $(BUILDDIR)/kernel/debian/linux-headers/usr/share/doc/linux-headers-* | cut -d '-' -f 3-))
	@mkdir -p $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)
	@cp -r /builder/deb/cvitek-osdrv/* $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/
	@mkdir -pv $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/
	@cp -p $(BUILDDIR)/osdrv/ko/*.ko $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/
	@rm -f $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/$(CHIP)_clock_cooling.ko
	@rm -f $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/$(CHIP)_pwm.ko
	@rm -f $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/$(CHIP)_rtc.ko
	@rm -f $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/$(CHIP)_saradc.ko
	@rm -f $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/$(CHIP)_wdt.ko
	@mkdir -pv $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/3rd/
	@cp -p $(BUILDDIR)/osdrv/ko/3rd/*.ko $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/mnt/system/ko/$(KERNELRELEASE)/3rd/
	@sed -i 's/Version: 1.0.0/Version: $(OSDRVVERSION)-$(KERNELRELEASE)/' $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/DEBIAN/control
	@sed -i 's/Package: cvitek-osdrv/Package: cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/' $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)/DEBIAN/control
	@cd $(BUILDDIR)/package/ && dpkg-deb --build cvitek-osdrv-$(BOARD)-$(KERNELRELEASE) cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)_$(OSDRVVERSION)-$(KERNELRELEASE)_$(DEB_ARCH).deb
	@cp $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)_$(OSDRVVERSION)-$(KERNELRELEASE)_$(DEB_ARCH).deb /output/
	@touch $@

osdrv: $(BUILDDIR)/osdrv-package-stamp

osdrv-clean:
	@rm -rf $(BUILDDIR)/osdrv
	@rm -f $(BUILDDIR)/osdrv-*-stamp


$(BUILDDIR)/middleware-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out Middleware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b sg200x-dev $(GIT_CLONE_OPTS) --recursive https://github.com/sophgo/cvi_mpi.git /build/middleware
	@cd $(BUILDDIR)/middleware && git checkout ebaaf1b
	@git clone -b sg200x-dev $(GIT_CLONE_OPTS) --recursive https://github.com/sophgo/SensorSupportList.git /build/middleware/component/isp
	@cd $(BUILDDIR)/middleware/component/isp && git checkout 7e900fc
	@mkdir -p $(BUILDDIR)/middleware/sample/test_mmf
	@git clone -b maixcdk $(GIT_CLONE_OPTS) --recursive https://github.com/scpcom/ireader $(BUILDDIR)/middleware/sample/test_mmf/media_server-1.0.x
	@cd $(BUILDDIR)/middleware/sample/test_mmf/media_server-1.0.x && git checkout 06cf95b
	@touch $@

$(BUILDDIR)/middleware-prepare-patch-stamp: $(BUILDDIR)/middleware-prepare-checkout-stamp $(BUILDDIR)/osdrv-compile-stamp
	@echo "$(COLOUR_GREEN)Patching Middleware for $(BOARD)$(END_COLOUR)"
	mkdir -pv $(BUILDDIR)/kernel/riscv/usr/include/linux/
	$(call copy_header_action, $(BUILDDIR)/kernel/riscv/usr/include)
	@$(foreach file, $(wildcard /configs/common/patches/middleware/*.patch), cd $(BUILDDIR)/middleware && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/$(BOARD)/patches/middleware/*.patch), cd $(BUILDDIR)/middleware && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/common/patches/sensorsupportlist/*.patch), cd $(BUILDDIR)/middleware/component/isp && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/$(BOARD)/patches/sensorsupportlist/*.patch), cd $(BUILDDIR)/middleware/component/isp && git apply --ignore-whitespace $(file);)
	sed -i 's|$$(ROOT_DIR)/../host-tools|/host-tools|g' $(BUILDDIR)/middleware/Makefile.param
	sed -i 's|^include $$(BUILD_PATH)/.config|-include $$(BUILD_PATH)/.config|g' $(BUILDDIR)/middleware/Makefile.param
	sed -i 's|^include $$(BUILD_PATH)/.config|-include $$(BUILD_PATH)/.config|g' $(BUILDDIR)/middleware/component/isp/Makefile
	sed -i 's|^include $$(BUILD_PATH)/.config|-include $$(BUILD_PATH)/.config|g' $(BUILDDIR)/middleware/component/isp/common/Makefile
	sed -i 's|^include $$(BUILD_PATH)/.config|-include $$(BUILD_PATH)/.config|g' $(BUILDDIR)/middleware/sample/common/Makefile
	[ -e $(BUILDDIR)/middleware/sample/mipi_tx ] || sed -i 's|cp -f sample/mipi_tx/sample_dsi|#cp -f sample/mipi_tx/sample_dsi|g' $(BUILDDIR)/middleware/Makefile
	[ -e $(BUILDDIR)/middleware/sample/cipher ] || sed -i 's|cp -f sample/cipher/sample_cipher|#cp -f sample/cipher/sample_cipher|g' $(BUILDDIR)/middleware/Makefile
	[ -e $(BUILDDIR)/middleware/sample/cvg ] || sed -i 's|cp -f sample/cvg/sample_cvg|#cp -f sample/cvg/sample_cvg|g' $(BUILDDIR)/middleware/Makefile
	sed -i 's|@cp -f modules/venc/vc_lib/bin/|#@cp -f modules/venc/vc_lib/bin/|g' $(BUILDDIR)/middleware/Makefile
	@touch $@

$(BUILDDIR)/middleware-prepare-configure-stamp: $(BUILDDIR)/middleware-prepare-patch-stamp
	@echo "$(COLOUR_GREEN)Configuring Middleware for $(BOARD)$(END_COLOUR)"
	@touch $@

$(BUILDDIR)/middleware-compile-stamp: $(BUILDDIR)/middleware-prepare-configure-stamp
	@echo "$(COLOUR_GREEN)Building Middleware for $(BOARD)$(END_COLOUR)"
	@cd $(BUILDDIR)/middleware && $(MIDDLEWARE_ENV) $(MAKE) KERNEL_DIR=$(BUILDDIR)/kernel all
	@mkdir -pv $(BUILDDIR)/middleware/install/system/
	@cd $(BUILDDIR)/middleware && $(MIDDLEWARE_ENV) $(MAKE) KERNEL_DIR=$(BUILDDIR)/kernel install DESTDIR=$(BUILDDIR)/middleware/install/system
	@touch $@

$(BUILDDIR)/middleware-package-stamp: $(BUILDDIR)/middleware-compile-stamp
	@echo "$(COLOUR_GREEN)Packaging Middleware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)
	@cp -r /builder/deb/cvitek-middleware/* $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/
	@mkdir -pv $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/system/
	@rsync -avpPxH $(BUILDDIR)/middleware/install/system/ $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/system/
	@mkdir -pv $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/usr/include/
	@mkdir -pv $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/usr/lib/
	@cp -p $(BUILDDIR)/middleware/sample/test_mmf/maix_mmf.h $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/usr/include/
	@cp -p $(BUILDDIR)/middleware/sample/test_mmf/maix_mmf/release.linux/libmaix_mmf.a $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/usr/lib/
	@sed -i 's/Version: 1.0.0/Version: $(MIDDLEWAREVERSION)/' $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@sed -i 's/Package: cvitek-middleware/Package: cvitek-middleware-$(BOARD)/' $(BUILDDIR)/package/cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@cd $(BUILDDIR)/package/ && dpkg-deb --build cvitek-middleware-$(BOARD)-$(MIDDLEWAREVERSION) $(BUILDDIR)/package/cvitek-middleware-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb
	@cp $(BUILDDIR)/package/cvitek-middleware-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb /output/
	@touch $@

middleware: $(BUILDDIR)/middleware-package-stamp

middleware-clean:
	@rm -rf $(BUILDDIR)/middleware
	@rm -f $(BUILDDIR)/middleware-*-stamp


$(BUILDDIR)/vcodec-firmware-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out vcodec-firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/vcodec-firmware
	@cd $(BUILDDIR)/vcodec-firmware && wget -N https://github.com/sophgo/ramdisk/raw/8bf2a7446007a2ce02aba56622303a7a5906f91c/rootfs/common_musl_riscv64/usr/share/fw_vcodec/coda980.bin
	@cd $(BUILDDIR)/vcodec-firmware && wget -N https://github.com/sophgo/ramdisk/raw/8bf2a7446007a2ce02aba56622303a7a5906f91c/rootfs/common_musl_riscv64/usr/share/fw_vcodec/monet.bin
	@touch $@

$(BUILDDIR)/vcodec-firmware-package-stamp: $(BUILDDIR)/vcodec-firmware-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Packaging vcodec-firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/package/firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION)
	@cp -r /builder/deb/firmware-vcodec-cv181x/* $(BUILDDIR)/package/firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION)/
	@mkdir -pv $(BUILDDIR)/package/firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION)/usr/share/fw_vcodec/
	@rsync -avpPxH $(BUILDDIR)/vcodec-firmware/ $(BUILDDIR)/package/firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION)/usr/share/fw_vcodec/
	@sed -i 's/Version: 1.0.0/Version: $(MIDDLEWAREVERSION)/' $(BUILDDIR)/package/firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@sed -i 's/Package: firmware-vcodec-cv181x/Package: firmware-vcodec-$(CHIP)/' $(BUILDDIR)/package/firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@cd $(BUILDDIR)/package/ && dpkg-deb --build firmware-vcodec-$(CHIP)-$(MIDDLEWAREVERSION) firmware-vcodec-$(CHIP)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb
	@cp $(BUILDDIR)/package/firmware-vcodec-$(CHIP)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb /output/
	@touch $@

vcodec-firmware: $(BUILDDIR)/vcodec-firmware-package-stamp

vcodec-firmware-clean:
	@rm -rf $(BUILDDIR)/vcodec-firmware
	@rm -f $(BUILDDIR)/vcodec-firmware-*-stamp


$(BUILDDIR)/uboot-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out U-Boot for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone $(GIT_CLONE_OPTS) https://github.com/sophgo/u-boot-2021.10.git $(BUILDDIR)/u-boot
	@cd $(BUILDDIR)/u-boot && git checkout 4a21b6b
	@touch $@

$(BUILDDIR)/uboot-prepare-patch-stamp: $(BUILDDIR)/uboot-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching U-Boot for $(BOARD)$(END_COLOUR)"
	@$(foreach file, $(wildcard /configs/common/patches/u-boot/*.patch), cd $(BUILDDIR)/u-boot && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/$(BOARD)/patches/u-boot/*.patch), cd $(BUILDDIR)/u-boot && git apply --ignore-whitespace $(file);)
	@cp /configs/$(BOARD)/u-boot/defconfig $(BUILDDIR)/u-boot/configs/$(BOARD)_defconfig
	@$(foreach file, $(wildcard /configs/common/dts/$(CHIP)/*), cp $(file) $(BUILDDIR)/u-boot/arch/riscv/dts/;)
	@$(foreach file, $(wildcard /configs/common/dts/$(CHIP)_$(ARCH)/*), cp $(file) $(BUILDDIR)/u-boot/arch/riscv/dts/;)
	@$(foreach file, $(wildcard /configs/$(BOARD)/dts/*), cp $(file) $(BUILDDIR)/u-boot/arch/riscv/dts/;)
	@cp /configs/$(BOARD)/u-boot/cvitek.h $(BUILDDIR)/u-boot/include/cvitek.h
	@cp /configs/$(BOARD)/u-boot/cvi_board_init.c $(BUILDDIR)/u-boot/board/cvitek/cvi_board_init.c
	@python3 /builder/python/mmap_conv.py --type h /configs/$(BOARD)/memmap.py $(BUILDDIR)/u-boot/include/cvi_board_memmap.h
	@python3 /builder/python/mkcvipart.py /configs/$(BOARD)/$(PARTITION_FILE) $(BUILDDIR)/u-boot/include/
	@python3 /builder/python/mk_imgHeader.py /configs/$(BOARD)/$(PARTITION_FILE) $(BUILDDIR)/u-boot/include/ 
	@touch $@

$(BUILDDIR)/uboot-prepare-configure-stamp: $(BUILDDIR)/uboot-prepare-patch-stamp
	@echo "$(COLOUR_GREEN)Configuring U-Boot for $(BOARD)$(END_COLOUR)"
	cd $(BUILDDIR)/u-boot/ && $(MAKE) -j$(NPROCS) BOARD=cv181x CONFIG_USE_DEFAULT_ENV=y STORAGE_TYPE=$(STORAGE_TYPE) CHIP=$(UBOOT_CHIP) CVIBOARD=$(UBOOT_BOARD) CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" $(BOARD)_defconfig
	@touch $@

$(BUILDDIR)/uboot-compile-stamp: $(BUILDDIR)/uboot-prepare-configure-stamp
	@echo "$(COLOUR_GREEN)Building U-Boot for $(BOARD)$(END_COLOUR)"
	cd $(BUILDDIR)/u-boot/ && $(MAKE) -j$(NPROCS) BOARD=cv181x CONFIG_USE_DEFAULT_ENV=y STORAGE_TYPE=$(STORAGE_TYPE) CHIP=$(UBOOT_CHIP) CVIBOARD=$(UBOOT_BOARD) CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-"
	cd $(BUILDDIR)/u-boot/ && $(MAKE) -j$(NPROCS) BOARD=cv181x CONFIG_USE_DEFAULT_ENV=y STORAGE_TYPE=$(STORAGE_TYPE) CHIP=$(UBOOT_CHIP) CVIBOARD=$(UBOOT_BOARD) CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" u-boot-initial-env
	@cp $(BUILDDIR)/u-boot/u-boot.bin $(BUILDDIR)
	@cp $(BUILDDIR)/u-boot/u-boot.dtb $(BUILDDIR)
	@cp $(BUILDDIR)/u-boot/u-boot-initial-env $(BUILDDIR)
	@touch $@

uboot: $(BUILDDIR)/uboot-compile-stamp

uboot-clean:
	@rm -rf $(BUILDDIR)/u-boot
	@rm -f $(BUILDDIR)/uboot-*-stamp


$(BUILDDIR)/opensbi-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out OpenSBI for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b sg200x-dev $(GIT_CLONE_OPTS) https://github.com/sophgo/opensbi.git $(BUILDDIR)/opensbi
	@cd $(BUILDDIR)/opensbi && git checkout 216793f
#	git clone https://github.com/riscv-software-src/opensbi.git $(BUILDDIR)/opensbi
#	@cd $(BUILDDIR)/opensbi && git checkout a2b255b
	@touch $@


$(BUILDDIR)/opensbi-prepare-patch-stamp: $(BUILDDIR)/opensbi-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching OpenSBI for $(BOARD)$(END_COLOUR)"
	@$(foreach file, $(wildcard /configs/common/patches/opensbi/*.patch), cd $(BUILDDIR)/opensbi && git apply --ignore-whitespace $(file);)
	@$(foreach file, $(wildcard /configs/$(BOARD)/patches/opensbi/*.patch), cd $(BUILDDIR)/opensbi && git apply --ignore-whitespace $(file);)
	@touch $@

$(BUILDDIR)/opensbi-compile-stamp: $(BUILDDIR)/opensbi-prepare-patch-stamp $(BUILDDIR)/uboot-compile-stamp
	@echo "$(COLOUR_GREEN)Building OpenSBI for $(BOARD)$(END_COLOUR)"
	@cd $(BUILDDIR)/opensbi && CROSS_COMPILE="/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-" make PLATFORM=generic FW_FDT_PATH=$(BUILDDIR)/u-boot.dtb
	@cp $(BUILDDIR)/opensbi/build/platform/generic/firmware/fw_dynamic.bin $(BUILDDIR) 
	@touch $@

opensbi: $(BUILDDIR)/opensbi-compile-stamp

opensbi-clean:
	@rm -rf $(BUILDDIR)/opensbi
	@rm -f $(BUILDDIR)/opensbi-*-stamp


$(BUILDDIR)/fsbl-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out FSBL for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b sg200x-dev $(GIT_CLONE_OPTS) https://github.com/sophgo/fsbl.git $(BUILDDIR)/fsbl
	@cd $(BUILDDIR)/fsbl && git checkout 2039113
	@touch $@

$(BUILDDIR)/fsbl-prepare-patch-stamp: $(BUILDDIR)/fsbl-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching FSBL for $(BOARD)$(END_COLOUR)"
	$(foreach file, $(wildcard /configs/common/patches/fsbl/*.patch), cd $(BUILDDIR)/fsbl && git apply --ignore-whitespace $(file);)
	$(foreach file, $(wildcard /configs/$(BOARD)/patches/fsbl/*.patch), cd $(BUILDDIR)/fsbl && git apply --ignore-whitespace $(file);)
	@python3 /builder/python/mmap_conv.py --type h /configs/$(BOARD)/memmap.py $(BUILDDIR)/fsbl/plat/$(CHIP)/include/cvi_board_memmap.h
	@printf '\163\000\120\020\157\360\337\377' > $(BUILDDIR)/fsbl/blank.bin
	@touch $@

$(BUILDDIR)/fsbl-compile-stamp: $(BUILDDIR)/fsbl-prepare-patch-stamp $(BUILDDIR)/opensbi-compile-stamp $(BUILDDIR)/uboot-compile-stamp $(FSBL_RTOS_TARGET)
	@echo "$(COLOUR_GREEN)Building FSBL for $(BOARD)$(END_COLOUR)"
	@test -s $(FSBL_BLCP_2ND_PATH)
	@cd $(BUILDDIR)/fsbl && OD_CLK_SEL=y $(MAKE) -j$(NPROCS) ARCH=$(ARCH) CHIP_ARCH=$(CHIP) BOOT_CPU=$(BOOT_CPU) DDR_CFG=$(DDR_CFG) RTOS_ENABLE_FREERTOS=y BLCP_2ND_PATH=$(FSBL_BLCP_2ND_PATH) LOADER_2ND_PATH=$(BUILDDIR)/u-boot.bin \
    CROSS_COMPILE=/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl- 
	@cp $(BUILDDIR)/fsbl/build/$(CHIP)/fip.bin $(BUILDDIR)
	@touch $@

$(BUILDDIR)/fsbl-package-stamp: $(BUILDDIR)/fsbl-compile-stamp
	@echo "$(COLOUR_GREEN)Packaging FSBL for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)
	@cp -r /builder/deb/cvitek-fsbl/* $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/
	@mkdir -p $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/boot/ $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/etc/
	@cp $(BUILDDIR)/fip.bin $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/boot/
	@cp $(BUILDDIR)/u-boot-initial-env $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/etc/
	@sed -i 's/Version: 1.0.0/Version: $(FSBLVERSION)/' $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/DEBIAN/control
	@sed -i 's/Package: cvitek-fsbl/Package: cvitek-fsbl-$(BOARD)/' $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION)/DEBIAN/control
	@cd $(BUILDDIR)/package/ && dpkg-deb --build cvitek-fsbl-$(BOARD)-$(FSBLVERSION)
	@cp $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION).deb /output/
	@touch $@

fsbl: $(BUILDDIR)/fsbl-package-stamp

fsbl-clean:
	@rm -rf $(BUILDDIR)/fsbl
	@rm -rf $(BUILDDIR)/package/cvitek-fsbl-*
	@rm -f $(BUILDDIR)/fsbl-*-stamp


$(BUILDDIR)/cvi-pinmux-package-stamp:
	@echo "$(COLOUR_GREEN)Building pinned cvi_pinmux for $(BOARD)$(END_COLOUR)"
	@rm -rf $(PINMUX_SRC) $(PINMUX_PKG)
	@mkdir -p $(PINMUX_SRC) $(PINMUX_PKG)/DEBIAN $(PINMUX_PKG)/usr/bin
	@curl --fail --location --retry 5 --retry-delay 3 --retry-all-errors --connect-timeout 20 \
		https://github.com/Fishwaldo/sophgo-sg200x-packages/archive/$(PINMUX_COMMIT).tar.gz \
		-o $(BUILDDIR)/cvi-pinmux-cv181x.tar.gz
	@tar -xzf $(BUILDDIR)/cvi-pinmux-cv181x.tar.gz --strip-components=2 -C $(PINMUX_SRC) \
		sophgo-sg200x-packages-$(PINMUX_COMMIT)/cvi-pinmux-cv181x
	@$(PINMUX_CC) -static -Os -s -Wall -Wextra -o $(PINMUX_PKG)/usr/bin/cvi_pinmux \
		$(PINMUX_SRC)/cvi_pinmux.c $(PINMUX_SRC)/devmem.c
	@printf 'Package: cvi-pinmux-cv181x\nVersion: $(PINMUX_VERSION)\nArchitecture: riscv64\nMaintainer: MaixCAM image automation\nDescription: Pinmux control for CVITEK CV181x chips\n' > $(PINMUX_PKG)/DEBIAN/control
	@chmod 0755 $(PINMUX_PKG)/usr/bin/cvi_pinmux
	@dpkg-deb --build $(PINMUX_PKG) $(BUILDDIR)/package/cvi-pinmux-cv181x_$(PINMUX_VERSION)_$(DEB_ARCH).deb
	@cp $(BUILDDIR)/package/cvi-pinmux-cv181x_$(PINMUX_VERSION)_$(DEB_ARCH).deb /output/
	@touch $@


$(BUILDDIR)/image-prepare-stamp: 
	@echo "$(COLOUR_GREEN)Preparing Image for $(BOARD)$(END_COLOUR)"
	@-mkdir $(BUILDDIR)
	@rm -rf /rootfs/
	@-rm $(addon-targets)
	@mkdir -p /rootfs/

ifeq ($(BOARD),licheervnano)
	@mmdebstrap -v --architectures=riscv64 --include="$(_PACKAGES)" sid "/rootfs/" "deb http://deb.debian.org/debian/ sid main"
else
	@curl --fail --location --retry 5 --retry-delay 3 --retry-all-errors --connect-timeout 20 https://sophgo.my-ho.st/public-key.asc -o $(BUILDDIR)/public-key.asc
	@mmdebstrap -v --architectures=riscv64 --include="$(_PACKAGES)" sid "/rootfs/" "deb http://deb.debian.org/debian/ sid main" "deb [signed-by=$(BUILDDIR)/public-key.asc] https://sophgo.my-ho.st:8443/ debian sophgo"

endif
	@touch $@

$(BUILDDIR)/image-addons-stamp: $(BUILDDIR)/image-prepare-stamp $(BUILDDIR)/fsbl-package-stamp $(BUILDDIR)/linux-compile-stamp $(BUILDDIR)/osdrv-package-stamp $(BUILDDIR)/middleware-package-stamp $(BUILDDIR)/vcodec-firmware-package-stamp $(LOCAL_PINMUX_TARGET) $(addon-targets)
	@[ -e /rootfs/usr/bin/devmem ] || ln -s busybox /rootfs/usr/bin/devmem
	@echo "$(COLOUR_GREEN)Copying Deb files for installation on $(BOARD)$(END_COLOUR)"
	@cp $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION).deb /rootfs/tmp/install/
	@cp $(BUILDDIR)/package/cvitek-osdrv-*$(BOARD)*.deb /rootfs/tmp/install/
	@cp $(BUILDDIR)/package/cvitek-middleware-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb /rootfs/tmp/install/
	@cp $(BUILDDIR)/package/firmware-vcodec-$(CHIP)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb /rootfs/tmp/install/
	@cp $(BUILDDIR)/linux-image-$(BOARD)-*.deb /rootfs/tmp/install/
	@cp $(BUILDDIR)/linux-headers-*.deb /rootfs/tmp/install/
	@cp $(BUILDDIR)/linux-libc-dev*.deb /rootfs/tmp/install/

ifeq ($(BOARD),licheervnano)
	@cp $(BUILDDIR)/package/cvi-pinmux-cv181x_$(PINMUX_VERSION)_$(DEB_ARCH).deb /rootfs/tmp/install/

endif
	@touch $@


$(BUILDDIR)/image-customize-stamp: $(BUILDDIR)/image-addons-stamp $(BUILDDIR)/linux-compile-stamp $(BUILDDIR)/fsbl-package-stamp
	@echo "$(COLOUR_GREEN)Customizing Image for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/tmp/install/
	@echo $(BOARD) > /rootfs/tmp/install/hostname
	@echo $(BOARD) > /rootfs/tmp/install/board
	@echo $(STORAGE_TYPE) > /rootfs/tmp/install/storage
	@sh -ec 'qemu_bin=/usr/bin/qemu-riscv64-static; \
		if [ ! -x "$${qemu_bin}" ]; then qemu_bin=/usr/bin/qemu-riscv64; fi; \
		cp -v "$${qemu_bin}" /rootfs/tmp/install/qemu-riscv64-static'
	@cp -v /builder/setup_rootfs.sh /rootfs/tmp/install/

ifneq ($(BOARD),licheervnano)
	@cp -v $(BUILDDIR)/public-key.asc /rootfs/tmp/install/

endif
	@chroot /rootfs/ /tmp/install/qemu-riscv64-static /bin/sh /tmp/install/setup_rootfs.sh
	@rm -rf /rootfs/tmp/install/
	@umount /rootfs/proc || true 
	@umount /rootfs/sys || true 
	@umount /rootfs/run || true 
	@umount /rootfs/dev || true
	@touch $@

$(BUILDDIR)/image-compile-stamp: $(BUILDDIR)/image-customize-stamp
	@echo "$(COLOUR_GREEN)Compiling Image for $(BOARD)$(END_COLOUR)"
	@rm -rf /tmp/genimage/
	@cd $(BUILDDIR) && genimage --config /builder/genimage_$(STORAGE_TYPE).cfg --tmppath /tmp/genimage --rootpath /rootfs/
	@rm -rf /tmp/genimage/
	@if [ "$(STORAGE_TYPE)" = "emmc" ]; then \
		python3 /builder/python/raw2cimg.py -v $(BUILDDIR)/images/sdcard.img $(BUILDDIR)/images /configs/$(BOARD)/partition_emmc.xml; \
		mkdir -p /tmp/rom/; \
		cp $(BUILDDIR)/images/sdcard.img /tmp/rom/; \
		cp /configs/$(BOARD)/partition_emmc.xml /tmp/rom/; \
		cp $(BUILDDIR)/fip.bin /tmp/rom/; \
		cd /tmp && zip $(BOARD)_$(STORAGE_TYPE).zip -r rom/; \
		cp /tmp/$(BOARD)_$(STORAGE_TYPE).zip /output/; \
		echo "$(COLOUR_GREEN)Image for $(BOARD) is $(BOARD)_$(STORAGE_TYPE).zip$(END_COLOUR)"; \
	else \
		cp $(BUILDDIR)/images/sdcard.img /output/$(BOARD)_$(STORAGE_TYPE).img; \
		echo "$(COLOUR_GREEN)Image for $(BOARD) is $(BOARD)_$(STORAGE_TYPE).img$(END_COLOUR)"; \
	fi 
	@touch $@

image: $(BUILDDIR)/image-compile-stamp

image-clean:
	@rm -rf /rootfs/
	@rm -f $(BUILDDIR)/image-*-stamp $(addon-targets)
	@rm -f /output/$(BOARD)_$(STORAGE_TYPE).img

image-clean-customize:
	@rm -f $(BUILDDIR)/image-customize-stamp

clean: opensbi-clean uboot-clean linux-clean osdrv-clean middleware-clean fsbl-clean

.PHONY: image clean opensbi uboot linux osdrv middleware fsbl fsbl-clean uboot-clean linux-clean opensbi-clean
