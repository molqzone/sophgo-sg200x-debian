$(BUILDDIR)/maixcam-usb-stamp:
	@echo "$(COLOUR_GREEN)Installing MaixCAM USB services$(END_COLOUR)"
	@mkdir -p $(ROOTFS)/usr/sbin $(ROOTFS)/usr/local/bin $(ROOTFS)/usr/local/sbin
	@mkdir -p $(ROOTFS)/etc/systemd/system $(ROOTFS)/etc/network/interfaces.d
	@cp -a addons/maixcam-usb/run_usb.sh $(ROOTFS)/usr/sbin/
	@chmod +x $(ROOTFS)/usr/sbin/run_usb.sh
	@cp -a addons/maixcam-usb/usb-mode-switch addons/maixcam-usb/maixcam-ssh-authorize addons/maixcam-usb/maixcam-usb-update $(ROOTFS)/usr/local/sbin/
	@chmod +x $(ROOTFS)/usr/local/sbin/usb-mode-switch $(ROOTFS)/usr/local/sbin/maixcam-ssh-authorize $(ROOTFS)/usr/local/sbin/maixcam-usb-update
	@cp -a addons/maixcam-usb/usb0-ip.sh $(ROOTFS)/usr/local/sbin/
	@chmod +x $(ROOTFS)/usr/local/sbin/usb0-ip.sh
	@cp -a addons/maixcam-usb/usb-shell-wrapper $(ROOTFS)/usr/local/bin/
	@chmod +x $(ROOTFS)/usr/local/bin/usb-shell-wrapper
	@cp -a addons/maixcam-usb/usb-gadget*.service addons/maixcam-usb/usb-shell.service $(ROOTFS)/etc/systemd/system/
	@cp -a addons/maixcam-usb/maixcam-ssh-authorize.service $(ROOTFS)/etc/systemd/system/
	@chmod 0644 $(ROOTFS)/etc/systemd/system/usb-gadget*.service $(ROOTFS)/etc/systemd/system/usb-shell.service
	@chmod 0644 $(ROOTFS)/etc/systemd/system/maixcam-ssh-authorize.service
	@# Drop the 180 s ACM fallback and the old RNDIS units.  They are not part
	@# of this image; if an offline/cached rootfs still carries them, their
	@# unit files and enable links must not leak into the image and flip the
	@# management gadget away from NCM.
	@rm -f $(ROOTFS)/etc/systemd/system/usb-mode-autorevert.service \
		$(ROOTFS)/etc/systemd/system/usb-mode-autorevert.timer \
		$(ROOTFS)/etc/systemd/system/usb-gadget-rndis.service \
		$(ROOTFS)/etc/systemd/system/usb-gadget-rndis-usb0.service
	@rm -f $(ROOTFS)/etc/systemd/system/timers.target.wants/usb-mode-autorevert.timer \
		$(ROOTFS)/etc/systemd/system/multi-user.target.wants/usb-gadget-acm.service \
		$(ROOTFS)/etc/systemd/system/multi-user.target.wants/usb-shell.service \
		$(ROOTFS)/etc/systemd/system/multi-user.target.wants/usb-gadget-rndis.service \
		$(ROOTFS)/etc/systemd/system/sys-subsystem-net-devices-usb0.device.wants/usb-gadget-rndis-usb0.service
	@cp -a addons/maixcam-usb/usb0 $(ROOTFS)/etc/network/interfaces.d/
	@mkdir -p $(ROOTFS)/tmp/install
	@echo " usb-gadget-ncm.service usb-gadget-ncm-usb0.service maixcam-ssh-authorize.service" >> $(ROOTFS)/tmp/install/systemd-enable
	@touch $@
