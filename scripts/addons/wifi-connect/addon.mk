$(BUILDDIR)/wifi-connect-stamp:
	@echo "$(COLOUR_GREEN)Installing wifi-connect helper for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/usr/bin/
	@cp -a addons/wifi-connect/wifi-connect /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/wifi-connect
	@touch $@
