$(BUILDDIR)/guidance-autostart-stamp:
	@echo "$(COLOUR_GREEN)Installing guidance-autostart for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/etc/systemd/system/ /rootfs/usr/local/bin/
	@cp -a addons/guidance-autostart/guidance-core.service \
	         addons/guidance-autostart/guidance-core.timer \
	         addons/guidance-autostart/guidance-vision.service /rootfs/etc/systemd/system/
	@chmod 0644 /rootfs/etc/systemd/system/guidance-core.service \
	             /rootfs/etc/systemd/system/guidance-core.timer \
	             /rootfs/etc/systemd/system/guidance-vision.service
	@cp -a addons/guidance-autostart/loadmedia.sh /rootfs/usr/local/bin/
	@chmod 0755 /rootfs/usr/local/bin/loadmedia.sh
	@mkdir -p /rootfs/tmp/install/
	@echo " guidance-core.timer guidance-vision.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
