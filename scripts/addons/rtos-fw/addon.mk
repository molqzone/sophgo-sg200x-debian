RTOS_FW_COMPONENT := $(COMPONENTS_ROOT)/rtos-fw
RTOS_FW_RELEASE_IN := $(RTOS_FW_COMPONENT)/linux/systemd/rtos-release.service.in
GUIDANCE_FW_LOAD_SYSTEMKO_NOOP := $(RTOS_FW_COMPONENT)/linux/systemd/load-systemko-none.conf
GUIDANCE_FW_FIRMWARE_DIR := $(RTOS_FW_COMPONENT)/firmware
GUIDANCE_FW_FIRMWARE_ELF := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.elf
GUIDANCE_FW_FIRMWARE_BIN := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.bin
# 由 scripts/Makefile 的启动策略决定；单独使用本文件时退回默认值。
# Set by scripts/Makefile's boot policy; a default keeps this file usable on its own.
# load 与 attach 是策略的取值，不是 rtos-mode 的子命令。映射放在这里，只有一个地方。
# `load` and `attach` are policy values, not rtos-mode subcommands; the mapping lives here
# and nowhere else.
GUIDANCE_FW_RPROC_COMMAND ?= $(if $(filter fsbl,$(GUIDANCE_FW_LOADER)),attach,remoteproc)

$(BUILDDIR)/rtos-fw-build-stamp:
	@echo "$(COLOUR_GREEN)Checking rtos-fw firmware for $(BOARD)$(END_COLOUR)"
	@test -s $(GUIDANCE_FW_FIRMWARE_ELF)
	@test -s $(GUIDANCE_FW_FIRMWARE_BIN)
	@test -x $(RTOS_FW_COMPONENT)/tools/cop
	@cd $(GUIDANCE_FW_FIRMWARE_DIR) && sha256sum -c sha256sums.txt
	@touch $@

$(BUILDDIR)/rtos-fw-stamp: $(BUILDDIR)/rtos-fw-build-stamp
	@echo "$(COLOUR_GREEN)Installing rtos-fw firmware for $(BOARD)$(END_COLOUR)"
	@install -d "$(ROOTFS)/lib/firmware" "$(ROOTFS)/usr/bin" \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d"
	@install -m 0644 "$(GUIDANCE_FW_FIRMWARE_ELF)" \
		"$(ROOTFS)/lib/firmware/c906-mcu.elf"
	@install -m 0755 "$(RTOS_FW_COMPONENT)/tools/rtos-mode" \
		"$(RTOS_FW_COMPONENT)/tools/cop" \
		"$(ROOTFS)/usr/bin/"
	# 唯一的放核单元：after multi-user，参数由 GUIDANCE_FW_LOADER 映射；
	# 厂商 load-systemko 由 50- 空操作，避免第二次放核。
	# The single release unit: after multi-user, with the rtos-mode argument mapped
	# from GUIDANCE_FW_LOADER.  The vendor load-systemko is neutralised by 50-.
	@sed 's|@GUIDANCE_FW_RPROC_COMMAND@|$(GUIDANCE_FW_RPROC_COMMAND)|' \
		"$(RTOS_FW_RELEASE_IN)" > \
		"$(ROOTFS)/etc/systemd/system/rtos-release.service"
	@grep -q "^ExecStart=.*rtos-mode $(GUIDANCE_FW_RPROC_COMMAND)$$" \
		"$(ROOTFS)/etc/systemd/system/rtos-release.service"
	@awk -v c='$(GUIDANCE_FW_RPROC_COMMAND)' '$$0 == "\t" c ")" { found = 1 } \
		END { exit !found }' "$(RTOS_FW_COMPONENT)/tools/rtos-mode" || \
		{ echo "rtos-mode has no '$(GUIDANCE_FW_RPROC_COMMAND)' subcommand" >&2; exit 1; }
	@install -D -m 0644 "$(GUIDANCE_FW_LOAD_SYSTEMKO_NOOP)" \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/50-rtos-fw.conf"
	@install -D -m 0644 \
		"$(RTOS_FW_COMPONENT)/linux/modprobe.d/rtos-uio.conf" \
		"$(ROOTFS)/etc/modprobe.d/rtos-uio.conf"
	@install -D -m 0644 \
		"$(RTOS_FW_COMPONENT)/linux/modules-load.d/rtos-uio.conf" \
		"$(ROOTFS)/etc/modules-load.d/rtos-uio.conf"
	@install -D -m 0644 \
		"$(RTOS_FW_COMPONENT)/linux/udev/60-rtos-uio.rules" \
		"$(ROOTFS)/etc/udev/rules.d/60-rtos-uio.rules"
	@mkdir -p "$(ROOTFS)/tmp/install"
	@echo " rtos-release.service" >> "$(ROOTFS)/tmp/install/systemd-enable"
	@touch $@
