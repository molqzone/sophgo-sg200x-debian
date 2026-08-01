COLOUR_GREEN=\n\033[0;32m
COLOUR_RED=\033[0;31m
COLOUR_BLUE=\033[0;34m
END_COLOUR=\033[0m\n

.DEFAULT_GOAL := image

red:=$(shell tput setaf 1)
blue:=$(shell tput setaf 4)
reset:=$(shell tput sgr0)

BUILDDIR ?= /build
ROOTFS ?= /rootfs
CONFIG_ROOT ?= /configs
COMPONENTS_ROOT ?= /workspace/components
OUTPUT_DIR ?= /output
VERSION_FILE ?= /workspace/versions.env

include $(VERSION_FILE)

BUILDDIR := $(patsubst %/,%,$(BUILDDIR))
ROOTFS := $(patsubst %/,%,$(ROOTFS))
CROSS_COMPILE ?= /host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-
RTOS_CROSS_COMPILE ?= /host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-
HOST_TOOLS ?= /host-tools
SDK_CACHE ?= $(BUILDDIR)/sdk-cache

DEB_ARCH = riscv64

GIT_CLONE_OPTS = --shallow-since=2020-03-12

ifndef BOARD
$(error $(red)BOARD is not set$(reset))
endif

PLAN_TOOL ?= /workspace/scripts/ci/plan.py
ENABLE_MEDIA_STACK ?= 0
ENABLE_SG2002_IPC ?= 0
ENABLE_LOCAL_PINMUX ?= 0
USE_VENDOR_APT ?= 1

BOARD_CHAIN := $(shell python3 $(PLAN_TOOL) --config-root $(CONFIG_ROOT) board-chain --board $(BOARD) --order precedence 2>/dev/null)
ifeq ($(strip $(BOARD_CHAIN)),)
$(error $(red)Board $(BOARD) is not supported$(reset))
endif
BOARD_LAYERS := $(shell python3 $(PLAN_TOOL) --config-root $(CONFIG_ROOT) board-chain --board $(BOARD) --order application)
BOARD_CONFIG_LAYERS := $(filter-out common,$(BOARD_LAYERS))
BOARD_SETTINGS := $(shell python3 $(PLAN_TOOL) --config-root $(CONFIG_ROOT) settings --board $(BOARD))

include $(CONFIG_ROOT)/settings.mk
include $(BOARD_SETTINGS)

resolve-board-file = $(shell python3 $(PLAN_TOOL) --config-root $(CONFIG_ROOT) resolve --board $(BOARD) --path $(1))
board-patches = $(shell python3 $(PLAN_TOOL) --config-root $(CONFIG_ROOT) patches --board $(BOARD) --component $(1))

BOARD_LINUX_DEFCONFIG := $(call resolve-board-file,linux/defconfig)
BOARD_MEMMAP := $(call resolve-board-file,memmap.py)
BOARD_UBOOT_DEFCONFIG := $(call resolve-board-file,u-boot/defconfig)
BOARD_UBOOT_HEADER := $(call resolve-board-file,u-boot/cvitek.h)
BOARD_UBOOT_INIT := $(call resolve-board-file,u-boot/cvi_board_init.c)
BOARD_PARTITION_FILE := $(call resolve-board-file,$(PARTITION_FILE))
BOARD_DTS_FILES := $(foreach layer,$(BOARD_CONFIG_LAYERS),$(wildcard $(CONFIG_ROOT)/$(layer)/dts/*))
LINUX_PATCHES := $(call board-patches,linux)
OSDRV_PATCHES := $(call board-patches,osdrv)
MIDDLEWARE_PATCHES := $(call board-patches,middleware)
SENSOR_PATCHES := $(call board-patches,sensorsupportlist)
UBOOT_PATCHES := $(call board-patches,u-boot)
OPENSBI_PATCHES := $(call board-patches,opensbi)
FSBL_PATCHES := $(call board-patches,fsbl)

OSDRV_ENV = CHIP_ARCH=CV181X \
CVIARCH=CV181X \
SDK_VER=musl_riscv64 \
CROSS_COMPILE_MUSL_RISCV64=$(CROSS_COMPILE) \
CONFIG_ARCH=riscv \
CONFIG_CROSS_COMPILE_KERNEL="$(CROSS_COMPILE)" \
CONFIG_CP_EXT_WIRELESS=y

ifneq ($(findstring # CONFIG_FB is not set,$(shell cat $(BOARD_LINUX_DEFCONFIG) 2>/dev/null)),)
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

include $(wildcard /builder/addons/*/addon.mk)

strip-quotes = $(subst ",,$(1))
addon-targets = $(addprefix $(BUILDDIR)/,$(addsuffix -stamp,$(call strip-quotes,$(IMAGE_ADDITIONS))))
_PACKAGES = $(call strip-quotes,$(PACKAGES))

FSBL_BLCP_2ND_PATH := $(BUILDDIR)/fsbl/blank.bin
ifneq ($(filter $(BUILDDIR)/sg2002-ipc-stamp,$(addon-targets)),)
FSBL_RTOS_TARGET := $(BUILDDIR)/sg2002-ipc-build-stamp
FSBL_BLCP_2ND_PATH := $(SG2002_IPC_FIRMWARE_BIN)
endif

MEDIA_IMAGE_TARGETS :=
ifeq ($(ENABLE_MEDIA_STACK),1)
MEDIA_IMAGE_TARGETS += $(BUILDDIR)/osdrv-package-stamp
MEDIA_IMAGE_TARGETS += $(BUILDDIR)/middleware-package-stamp
MEDIA_IMAGE_TARGETS += $(BUILDDIR)/vcodec-firmware-package-stamp
endif


$(info $(blue)Board: $(BOARD)$(reset))
$(info $(blue)Storage: $(STORAGE_TYPE)$(reset))
$(info $(blue)Image Addons: $(IMAGE_ADDITIONS)$(reset))
$(info $(blue)Packages: $(_PACKAGES)$(reset))

NPROCS := $(shell nproc)

PINMUX_SRC = $(BUILDDIR)/cvi-pinmux-cv181x
PINMUX_PKG = $(BUILDDIR)/package/cvi-pinmux-cv181x-$(PINMUX_VERSION)
PINMUX_CC = $(CROSS_COMPILE)gcc

ifeq ($(ENABLE_LOCAL_PINMUX),1)
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
	@for kodir in $(KO_DIRS); do \
		find ${1}/lib/modules -type f -path "*/kernel/$$kodir/*.ko" \
			-exec cp -f {} $(BUILDDIR)/osdrv/ko/ \; ; \
	done
endef

$(BUILDDIR)/linux-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out Kernel for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b $(LINUX_BRANCH) $(GIT_CLONE_OPTS) $(LINUX_REPO) $(BUILDDIR)/kernel
	@cd $(BUILDDIR)/kernel && git checkout $(LINUX_COMMIT)
	@touch $@

$(BUILDDIR)/linux-prepare-patch-stamp: $(BUILDDIR)/linux-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching Kernel for $(BOARD)$(END_COLOUR)"
	@$(foreach file,$(LINUX_PATCHES),cd $(BUILDDIR)/kernel && git apply --ignore-whitespace $(file);)
	@if [ "$(ENABLE_SG2002_IPC)" = "1" ]; then \
		grep -Fq "firmware has no resource table; using mailbox-only mode" $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c; \
		grep -Fq "failed to %s C906L via SBI: %d" $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c; \
		grep -Fq "boot trace host sentinel at 0x%llx" $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c; \
		grep -Fq 'return "reply_posted"' $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c; \
		grep -Fq 'cvitek,boot-trace' $(BUILDDIR)/kernel/drivers/remoteproc/cvitek_remoteproc.c; \
	fi
	@cp $(BOARD_LINUX_DEFCONFIG) $(BUILDDIR)/kernel/arch/riscv/configs/${BOARD}_defconfig
	@$(foreach file, $(wildcard $(CONFIG_ROOT)/common/dts/$(CHIP)/*), cp $(file) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/;)
	@$(foreach file, $(wildcard $(CONFIG_ROOT)/common/dts/$(CHIP)_$(ARCH)/*), cp $(file) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/;)
	@$(foreach file,$(BOARD_DTS_FILES),cp $(file) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/;)
	@python3 /builder/python/mmap_conv.py --type h $(BOARD_MEMMAP) $(BUILDDIR)/kernel/arch/riscv/boot/dts/cvitek/cvi_board_memmap.h
	@touch $@

$(BUILDDIR)/linux-prepare-configure-stamp: $(BUILDDIR)/linux-prepare-patch-stamp
	@echo "$(COLOUR_GREEN)Configuring Kernel for $(BOARD)$(END_COLOUR)"
	@cd $(BUILDDIR)/kernel KDEB_SOURCENAME=linux-sophgo  && $(MAKE) -j$(NPROCS) O=$(BUILDDIR)/kernel/ ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" ${BOARD}_defconfig
	@$(eval LV=$(shell cd $(BUILDDIR)/kernel && git log -1 --format="%at" | xargs -I{} date -d @{} +-%Y%m%d-${KERNELREV}))
	@sed -i 's/CONFIG_LOCALVERSION=""/CONFIG_LOCALVERSION="$(LV)"/' $(BUILDDIR)/kernel/.config
	@touch $@

$(BUILDDIR)/linux-compile-stamp: $(BUILDDIR)/linux-prepare-configure-stamp
	@echo "$(COLOUR_GREEN)Building Kernel for $(BOARD)$(END_COLOUR)"
	@sed -i 's/packagename=linux-image.*/packagename=linux-image-$(BOARD)/' $(BUILDDIR)/kernel/scripts/package/mkdebian
	@sed -i 's/packagename=linux-image-$$version/packagename=linux-image-$(BOARD)-$$version/' $(BUILDDIR)/kernel/scripts/package/builddeb
	@cd $(BUILDDIR)/kernel && KCFLAGS=-Wno-attribute-alias ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" KDEB_SOURCENAME=linux-$(BOARD) $(MAKE) -j$(NPROCS) O=$(BUILDDIR)/kernel/ bindeb-pkg
	@cd $(BUILDDIR)/kernel && KCFLAGS=-Wno-attribute-alias ARCH=riscv CROSS_COMPILE="$(CROSS_COMPILE)" KDEB_SOURCENAME=linux-$(BOARD) $(MAKE) -j$(NPROCS) O=$(BUILDDIR)/kernel/ modules_install INSTALL_MOD_PATH=$(BUILDDIR)/kernel/modules headers_install INSTALL_HDR_PATH=$(BUILDDIR)/kernel/riscv/usr
	@cp $(BUILDDIR)/*.deb $(OUTPUT_DIR)/
	@touch $@

linux: $(BUILDDIR)/linux-compile-stamp

linux-clean:
	@rm -rf $(BUILDDIR)/kernel
	@rm -f $(BUILDDIR)/linux-*-stamp
	@rm -f $(BUILDDIR)/linux-*.deb


$(BUILDDIR)/osdrv-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out OSdrv for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b $(OSDRV_BRANCH) $(GIT_CLONE_OPTS) $(OSDRV_REPO) $(BUILDDIR)/osdrv
	@cd $(BUILDDIR)/osdrv && git checkout $(OSDRV_COMMIT)
	@touch $@

$(BUILDDIR)/osdrv-prepare-patch-stamp: $(BUILDDIR)/osdrv-prepare-checkout-stamp $(BUILDDIR)/linux-compile-stamp
	@echo "$(COLOUR_GREEN)Patching OSdrv for $(BOARD)$(END_COLOUR)"
	$(call copy_ko_action, $(BUILDDIR)/kernel/modules)
	@$(foreach file,$(OSDRV_PATCHES),cd $(BUILDDIR)/osdrv && git apply --ignore-whitespace $(file);)
	@if [ "$(ENABLE_SG2002_IPC)" = "1" ]; then \
		for file in $(SG2002_IPC_COMPONENT)/ports/osdrv/patches/*.patch; do \
			cd $(BUILDDIR)/osdrv && git apply --ignore-whitespace "$$file"; \
		done; \
		cp $(SG2002_IPC_COMPONENT)/include/sg2002_rtos_shm.h $(BUILDDIR)/osdrv/interdrv/rtos_cmdqu/; \
		grep -Fq '(cmdq->cmd_id >= SYS_CMD_INFO_LIMIT && cmdq->ip_id != IP_SYSTEM)' $(BUILDDIR)/osdrv/interdrv/rtos_cmdqu/rtos_cmdqu.c; \
		grep -Fq 'static DEFINE_MUTEX(send_wait_lock);' $(BUILDDIR)/osdrv/interdrv/rtos_cmdqu/rtos_cmdqu.c; \
		grep -Fq 'wait_list = kzalloc(sizeof(struct rtos_cmdqu_wait_list_t), GFP_KERNEL);' $(BUILDDIR)/osdrv/interdrv/rtos_cmdqu/rtos_cmdqu.c; \
		grep -Fq 'ret = rtos_cmdqu_send_wait(&cmdq, cmdq.cmd_id);' $(BUILDDIR)/osdrv/interdrv/rtos_cmdqu/rtos_cmdqu.c; \
		grep -Fq 'SG2002_RTOS_SHM_ACQUIRE' $(BUILDDIR)/osdrv/interdrv/rtos_cmdqu/rtos_cmdqu.c; \
	fi
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
	@cp $(BUILDDIR)/package/cvitek-osdrv-$(BOARD)-$(KERNELRELEASE)_$(OSDRVVERSION)-$(KERNELRELEASE)_$(DEB_ARCH).deb $(OUTPUT_DIR)/
	@touch $@

osdrv: $(BUILDDIR)/osdrv-package-stamp

osdrv-clean:
	@rm -rf $(BUILDDIR)/osdrv
	@rm -f $(BUILDDIR)/osdrv-*-stamp


$(BUILDDIR)/middleware-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out Middleware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b $(MIDDLEWARE_BRANCH) $(GIT_CLONE_OPTS) --recursive $(MIDDLEWARE_REPO) $(BUILDDIR)/middleware
	@cd $(BUILDDIR)/middleware && git checkout $(MIDDLEWARE_COMMIT)
	@git clone -b $(SENSOR_SUPPORT_BRANCH) $(GIT_CLONE_OPTS) --recursive $(SENSOR_SUPPORT_REPO) $(BUILDDIR)/middleware/component/isp
	@cd $(BUILDDIR)/middleware/component/isp && git checkout $(SENSOR_SUPPORT_COMMIT)
	@mkdir -p $(BUILDDIR)/middleware/sample/test_mmf
	@git clone -b $(MEDIA_SERVER_BRANCH) $(GIT_CLONE_OPTS) --recursive $(MEDIA_SERVER_REPO) $(BUILDDIR)/middleware/sample/test_mmf/media_server-1.0.x
	@cd $(BUILDDIR)/middleware/sample/test_mmf/media_server-1.0.x && git checkout $(MEDIA_SERVER_COMMIT)
	@touch $@

$(BUILDDIR)/middleware-prepare-patch-stamp: $(BUILDDIR)/middleware-prepare-checkout-stamp $(BUILDDIR)/osdrv-compile-stamp
	@echo "$(COLOUR_GREEN)Patching Middleware for $(BOARD)$(END_COLOUR)"
	mkdir -pv $(BUILDDIR)/kernel/riscv/usr/include/linux/
	$(call copy_header_action, $(BUILDDIR)/kernel/riscv/usr/include)
	@$(foreach file,$(MIDDLEWARE_PATCHES),cd $(BUILDDIR)/middleware && git apply --ignore-whitespace $(file);)
	@$(foreach file,$(SENSOR_PATCHES),cd $(BUILDDIR)/middleware/component/isp && git apply --ignore-whitespace $(file);)
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
	@cp $(BUILDDIR)/package/cvitek-middleware-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb $(OUTPUT_DIR)/
	@touch $@

middleware: $(BUILDDIR)/middleware-package-stamp

middleware-clean:
	@rm -rf $(BUILDDIR)/middleware
	@rm -f $(BUILDDIR)/middleware-*-stamp


$(BUILDDIR)/vcodec-firmware-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out vcodec-firmware for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/vcodec-firmware
	@cd $(BUILDDIR)/vcodec-firmware && wget -N https://github.com/sophgo/ramdisk/raw/$(VCODEC_RAMDISK_COMMIT)/rootfs/common_musl_riscv64/usr/share/fw_vcodec/coda980.bin
	@cd $(BUILDDIR)/vcodec-firmware && wget -N https://github.com/sophgo/ramdisk/raw/$(VCODEC_RAMDISK_COMMIT)/rootfs/common_musl_riscv64/usr/share/fw_vcodec/monet.bin
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
	@cp $(BUILDDIR)/package/firmware-vcodec-$(CHIP)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb $(OUTPUT_DIR)/
	@touch $@

vcodec-firmware: $(BUILDDIR)/vcodec-firmware-package-stamp

vcodec-firmware-clean:
	@rm -rf $(BUILDDIR)/vcodec-firmware
	@rm -f $(BUILDDIR)/vcodec-firmware-*-stamp


$(BUILDDIR)/uboot-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out U-Boot for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone $(GIT_CLONE_OPTS) $(UBOOT_REPO) $(BUILDDIR)/u-boot
	@cd $(BUILDDIR)/u-boot && git checkout $(UBOOT_COMMIT)
	@touch $@

$(BUILDDIR)/uboot-prepare-patch-stamp: $(BUILDDIR)/uboot-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching U-Boot for $(BOARD)$(END_COLOUR)"
	@$(foreach file,$(UBOOT_PATCHES),cd $(BUILDDIR)/u-boot && git apply --ignore-whitespace $(file);)
	@cp $(BOARD_UBOOT_DEFCONFIG) $(BUILDDIR)/u-boot/configs/$(BOARD)_defconfig
	@$(foreach file, $(wildcard $(CONFIG_ROOT)/common/dts/$(CHIP)/*), cp $(file) $(BUILDDIR)/u-boot/arch/riscv/dts/;)
	@$(foreach file, $(wildcard $(CONFIG_ROOT)/common/dts/$(CHIP)_$(ARCH)/*), cp $(file) $(BUILDDIR)/u-boot/arch/riscv/dts/;)
	@$(foreach file,$(BOARD_DTS_FILES),cp $(file) $(BUILDDIR)/u-boot/arch/riscv/dts/;)
	@cp $(BOARD_UBOOT_HEADER) $(BUILDDIR)/u-boot/include/cvitek.h
	@cp $(BOARD_UBOOT_INIT) $(BUILDDIR)/u-boot/board/cvitek/cvi_board_init.c
	@python3 /builder/python/mmap_conv.py --type h $(BOARD_MEMMAP) $(BUILDDIR)/u-boot/include/cvi_board_memmap.h
	@python3 /builder/python/mkcvipart.py $(BOARD_PARTITION_FILE) $(BUILDDIR)/u-boot/include/
	@python3 /builder/python/mk_imgHeader.py $(BOARD_PARTITION_FILE) $(BUILDDIR)/u-boot/include/
	@touch $@

$(BUILDDIR)/uboot-prepare-configure-stamp: $(BUILDDIR)/uboot-prepare-patch-stamp
	@echo "$(COLOUR_GREEN)Configuring U-Boot for $(BOARD)$(END_COLOUR)"
	cd $(BUILDDIR)/u-boot/ && $(MAKE) -j$(NPROCS) BOARD=cv181x CONFIG_USE_DEFAULT_ENV=y STORAGE_TYPE=$(STORAGE_TYPE) CHIP=$(UBOOT_CHIP) CVIBOARD=$(UBOOT_BOARD) CROSS_COMPILE="$(CROSS_COMPILE)" $(BOARD)_defconfig
	@touch $@

$(BUILDDIR)/uboot-compile-stamp: $(BUILDDIR)/uboot-prepare-configure-stamp
	@echo "$(COLOUR_GREEN)Building U-Boot for $(BOARD)$(END_COLOUR)"
	cd $(BUILDDIR)/u-boot/ && $(MAKE) -j$(NPROCS) BOARD=cv181x CONFIG_USE_DEFAULT_ENV=y STORAGE_TYPE=$(STORAGE_TYPE) CHIP=$(UBOOT_CHIP) CVIBOARD=$(UBOOT_BOARD) CROSS_COMPILE="$(CROSS_COMPILE)"
	cd $(BUILDDIR)/u-boot/ && $(MAKE) -j$(NPROCS) BOARD=cv181x CONFIG_USE_DEFAULT_ENV=y STORAGE_TYPE=$(STORAGE_TYPE) CHIP=$(UBOOT_CHIP) CVIBOARD=$(UBOOT_BOARD) CROSS_COMPILE="$(CROSS_COMPILE)" u-boot-initial-env
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
	@git clone -b $(OPENSBI_BRANCH) $(GIT_CLONE_OPTS) $(OPENSBI_REPO) $(BUILDDIR)/opensbi
	@cd $(BUILDDIR)/opensbi && git checkout $(OPENSBI_COMMIT)
#	git clone https://github.com/riscv-software-src/opensbi.git $(BUILDDIR)/opensbi
#	@cd $(BUILDDIR)/opensbi && git checkout a2b255b
	@touch $@


$(BUILDDIR)/opensbi-prepare-patch-stamp: $(BUILDDIR)/opensbi-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching OpenSBI for $(BOARD)$(END_COLOUR)"
	@$(foreach file,$(OPENSBI_PATCHES),cd $(BUILDDIR)/opensbi && git apply --ignore-whitespace $(file);)
	@touch $@

$(BUILDDIR)/opensbi-compile-stamp: $(BUILDDIR)/opensbi-prepare-patch-stamp $(BUILDDIR)/uboot-compile-stamp
	@echo "$(COLOUR_GREEN)Building OpenSBI for $(BOARD)$(END_COLOUR)"
	@cd $(BUILDDIR)/opensbi && CROSS_COMPILE="$(CROSS_COMPILE)" make PLATFORM=generic FW_FDT_PATH=$(BUILDDIR)/u-boot.dtb
	@cp $(BUILDDIR)/opensbi/build/platform/generic/firmware/fw_dynamic.bin $(BUILDDIR) 
	@touch $@

opensbi: $(BUILDDIR)/opensbi-compile-stamp

opensbi-clean:
	@rm -rf $(BUILDDIR)/opensbi
	@rm -f $(BUILDDIR)/opensbi-*-stamp


$(BUILDDIR)/fsbl-prepare-checkout-stamp:
	@echo "$(COLOUR_GREEN)Checking out FSBL for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)
	@git clone -b $(FSBL_BRANCH) $(GIT_CLONE_OPTS) $(FSBL_REPO) $(BUILDDIR)/fsbl
	@cd $(BUILDDIR)/fsbl && git checkout $(FSBL_COMMIT)
	@touch $@

$(BUILDDIR)/fsbl-prepare-patch-stamp: $(BUILDDIR)/fsbl-prepare-checkout-stamp
	@echo "$(COLOUR_GREEN)Patching FSBL for $(BOARD)$(END_COLOUR)"
	$(foreach file,$(FSBL_PATCHES),cd $(BUILDDIR)/fsbl && git apply --ignore-whitespace $(file);)
	@python3 /builder/python/mmap_conv.py --type h $(BOARD_MEMMAP) $(BUILDDIR)/fsbl/plat/$(CHIP)/include/cvi_board_memmap.h
	@printf '\163\000\120\020\157\360\337\377' > $(BUILDDIR)/fsbl/blank.bin
	@touch $@

$(BUILDDIR)/fsbl-compile-stamp: $(BUILDDIR)/fsbl-prepare-patch-stamp $(BUILDDIR)/opensbi-compile-stamp $(BUILDDIR)/uboot-compile-stamp $(FSBL_RTOS_TARGET)
	@echo "$(COLOUR_GREEN)Building FSBL for $(BOARD)$(END_COLOUR)"
	@test -s $(FSBL_BLCP_2ND_PATH)
	@cd $(BUILDDIR)/fsbl && OD_CLK_SEL=y $(MAKE) -j$(NPROCS) ARCH=$(ARCH) CHIP_ARCH=$(CHIP) BOOT_CPU=$(BOOT_CPU) DDR_CFG=$(DDR_CFG) RTOS_ENABLE_FREERTOS=y BLCP_2ND_PATH=$(FSBL_BLCP_2ND_PATH) LOADER_2ND_PATH=$(BUILDDIR)/u-boot.bin \
    CROSS_COMPILE=$(CROSS_COMPILE)
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
	@cp $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION).deb $(OUTPUT_DIR)/
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
	@cp $(BUILDDIR)/package/cvi-pinmux-cv181x_$(PINMUX_VERSION)_$(DEB_ARCH).deb $(OUTPUT_DIR)/
	@touch $@


$(BUILDDIR)/image-prepare-stamp: 
	@echo "$(COLOUR_GREEN)Preparing Image for $(BOARD)$(END_COLOUR)"
	@-mkdir $(BUILDDIR)
	@rm -rf $(ROOTFS)
	@-rm $(addon-targets)
	@mkdir -p $(ROOTFS)

ifeq ($(USE_VENDOR_APT),0)
	@mmdebstrap -v --architectures=riscv64 --include="$(_PACKAGES)" sid "$(ROOTFS)" "deb http://deb.debian.org/debian/ sid main"
else
	@curl --fail --location --retry 5 --retry-delay 3 --retry-all-errors --connect-timeout 20 https://sophgo.my-ho.st/public-key.asc -o $(BUILDDIR)/public-key.asc
	@mmdebstrap -v --architectures=riscv64 --include="$(_PACKAGES)" sid "$(ROOTFS)" "deb http://deb.debian.org/debian/ sid main" "deb [signed-by=$(BUILDDIR)/public-key.asc] https://sophgo.my-ho.st:8443/ debian sophgo"

endif
	@touch $@

$(BUILDDIR)/image-addons-stamp: $(BUILDDIR)/image-prepare-stamp $(BUILDDIR)/fsbl-package-stamp $(BUILDDIR)/linux-compile-stamp $(MEDIA_IMAGE_TARGETS) $(LOCAL_PINMUX_TARGET) $(addon-targets)

ifeq ($(ENABLE_MEDIA_STACK),1)
	@[ -e $(ROOTFS)/usr/bin/devmem ] || ln -s busybox $(ROOTFS)/usr/bin/devmem

endif
	@echo "$(COLOUR_GREEN)Copying Deb files for installation on $(BOARD)$(END_COLOUR)"
	@cp $(BUILDDIR)/package/cvitek-fsbl-$(BOARD)-$(FSBLVERSION).deb $(ROOTFS)/tmp/install/

ifeq ($(ENABLE_MEDIA_STACK),1)
	@cp $(BUILDDIR)/package/cvitek-osdrv-*$(BOARD)*.deb $(ROOTFS)/tmp/install/
	@cp $(BUILDDIR)/package/cvitek-middleware-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb $(ROOTFS)/tmp/install/
	@cp $(BUILDDIR)/package/firmware-vcodec-$(CHIP)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb $(ROOTFS)/tmp/install/

endif
	@cp $(BUILDDIR)/linux-image-$(BOARD)-*.deb $(ROOTFS)/tmp/install/
	@cp $(BUILDDIR)/linux-headers-*.deb $(ROOTFS)/tmp/install/
	@cp $(BUILDDIR)/linux-libc-dev*.deb $(ROOTFS)/tmp/install/

ifeq ($(ENABLE_LOCAL_PINMUX),1)
	@cp $(BUILDDIR)/package/cvi-pinmux-cv181x_$(PINMUX_VERSION)_$(DEB_ARCH).deb $(ROOTFS)/tmp/install/

endif
	@touch $@


$(BUILDDIR)/image-customize-stamp: $(BUILDDIR)/image-addons-stamp $(BUILDDIR)/linux-compile-stamp $(BUILDDIR)/fsbl-package-stamp
	@echo "$(COLOUR_GREEN)Customizing Image for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(ROOTFS)/tmp/install/
	@echo $(BOARD) > $(ROOTFS)/tmp/install/hostname
	@echo $(STORAGE_TYPE) > $(ROOTFS)/tmp/install/storage
	@echo $(BOARD) > $(ROOTFS)/tmp/install/board
	@sh -ec 'qemu_bin=/usr/bin/qemu-riscv64-static; \
		if [ ! -x "$${qemu_bin}" ]; then qemu_bin=/usr/bin/qemu-riscv64; fi; \
		cp -v "$${qemu_bin}" $(ROOTFS)/tmp/install/qemu-riscv64-static'
	@cp -v /builder/setup_rootfs.sh $(ROOTFS)/tmp/install/

ifeq ($(USE_VENDOR_APT),1)
	@cp -v $(BUILDDIR)/public-key.asc $(ROOTFS)/tmp/install/

endif
	@chroot $(ROOTFS) /tmp/install/qemu-riscv64-static /bin/sh /tmp/install/setup_rootfs.sh
	@rm -rf $(ROOTFS)/tmp/install/
	@umount $(ROOTFS)/proc || true
	@umount $(ROOTFS)/sys || true
	@umount $(ROOTFS)/run || true
	@umount $(ROOTFS)/dev || true
	@touch $@

$(BUILDDIR)/image-compile-stamp: $(BUILDDIR)/image-customize-stamp
	@echo "$(COLOUR_GREEN)Compiling Image for $(BOARD)$(END_COLOUR)"
	@rm -rf /tmp/genimage/
	@cd $(BUILDDIR) && genimage --config /builder/genimage_$(STORAGE_TYPE).cfg --tmppath /tmp/genimage --rootpath $(ROOTFS)
	@rm -rf /tmp/genimage/
	@if [ "$(STORAGE_TYPE)" = "emmc" ]; then \
		python3 /builder/python/raw2cimg.py -v $(BUILDDIR)/images/sdcard.img $(BUILDDIR)/images $(BOARD_PARTITION_FILE); \
		mkdir -p /tmp/rom/; \
		cp $(BUILDDIR)/images/sdcard.img /tmp/rom/; \
		cp $(BOARD_PARTITION_FILE) /tmp/rom/partition_emmc.xml; \
		cp $(BUILDDIR)/fip.bin /tmp/rom/; \
		cd /tmp && zip $(BOARD)_$(STORAGE_TYPE).zip -r rom/; \
		cp /tmp/$(BOARD)_$(STORAGE_TYPE).zip $(OUTPUT_DIR)/; \
		echo "$(COLOUR_GREEN)Image for $(BOARD) is $(BOARD)_$(STORAGE_TYPE).zip$(END_COLOUR)"; \
	else \
		cp $(BUILDDIR)/images/sdcard.img $(OUTPUT_DIR)/$(BOARD)_$(STORAGE_TYPE).img; \
		echo "$(COLOUR_GREEN)Image for $(BOARD) is $(BOARD)_$(STORAGE_TYPE).img$(END_COLOUR)"; \
	fi 
	@touch $@

image: $(BUILDDIR)/image-compile-stamp

image-clean:
	@rm -rf $(ROOTFS)
	@rm -f $(BUILDDIR)/image-*-stamp $(addon-targets)
	@rm -f $(OUTPUT_DIR)/$(BOARD)_$(STORAGE_TYPE).img $(OUTPUT_DIR)/$(BOARD)_$(STORAGE_TYPE).zip

image-clean-customize:
	@rm -f $(BUILDDIR)/image-customize-stamp

clean: opensbi-clean uboot-clean linux-clean osdrv-clean middleware-clean fsbl-clean

.PHONY: image clean opensbi uboot linux osdrv middleware fsbl fsbl-clean uboot-clean linux-clean opensbi-clean
