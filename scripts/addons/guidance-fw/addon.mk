GUIDANCE_FW_COMPONENT := $(COMPONENTS_ROOT)/guidance-fw
GUIDANCE_FW_FIRMWARE_DIR := $(GUIDANCE_FW_COMPONENT)/firmware
GUIDANCE_FW_FIRMWARE_ELF := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.elf
GUIDANCE_FW_FIRMWARE_BIN := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.bin

$(BUILDDIR)/guidance-fw-build-stamp:
	@echo "$(COLOUR_GREEN)Checking guidance-fw firmware for $(BOARD)$(END_COLOUR)"
	@test -s $(GUIDANCE_FW_FIRMWARE_ELF)
	@test -s $(GUIDANCE_FW_FIRMWARE_BIN)
	@cd $(GUIDANCE_FW_FIRMWARE_DIR) && sha256sum -c sha256sums.txt
	@touch $@

$(BUILDDIR)/guidance-fw-stamp: $(BUILDDIR)/guidance-fw-build-stamp
	@echo "$(COLOUR_GREEN)Installing guidance-fw firmware for $(BOARD)$(END_COLOUR)"
	@install -d "$(ROOTFS)/lib/firmware" "$(ROOTFS)/usr/bin"
	@install -m 0644 "$(GUIDANCE_FW_FIRMWARE_ELF)" \
		"$(ROOTFS)/lib/firmware/c906-mcu.elf"
	@install -m 0755 "$(GUIDANCE_FW_COMPONENT)/tools/guidance-cmd.py" \
		"$(GUIDANCE_FW_COMPONENT)/tools/rtos-mode" \
		"$(ROOTFS)/usr/bin/"
	@install -D -m 0644 \
		"$(GUIDANCE_FW_COMPONENT)/linux/systemd/load-systemko.service.d/20-guidance-fw.conf" \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf"
	@install -D -m 0644 \
		"$(GUIDANCE_FW_COMPONENT)/linux/systemd/c906-ready.service" \
		"$(ROOTFS)/etc/systemd/system/c906-ready.service"
	@mkdir -p "$(ROOTFS)/tmp/install/"
	@grep -qx "c906-ready" "$(ROOTFS)/tmp/install/systemd-enable" 2>/dev/null || \
		echo " c906-ready" >> "$(ROOTFS)/tmp/install/systemd-enable"
	@touch $@
