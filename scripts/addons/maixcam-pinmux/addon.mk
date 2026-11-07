$(BUILDDIR)/maixcam-pinmux-stamp:
	@echo "$(COLOUR_GREEN)Installing the late pinmux re-assert for $(BOARD)$(END_COLOUR)"
	@install -d "$(ROOTFS)/usr/local/sbin" "$(ROOTFS)/etc/systemd/system"
	@install -m 0755 addons/maixcam-pinmux/maixcam-late-pinmux "$(ROOTFS)/usr/local/sbin/maixcam-late-pinmux"
	@install -D -m 0644 addons/maixcam-pinmux/maixcam-late-pinmux.service \
		"$(ROOTFS)/etc/systemd/system/maixcam-late-pinmux.service"
	@install -d "$(ROOTFS)/tmp/install"
	@grep -q "maixcam-late-pinmux" "$(ROOTFS)/tmp/install/systemd-enable" 2>/dev/null || \
		echo " maixcam-late-pinmux" >> "$(ROOTFS)/tmp/install/systemd-enable"
	@touch $@
