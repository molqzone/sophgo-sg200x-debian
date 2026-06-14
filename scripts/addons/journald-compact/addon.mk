$(BUILDDIR)/journald-compact-stamp:
	@echo "$(COLOUR_GREEN)Installing compact journald defaults for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/etc/systemd/journald.conf.d/ /rootfs/etc/systemd/system/systemd-journald.service.d/
	@cp -a addons/journald-compact/overlay/etc/systemd/journald.conf.d/*.conf /rootfs/etc/systemd/journald.conf.d/
	@cp -a addons/journald-compact/overlay/etc/systemd/system/systemd-journald.service.d/*.conf /rootfs/etc/systemd/system/systemd-journald.service.d/
	@chmod 0644 /rootfs/etc/systemd/journald.conf.d/*.conf /rootfs/etc/systemd/system/systemd-journald.service.d/*.conf
	@touch $@
