$(BUILDDIR)/network-services-stamp:
	@echo "$(COLOUR_GREEN)Installing network-services helper for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/usr/bin/ /rootfs/etc/systemd/system/
	@cp -a addons/network-services/network-services /rootfs/usr/bin/
	@chmod +x /rootfs/usr/bin/network-services
	@cp -a addons/network-services/network-services-disable.service /rootfs/etc/systemd/system/
	@chmod 0644 /rootfs/etc/systemd/system/network-services-disable.service
	@mkdir -p /rootfs/tmp/install/
	@echo " network-services-disable.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
