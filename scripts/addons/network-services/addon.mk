$(BUILDDIR)/network-services-stamp:
	@echo "$(COLOUR_GREEN)Installing network-services helper for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/usr/bin/
	@cp -a addons/network-services/network-services /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/network-services
	@touch $@
