$(BUILDDIR)/maixcam-usb-stamp:
	@echo "$(COLOUR_GREEN)Installing MaixCAM USB services$(END_COLOUR)"
	@mkdir -p $(ROOTFS)/usr/sbin $(ROOTFS)/usr/local/bin $(ROOTFS)/usr/local/sbin
	@mkdir -p $(ROOTFS)/etc/systemd/system $(ROOTFS)/etc/network/interfaces.d
	@cp -a addons/maixcam-usb/run_usb.sh $(ROOTFS)/usr/sbin/
	@chmod +x $(ROOTFS)/usr/sbin/run_usb.sh
	@cp -a addons/maixcam-usb/usb-mode-switch addons/maixcam-usb/maixcam-ssh-authorize addons/maixcam-usb/maixcam-usb-update $(ROOTFS)/usr/local/sbin/
	@chmod +x $(ROOTFS)/usr/local/sbin/usb-mode-switch $(ROOTFS)/usr/local/sbin/maixcam-ssh-authorize $(ROOTFS)/usr/local/sbin/maixcam-usb-update
	@cp -a addons/maixcam-usb/usb-shell-wrapper $(ROOTFS)/usr/local/bin/
	@chmod +x $(ROOTFS)/usr/local/bin/usb-shell-wrapper
	@cp -a addons/maixcam-usb/usb-gadget*.service addons/maixcam-usb/usb-shell.service $(ROOTFS)/etc/systemd/system/
	@cp -a addons/maixcam-usb/usb-mode-autorevert.service addons/maixcam-usb/usb-mode-autorevert.timer addons/maixcam-usb/maixcam-ssh-authorize.service $(ROOTFS)/etc/systemd/system/
	@chmod 0644 $(ROOTFS)/etc/systemd/system/usb-gadget*.service $(ROOTFS)/etc/systemd/system/usb-shell.service
	@chmod 0644 $(ROOTFS)/etc/systemd/system/usb-mode-autorevert.service $(ROOTFS)/etc/systemd/system/usb-mode-autorevert.timer $(ROOTFS)/etc/systemd/system/maixcam-ssh-authorize.service
	@cp -a addons/maixcam-usb/usb0 $(ROOTFS)/etc/network/interfaces.d/
	@mkdir -p $(ROOTFS)/tmp/install
	@echo " usb-gadget-rndis.service usb-gadget-rndis-usb0.service maixcam-ssh-authorize.service" >> $(ROOTFS)/tmp/install/systemd-enable
	@touch $@
