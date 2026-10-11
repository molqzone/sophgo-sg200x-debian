$(BUILDDIR)/bootlog-stamp:
	@echo "$(COLOUR_GREEN)Installing bootlog$(END_COLOUR)"
	@install -d "$(ROOTFS)/usr/local/sbin" "$(ROOTFS)/etc/systemd/system"
	@install -m 0755 addons/bootlog/bootlogd "$(ROOTFS)/usr/local/sbin/bootlogd"
	@install -D -m 0644 addons/bootlog/bootlog-snap.service \
		"$(ROOTFS)/etc/systemd/system/bootlog-snap.service"
	@mkdir -p "$(ROOTFS)/tmp/install/"
	@grep -q "bootlog-snap" "$(ROOTFS)/tmp/install/systemd-enable" 2>/dev/null || \
		echo " bootlog-snap" >> "$(ROOTFS)/tmp/install/systemd-enable"
	@touch $@
