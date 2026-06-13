$(BUILDDIR)/journald-compact-stamp:
	@echo "$(COLOUR_GREEN)Installing compact journald defaults for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/etc/systemd/journald.conf.d/
	@cp -a addons/journald-compact/overlay/etc/systemd/journald.conf.d/*.conf /rootfs/etc/systemd/journald.conf.d/
	@chmod 0644 /rootfs/etc/systemd/journald.conf.d/*.conf
	@touch $@
