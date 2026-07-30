$(BUILDDIR)/usb-gadget-stamp:
	@echo "$(COLOUR_GREEN)Installing usb-gadget for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/usr/sbin /rootfs/usr/local/bin /rootfs/usr/local/sbin /rootfs/etc/systemd/system/ /rootfs/etc/network/interfaces.d/
	@cp -a addons/usb-gadget/run_usb.sh /rootfs/usr/sbin/
	@chmod +x /rootfs/usr/sbin/run_usb.sh
	@cp -a addons/usb-gadget/usb-mode-switch addons/usb-gadget/maixcam-ssh-authorize addons/usb-gadget/maixcam-usb-update /rootfs/usr/local/sbin/
	@chmod +x /rootfs/usr/local/sbin/usb-mode-switch /rootfs/usr/local/sbin/maixcam-ssh-authorize /rootfs/usr/local/sbin/maixcam-usb-update
	@cp -a addons/usb-gadget/usb-shell-wrapper /rootfs/usr/local/bin/
	@chmod +x /rootfs/usr/local/bin/usb-shell-wrapper
	@cp -a addons/usb-gadget/usb-gadget*.service /rootfs/etc/systemd/system/
	@cp -a addons/usb-gadget/usb-shell.service /rootfs/etc/systemd/system/
	@cp -a addons/usb-gadget/usb-mode-autorevert.service addons/usb-gadget/usb-mode-autorevert.timer addons/usb-gadget/maixcam-ssh-authorize.service /rootfs/etc/systemd/system/
	@chmod 0644 /rootfs/etc/systemd/system/usb-gadget*.service /rootfs/etc/systemd/system/usb-shell.service /rootfs/etc/systemd/system/usb-mode-autorevert.service /rootfs/etc/systemd/system/usb-mode-autorevert.timer /rootfs/etc/systemd/system/maixcam-ssh-authorize.service
	@cp -a addons/usb-gadget/usb0 /rootfs/etc/network/interfaces.d/
	@mkdir -p /rootfs/tmp/install/
	@if [ "$(BOARD)" = "licheervnano" ]; then \
		echo " usb-gadget-rndis.service usb-gadget-rndis-usb0.service maixcam-ssh-authorize.service" >> /rootfs/tmp/install/systemd-enable; \
	else \
		echo " usb-gadget-acm usb-shell.service" >> /rootfs/tmp/install/systemd-enable; \
	fi
	@touch $@
