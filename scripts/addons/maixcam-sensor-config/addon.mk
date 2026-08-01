$(BUILDDIR)/maixcam-sensor-config-package-stamp:
	@echo "$(COLOUR_GREEN)Packaging MaixCAM sensor configuration$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)
	@cp -r /builder/deb/sensor-config/* $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/init.d/
	@cp -a addons/maixcam-sensor-config/sensor-config.sh $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/init.d/
	@chmod +x $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/init.d/sensor-config.sh
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/systemd/system/
	@cp -a addons/maixcam-sensor-config/sensor-config*.service $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/systemd/system/
	@chmod 0644 $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/systemd/system/sensor-config*.service
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/cfg/param/
	@rsync -avpPxH addons/maixcam-sensor-config/overlay/mnt/cfg/param/ $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/cfg/param/
	@rm -f $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/cfg/param/cvi_sdr_bin
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/data/
	@rsync -avpPxH addons/maixcam-sensor-config/overlay/mnt/data/ $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/data/
	@rm -f $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/data/sensor_cfg.ini
	@sed -i 's/Version: 1.0.0/Version: $(MIDDLEWAREVERSION)/' $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@sed -i 's/Package: sensor-config/Package: sensor-config-$(BOARD)/' $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@cd $(BUILDDIR)/package/ && dpkg-deb --build sensor-config-$(BOARD)-$(MIDDLEWAREVERSION) sensor-config-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb
	@cp $(BUILDDIR)/package/sensor-config-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb $(OUTPUT_DIR)/
	@mkdir -p $(ROOTFS)/tmp/install/
	@cp $(BUILDDIR)/package/sensor-config-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb $(ROOTFS)/tmp/install/
	@touch $@

$(BUILDDIR)/maixcam-sensor-config-stamp: $(BUILDDIR)/maixcam-sensor-config-package-stamp
	@echo "$(COLOUR_GREEN)Installing MaixCAM GC4653 sensor configuration$(END_COLOUR)"
	@mkdir -p $(ROOTFS)/boot $(ROOTFS)/mnt/cfg/param $(ROOTFS)/mnt/data
	@cp -p addons/maixcam-sensor-config/overlay/mnt/cfg/param/sipeed_gc4653_30fps_202403261356.bin $(ROOTFS)/mnt/cfg/param/cvi_sdr_bin
	@mkdir -p $(ROOTFS)/tmp/install
	@echo " sensor-config.service" >> $(ROOTFS)/tmp/install/systemd-enable
	@touch $@
