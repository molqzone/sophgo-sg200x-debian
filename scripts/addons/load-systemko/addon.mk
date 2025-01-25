$(BUILDDIR)/load-systemko-stamp:
	@echo "$(COLOUR_GREEN)Installing load-systemko for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/etc/init.d/
	@cp -a addons/load-systemko/load-systemko.sh /rootfs/etc/init.d/
	@chmod +x /rootfs/etc/init.d/load-systemko.sh
	@cp -a addons/load-systemko/load-systemko*.service /rootfs/etc/systemd/system/
	@mkdir -p /rootfs/tmp/install/
	@echo " load-systemko" >> /rootfs/tmp/install/systemd-enable
	@touch $@
