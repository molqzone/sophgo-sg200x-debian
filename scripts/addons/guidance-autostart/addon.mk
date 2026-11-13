# Boot the C906L and the vision app from a power cycle, and ship every binary the board
# needs so a reflash reproduces the working system with no manual injection.
#
# One release path only, and it is triggered by a condition rather than a timer:
#   guidance-release.service, After=multi-user.target, WantedBy=graphical.target
#     -> systemd-run --no-block rtos-mode remoteproc, then waits for `running`
#   guidance-vision.service, After=guidance-release.service
#     -> loads the media modules and starts the camera
# `load-systemko` is neutralised with a drop-in (see load-systemko-none.conf) so the vendor
# firmware is never released as well, and `rtos-mode`'s stop_media_chain therefore always
# runs on an idle camera chain.
$(BUILDDIR)/guidance-autostart-stamp: addons/guidance-autostart/guidance-release.service \
                                      addons/guidance-autostart/guidance-vision.service \
                                      addons/guidance-autostart/load-systemko-none.conf \
                                      addons/guidance-autostart/loadmedia.sh \
                                      addons/guidance-autostart/files/c906-mcu.elf \
                                      addons/guidance-autostart/files/guidance_vision_app \
                                      addons/guidance-autostart/files/sensor_cfg.ini \
                                      addons/guidance-autostart/files/libsns_sc035hgs.so \
                                      addons/guidance-autostart/files/libsns_full.so
	@echo "$(COLOUR_GREEN)Installing guidance-autostart for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/etc/systemd/system/ /rootfs/usr/local/bin/ /rootfs/lib/firmware/
	@cp -a addons/guidance-autostart/guidance-release.service \
	         addons/guidance-autostart/guidance-vision.service /rootfs/etc/systemd/system/
	@chmod 0644 /rootfs/etc/systemd/system/guidance-release.service \
	             /rootfs/etc/systemd/system/guidance-vision.service
	@install -m0755 addons/guidance-autostart/loadmedia.sh /rootfs/usr/local/bin/loadmedia.sh
	@install -m0755 addons/guidance-autostart/files/guidance_vision_app /rootfs/usr/local/bin/guidance_vision_app
	@install -m0644 addons/guidance-autostart/files/c906-mcu.elf /rootfs/lib/firmware/c906-mcu.elf
	@install -D -m0644 addons/guidance-autostart/files/sensor_cfg.ini /rootfs/mnt/data/sensor_cfg.ini
	# 厂商分区缺 SC035HGS 的传感器库：缺了 VI 初始化会以 `fail to find pstSnsObj` 失败。
	# The vendor partition lacks the SC035HGS sensor libraries; without them VI init fails
	# with "fail to find pstSnsObj" and the camera never delivers a frame.
	@install -D -m0644 addons/guidance-autostart/files/libsns_sc035hgs.so /rootfs/mnt/system/usr/lib/libsns_sc035hgs.so
	@install -D -m0644 addons/guidance-autostart/files/libsns_full.so /rootfs/mnt/system/usr/lib/libsns_full.so
	# 厂商放核入口的空操作 drop-in（50- 覆盖 guidance-fw 的 20-）：保留单元可 enable，
	# 但不再放厂商固件。
	# No-op drop-in for the vendor release hook (50- overrides guidance-fw's 20-): the unit
	# stays enable-able but releases nothing.
	@install -D -m0644 addons/guidance-autostart/load-systemko-none.conf \
	        /rootfs/etc/systemd/system/load-systemko.service.d/50-guidance-autostart.conf
	@mkdir -p /rootfs/tmp/install/
	@echo " guidance-release.service guidance-vision.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
