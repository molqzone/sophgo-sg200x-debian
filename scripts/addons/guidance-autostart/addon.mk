# Boot the C906L and the vision app from a power cycle, and ship every binary the board
# needs so a reflash reproduces the working system with no manual injection.
#
# The core release is guarded: `guidance-core-guard.sh` refuses to release the core if the
# previous boot that released it never reached `guidance-health` (90 s in).  That is what
# keeps a bad release from turning the board into a brick -- the boot after a failed one
# comes up with the core left off and a marker explaining how to retry.
$(BUILDDIR)/guidance-autostart-stamp: addons/guidance-autostart/guidance-core.service \
                                      addons/guidance-autostart/guidance-core.timer \
                                      addons/guidance-autostart/guidance-vision.service \
                                      addons/guidance-autostart/guidance-health.service \
                                      addons/guidance-autostart/guidance-health.timer \
                                      addons/guidance-autostart/guidance-core-guard.sh \
                                      addons/guidance-autostart/guidance-core-health.sh \
                                      addons/guidance-autostart/loadmedia.sh \
                                      addons/guidance-autostart/files/c906-mcu.elf \
                                      addons/guidance-autostart/files/guidance_vision_app \
                                      addons/guidance-autostart/files/sensor_cfg.ini
	@echo "$(COLOUR_GREEN)Installing guidance-autostart for $(BOARD)$(END_COLOUR)"
	@mkdir -pv /rootfs/etc/systemd/system/ /rootfs/usr/local/bin/ /rootfs/lib/firmware/ /rootfs/var/lib/guidance/
	@cp -a addons/guidance-autostart/guidance-core.service \
	         addons/guidance-autostart/guidance-core.timer \
	         addons/guidance-autostart/guidance-vision.service \
	         addons/guidance-autostart/guidance-health.service \
	         addons/guidance-autostart/guidance-health.timer /rootfs/etc/systemd/system/
	@chmod 0644 /rootfs/etc/systemd/system/guidance-*.service /rootfs/etc/systemd/system/guidance-*.timer
	@install -m0755 addons/guidance-autostart/loadmedia.sh /rootfs/usr/local/bin/loadmedia.sh
	@install -m0755 addons/guidance-autostart/guidance-core-guard.sh /rootfs/usr/local/bin/guidance-core-guard.sh
	@install -m0755 addons/guidance-autostart/guidance-core-health.sh /rootfs/usr/local/bin/guidance-core-health.sh
	@install -m0755 addons/guidance-autostart/files/guidance_vision_app /rootfs/usr/local/bin/guidance_vision_app
	@install -m0644 addons/guidance-autostart/files/c906-mcu.elf /rootfs/lib/firmware/c906-mcu.elf
	@install -D -m0644 addons/guidance-autostart/files/sensor_cfg.ini /rootfs/mnt/data/sensor_cfg.ini
	# 不放 /dev/null 掩码：镜像的 `image-customize` 还会 `systemctl enable
	# load-systemko`，掩码会让那一步直接失败（实测 `Failed to enable unit: ... is
	# masked`）。放核顺序是"厂商固件（load-systemko 开机）→ 我们的固件（本 timer
	# +20 秒替换）"，真正的安全网是守卫：一次失败的放核会把自动放核关掉。
	# No /dev/null mask: the image's `image-customize` still runs `systemctl enable
	# load-systemko`, and a mask makes that step fail (measured).  The release order is
	# vendor firmware (load-systemko at boot) followed by ours (this timer at +20 s); the
	# safety net is the guard, which disables automatic releases after a failed one.
	# 厂商放核入口的空操作 drop-in（编号 50 覆盖 guidance-fw 的 20-）。
	# The no-op drop-in for the vendor release hook (50- overrides guidance-fw's 20-).
	@install -D -m0644 addons/guidance-autostart/load-systemko-none.conf \
	        /rootfs/etc/systemd/system/load-systemko.service.d/50-guidance-autostart.conf
	@mkdir -p /rootfs/tmp/install/
	@echo " guidance-core.timer guidance-vision.service guidance-health.timer" >> /rootfs/tmp/install/systemd-enable
	@touch $@
