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
	# 厂商放核入口的空操作 drop-in（50- 覆盖 guidance-fw 的 20-）：保留单元可 enable，
	# 但不再放厂商固件。
	# No-op drop-in for the vendor release hook (50- overrides guidance-fw's 20-): the unit
	# stays enable-able but releases nothing.
	@install -D -m0644 addons/guidance-autostart/load-systemko-none.conf \
	        /rootfs/etc/systemd/system/load-systemko.service.d/50-guidance-autostart.conf
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
	# `maixcam-ssh-authorize` 自己实测要 5.7 s（14.6 → 20.3 s，脚本内部耗时，不是等设备），
	# 而它 `Before=ssh.service`，于是 ssh → multi-user → 放核 → 相机全排在它后面。它做的事
	# （导入 boot 分区的公钥、退役旧的 usb-mode-autorevert 定时器）对全新刷写的卡都是空操作，
	# 却占着关键路径。这里只清掉那条排序，单元照常运行、只是不再挡住启动。
	# `maixcam-ssh-authorize` takes 5.7 s measured (14.6 -> 20.3 s, inside the script,
	# not waiting on anything) and is `Before=ssh.service`, so ssh -> multi-user ->
	# release -> camera all queue behind it.  What it does (import keys from the boot
	# partition, retire the legacy usb-mode-autorevert timer) is a no-op on a freshly
	# written card, so only the ordering goes away: the unit still runs, it just no
	# longer gates the boot.
	@mkdir -p /rootfs/etc/systemd/system/maixcam-ssh-authorize.service.d
	@printf '[Unit]\nBefore=\n' > \
	        /rootfs/etc/systemd/system/maixcam-ssh-authorize.service.d/50-guidance-autostart.conf
	@mkdir -p /rootfs/tmp/install/
	@echo " guidance-release.service guidance-vision.service" >> /rootfs/tmp/install/systemd-enable
	@touch $@
