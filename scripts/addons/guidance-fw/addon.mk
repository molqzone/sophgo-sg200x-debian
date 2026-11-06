GUIDANCE_FW_COMPONENT := $(COMPONENTS_ROOT)/guidance-fw
GUIDANCE_FW_FIRMWARE_DIR := $(GUIDANCE_FW_COMPONENT)/firmware
GUIDANCE_FW_FIRMWARE_ELF := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.elf
GUIDANCE_FW_FIRMWARE_BIN := $(GUIDANCE_FW_FIRMWARE_DIR)/c906-mcu.bin
GUIDANCE_FW_CONTRACT := $(GUIDANCE_FW_FIRMWARE_DIR)/guidance_contract.json
GUIDANCE_FW_DROPIN_IN := $(GUIDANCE_FW_COMPONENT)/linux/systemd/load-systemko.service.d/20-guidance-fw.conf.in
# drop-in 里 rtos-mode 的参数由 LOADER 推导，不是独立旋钮：remoteproc 意味着 Linux 写
# 固件并起核（`rtos-mode remoteproc`），fsbl 意味着核已经被 FSBL 放行（`rtos-mode
# attach`）。写成独立变量就能配上 rtos-mode 根本没有的子命令——那次配出来的 `load` 让
# 单元以 usage 退出 2，核从未被放行。这两个取值是 rtos-mode 的 case 分支，下面安装时
# 还会对着工具本身核一遍。
# The rtos-mode argument is derived from the loader, not configured separately:
# remoteproc means Linux writes the firmware and starts the core, fsbl means the FSBL has
# already released it.  As its own variable it could hold a subcommand rtos-mode does not
# have -- `load` exited 2 with the usage text and the core was never released.  Both
# values are case arms in rtos-mode, which the install step re-checks against the tool.
GUIDANCE_FW_RPROC_COMMAND ?= $(if $(filter fsbl,$(GUIDANCE_FW_LOADER)),attach,remoteproc)

$(BUILDDIR)/guidance-fw-build-stamp:
	@echo "$(COLOUR_GREEN)Checking guidance-fw firmware for $(BOARD)$(END_COLOUR)"
	@test -s $(GUIDANCE_FW_FIRMWARE_ELF)
	@test -s $(GUIDANCE_FW_FIRMWARE_BIN)
	@test -s $(GUIDANCE_FW_CONTRACT)
	@cd $(GUIDANCE_FW_FIRMWARE_DIR) && sha256sum -c sha256sums.txt
	@touch $@

# 安装步骤的输入就是这些文件：它们改了，stamp 必须失效，否则增量构建会留下上一版的
# drop-in（`make image` 每次都重建 rootfs，但 `image-customize` 不会）。
# The install step's inputs are these files: when one changes the stamp must fall out,
# or an incremental build keeps the previous drop-in (`make image` rebuilds the rootfs
# every time, but `image-customize` does not).
$(BUILDDIR)/guidance-fw-stamp: $(BUILDDIR)/guidance-fw-build-stamp \
		$(GUIDANCE_FW_DROPIN_IN) \
		$(GUIDANCE_FW_COMPONENT)/tools/rtos-mode \
		$(GUIDANCE_FW_COMPONENT)/tools/cop \
		$(GUIDANCE_FW_COMPONENT)/linux/modprobe.d/guidance-uio.conf \
		$(GUIDANCE_FW_COMPONENT)/linux/modules-load.d/guidance-uio.conf \
		$(lastword $(MAKEFILE_LIST))
	@echo "$(COLOUR_GREEN)Installing guidance-fw firmware for $(BOARD) (rtos-mode $(GUIDANCE_FW_RPROC_COMMAND))$(END_COLOUR)"
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
	@sed -i 's/@GUIDANCE_FW_RPROC_COMMAND@/$(GUIDANCE_FW_RPROC_COMMAND)/' \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf"
	@grep -q "^ExecStart=/usr/bin/rtos-mode $(GUIDANCE_FW_RPROC_COMMAND)$$" \
		"$(ROOTFS)/etc/systemd/system/load-systemko.service.d/20-guidance-fw.conf"
	@awk -v c='$(GUIDANCE_FW_RPROC_COMMAND)' '$$0 == "\t" c ")" { found = 1 } \
		END { exit !found }' "$(GUIDANCE_FW_COMPONENT)/tools/rtos-mode" || \
		{ echo "rtos-mode has no '$(GUIDANCE_FW_RPROC_COMMAND)' subcommand" >&2; exit 1; }
	@install -D -m 0644 \
		"$(GUIDANCE_FW_COMPONENT)/linux/modprobe.d/guidance-uio.conf" \
		"$(ROOTFS)/etc/modprobe.d/guidance-uio.conf"
	@install -D -m 0644 \
		"$(GUIDANCE_FW_COMPONENT)/linux/modules-load.d/guidance-uio.conf" \
		"$(ROOTFS)/etc/modules-load.d/guidance-uio.conf"
	@touch $@
