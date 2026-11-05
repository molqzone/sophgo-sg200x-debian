GUIDANCE_FW_COMPONENT := $(COMPONENTS_ROOT)/guidance-fw
GUIDANCE_FW_FIRMWARE_DIR := $(GUIDANCE_FW_COMPONENT)/firmware
GUIDANCE_FW_FIRMWARE_ELF := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.elf
GUIDANCE_FW_FIRMWARE_BIN := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.bin
GUIDANCE_FW_CONTRACT := $(GUIDANCE_FW_FIRMWARE_DIR)/guidance_contract.json
GUIDANCE_FW_DROPIN_IN := $(GUIDANCE_FW_COMPONENT)/linux/systemd/load-systemko.service.d/20-guidance-fw.conf.in
# 由 scripts/Makefile 的启动策略决定；单独使用本文件时退回默认值。
# Set by scripts/Makefile's boot policy; a default keeps this file usable on its own.
GUIDANCE_FW_RPROC_MODE ?= load

$(BUILDDIR)/guidance-fw-build-stamp:
	@echo "$(COLOUR_GREEN)Checking guidance-fw firmware for $(BOARD)$(END_COLOUR)"
	@test -s $(GUIDANCE_FW_FIRMWARE_ELF)
	@test -s $(GUIDANCE_FW_FIRMWARE_BIN)
	@test -s $(GUIDANCE_FW_CONTRACT)
	@cd $(GUIDANCE_FW_FIRMWARE_DIR) && sha256sum -c sha256sums.txt
	@touch $@

$(BUILDDIR)/guidance-fw-stamp: $(BUILDDIR)/guidance-fw-build-stamp
	@echo "$(COLOUR_GREEN)Installing guidance-fw firmware for $(BOARD)$(END_COLOUR)"
	@install -d "$(ROOTFS)/lib/firmware" "$(ROOTFS)/usr/bin"
	@install -m 0644 "$(GUIDANCE_FW_FIRMWARE_ELF)" \
		"$(ROOTFS)/lib/firmware/c906-mcu.elf"
	@install -m 0755 "$(GUIDANCE_FW_COMPONENT)/tools/rtos-mode" \
		"$(GUIDANCE_FW_COMPONENT)/tools/cop" \
		"$(ROOTFS)/usr/bin/"
	@install -m 0644 "$(GUIDANCE_FW_CONTRACT)" \
		"$(ROOTFS)/lib/firmware/guidance_contract.json"
	@install -D -m 0644 "$(GUIDANCE_FW_DROPIN_IN)" \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf"
	@sed -i 's/@GUIDANCE_FW_RPROC_MODE@/$(GUIDANCE_FW_RPROC_MODE)/' \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf"
	@grep -q "^ExecStart=/usr/bin/rtos-mode $(GUIDANCE_FW_RPROC_MODE)$$" \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf"
	@install -D -m 0644 \
		"$(GUIDANCE_FW_COMPONENT)/linux/modprobe.d/guidance-uio.conf" \
		"$(ROOTFS)/etc/modprobe.d/guidance-uio.conf"
	@install -D -m 0644 \
		"$(GUIDANCE_FW_COMPONENT)/linux/modules-load.d/guidance-uio.conf" \
		"$(ROOTFS)/etc/modules-load.d/guidance-uio.conf"
	@touch $@
