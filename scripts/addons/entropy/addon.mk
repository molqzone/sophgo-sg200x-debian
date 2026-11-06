$(BUILDDIR)/entropy-stamp:
	@echo "$(COLOUR_GREEN)Installing entropy seeding for $(BOARD)$(END_COLOUR)"
	@install -d "$(ROOTFS)/usr/local/sbin" "$(ROOTFS)/etc/systemd/system"
	@install -m 0755 addons/entropy/seed-entropy "$(ROOTFS)/usr/local/sbin/seed-entropy"
	@install -D -m 0644 addons/entropy/seed-entropy.service \
		"$(ROOTFS)/etc/systemd/system/seed-entropy.service"
	@install -d "$(ROOTFS)/tmp/install"
	@grep -q "seed-entropy" "$(ROOTFS)/tmp/install/systemd-enable" 2>/dev/null || \
		echo " seed-entropy" >> "$(ROOTFS)/tmp/install/systemd-enable"
	@touch $@
