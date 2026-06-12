$(BUILDDIR)/time-sync-stamp:
	@echo "$(COLOUR_GREEN)Installing time-sync helper for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/usr/bin/
	@cp -a addons/time-sync/time-sync /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/time-sync
	@touch $@
