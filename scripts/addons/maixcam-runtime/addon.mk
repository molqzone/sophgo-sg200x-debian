$(BUILDDIR)/maixcam-runtime-stamp:
	@echo "$(COLOUR_GREEN)Installing MaixCAM rootfs policy$(END_COLOUR)"
	@mkdir -p $(ROOTFS)/tmp/install/rootfs-hooks
	@touch $(ROOTFS)/tmp/install/apt-allow-downgrades
	@cp -a addons/maixcam-runtime/rootfs-hook.sh $(ROOTFS)/tmp/install/rootfs-hooks/90-maixcam.sh
	@chmod 0755 $(ROOTFS)/tmp/install/rootfs-hooks/90-maixcam.sh
	@touch $@
