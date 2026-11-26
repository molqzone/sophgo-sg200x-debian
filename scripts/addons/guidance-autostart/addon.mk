# Vision application and camera setup.  This addon installs no firmware, no
# release unit and no load-systemko override: rtos-fw owns all of those.
# It only starts the vision app after rtos-release has released the core.
$(BUILDDIR)/guidance-autostart-stamp: addons/guidance-autostart/guidance-vision.service \
                                      addons/guidance-autostart/loadmedia.sh \
                                      addons/guidance-autostart/files/guidance_vision_app \
                                      addons/guidance-autostart/files/sensor_cfg.ini \
                                      addons/guidance-autostart/files/libsns_sc035hgs.so \
                                      addons/guidance-autostart/files/libsns_full.so
	@echo "$(COLOUR_GREEN)Installing guidance-autostart for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/etc/systemd/system/ /rootfs/usr/local/bin/ /rootfs/lib/firmware/
	@install -D -m0644 addons/guidance-autostart/guidance-vision.service \
	        /rootfs/etc/systemd/system/guidance-vision.service
	@install -m0755 addons/guidance-autostart/loadmedia.sh /rootfs/usr/local/bin/loadmedia.sh
	@install -m0755 addons/guidance-autostart/files/guidance_vision_app /rootfs/usr/local/bin/guidance_vision_app
	@install -D -m0644 addons/guidance-autostart/files/sensor_cfg.ini /rootfs/mnt/data/sensor_cfg.ini
	# 开机时 `maixcam-sensor-config` 会用 `SENSOR_CONFIG_DEFAULT`（就是下面这个 `.SC035HGS`
	# 文件）**覆盖** `/mnt/data/sensor_cfg.ini`，所以我们的 profile 必须写在这个路径上，
	# 否则每次上电都会被换回镜像自带的档位，VI 按 120 fps 配置、传感器实际 30 fps，
	# `CVI_VPSS_SetChnAttr` 直接失败，相机起不来。
	# At boot `maixcam-sensor-config` **overwrites** `/mnt/data/sensor_cfg.ini` from
	# `SENSOR_CONFIG_DEFAULT` (this very `.SC035HGS` file), so our profile has to live at
	# that path; otherwise every power cycle restores the image's own profile, VI asks for
	# 120 fps while the sensor runs 30, `CVI_VPSS_SetChnAttr` fails and the camera never
	# comes up.
	@install -D -m0644 addons/guidance-autostart/files/sensor_cfg.ini /rootfs/mnt/data/sensor_cfg.ini.SC035HGS
	# 厂商分区缺 SC035HGS 的传感器库：缺了 VI 初始化会以 `fail to find pstSnsObj` 失败。
	# The vendor partition lacks the SC035HGS sensor libraries; without them VI init fails
	# with "fail to find pstSnsObj" and the camera never delivers a frame.
	@install -D -m0644 addons/guidance-autostart/files/libsns_sc035hgs.so /rootfs/mnt/system/usr/lib/libsns_sc035hgs.so
	@install -D -m0644 addons/guidance-autostart/files/libsns_full.so /rootfs/mnt/system/usr/lib/libsns_full.so
	# 放核、固件和 load-systemko 空操作都归 rtos-fw；这里只起视觉应用。
	# Release, firmware and the load-systemko no-op belong to rtos-fw; this addon
	# only starts the vision application.
	@mkdir -p /rootfs/tmp/install/
	@echo " guidance-vision.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
