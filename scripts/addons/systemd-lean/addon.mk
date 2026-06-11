$(BUILDDIR)/systemd-lean-stamp:
	@echo "$(COLOUR_GREEN)Installing lean systemd defaults for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/etc/systemd/system.conf.d/
	@cp -a addons/systemd-lean/overlay/etc/systemd/system.conf.d/*.conf /rootfs/etc/systemd/system.conf.d/
	@touch $@
