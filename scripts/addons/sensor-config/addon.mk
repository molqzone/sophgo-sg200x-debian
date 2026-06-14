$(BUILDDIR)/sensor-config-package-stamp:
	@echo "$(COLOUR_GREEN)Packaging sensor-config for $(BOARD)$(END_COLOUR)"
	@mkdir -p $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)
	@cp -r /builder/deb/sensor-config/* $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/init.d/
	@cp -a addons/sensor-config/sensor-config.sh $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/init.d/
	@chmod +x $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/init.d/sensor-config.sh
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/systemd/system/
	@cp -a addons/sensor-config/sensor-config*.service $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/systemd/system/
	@chmod 0644 $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/etc/systemd/system/sensor-config*.service
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/cfg/param/
	@rsync -avpPxH addons/sensor-config/overlay/mnt/cfg/param/ $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/cfg/param/
	@rm -f $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/cfg/param/cvi_sdr_bin
	@mkdir -pv $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/data/
	@rsync -avpPxH addons/sensor-config/overlay/mnt/data/ $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/data/
	@rm -f $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/mnt/data/sensor_cfg.ini
	@sed -i 's/Version: 1.0.0/Version: $(MIDDLEWAREVERSION)/' $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@sed -i 's/Package: sensor-config/Package: sensor-config-$(BOARD)/' $(BUILDDIR)/package/sensor-config-$(BOARD)-$(MIDDLEWAREVERSION)/DEBIAN/control
	@cd $(BUILDDIR)/package/ && dpkg-deb --build sensor-config-$(BOARD)-$(MIDDLEWAREVERSION) sensor-config-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb
	@cp $(BUILDDIR)/package/sensor-config-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb /output/
	@mkdir -p /rootfs/tmp/install/
	@cp $(BUILDDIR)/package/sensor-config-$(BOARD)_$(MIDDLEWAREVERSION)_$(DEB_ARCH).deb /rootfs/tmp/install/
	@touch $@

$(BUILDDIR)/sensor-config-stamp: $(BUILDDIR)/sensor-config-package-stamp
	@echo "$(COLOUR_GREEN)Installing sensor-config for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/boot/
	@[ "$(BOARD)" = "licheervnano" ] || touch /rootfs/boot/epsilon
	@mkdir -pv /rootfs/mnt/cfg/param/
	@mkdir -pv /rootfs/mnt/data/
	@if [ "$(BOARD)" = "licheervnano" ]; then \
		cp -p addons/sensor-config/overlay/mnt/cfg/param/sipeed_gc4653_30fps_202403261356.bin /rootfs/mnt/cfg/param/cvi_sdr_bin ; \
	elif [ "$(BOARD)" = "duo256" ]; then \
		cp -p addons/sensor-config/overlay/mnt/cfg/param/cvi_sdr_bin_GC2083 /rootfs/mnt/cfg/param/cvi_sdr_bin && \
		cp -p addons/sensor-config/overlay/mnt/data/sensor_cfg_GC2083.ini /rootfs/mnt/data/sensor_cfg.ini ; \
	fi
	@mkdir -p /rootfs/tmp/install/
	@echo " sensor-config.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
