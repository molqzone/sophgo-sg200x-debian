# Vision application and camera setup.  The C906L itself is released by
# guidance-fw's load-systemko drop-in; this addon must not install a second ELF,
# a second release unit, or a competing load-systemko drop-in.
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
	# 放核归 guidance-fw 的 load-systemko drop-in；这里只起视觉应用。
	# Release belongs to guidance-fw's load-systemko drop-in; this addon only starts
	# the vision application.
	# 这块板只走 USB NCM（`ifup@usb0` 静态 10.42.0.1 + dnsmasq），NetworkManager 与
	# wpa_supplicant 对产品路径没有贡献，却卡在 `multi-user.target` 前面：实测 NM 占
	# 6.2 s、wpa 4.7 s，导致放核从 15 s 被拖到 23 s、相机从 25 s 拖到 32 s。镜像里那个
	# `network-services-disable.service` 本意就是关掉它们，但它在单元里调
	# `systemctl disable --now` 必然失败（自依赖），所以这层改为构建期 mask；那个单元
	# 本身被镜像的 enable 列表引用，不能一起 mask（`systemctl enable` 会直接报错）。
	# This board only uses USB NCM (`ifup@usb0` static 10.42.0.1 plus dnsmasq);
	# NetworkManager and wpa_supplicant add nothing to the product path yet sit in front
	# of `multi-user.target`: measured, NM costs 6.2 s and wpa 4.7 s, pushing the release
	# from 15 s to 23 s and the camera from 25 s to 32 s.  The image's own
	# `network-services-disable.service` intends exactly this but cannot work (it calls
	# `systemctl disable --now` from inside a unit), so mask them at build time instead.
	# That unit itself is in the image's enable list, so it must stay unmasked.
	@for unit in NetworkManager.service NetworkManager-wait-online.service \
	             wpa_supplicant.service; do \
	        ln -sf /dev/null "/rootfs/etc/systemd/system/$$unit"; \
	        rm -f "/rootfs/etc/systemd/system/multi-user.target.wants/$$unit"; \
	        rm -f "/rootfs/etc/systemd/system/graphical.target.wants/$$unit"; \
	done
	@mkdir -p /rootfs/tmp/install/
	@echo " guidance-vision.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
