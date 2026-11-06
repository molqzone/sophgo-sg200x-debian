$(BUILDDIR)/systemd-lean-stamp:
	@echo "$(COLOUR_GREEN)Installing lean systemd defaults for $(BOARD)$(END_COLOUR)"
	@mkdir -p /rootfs/etc/systemd/system.conf.d/
	@mkdir -p /rootfs/etc/systemd/system/systemd-udevd.service.d/
	@mkdir -p /rootfs/etc/systemd/system/systemd-logind.service.d/
	@cp -a addons/systemd-lean/overlay/etc/systemd/system.conf.d/*.conf /rootfs/etc/systemd/system.conf.d/
	@cp -a addons/systemd-lean/overlay/etc/systemd/system/systemd-udevd.service.d/*.conf /rootfs/etc/systemd/system/systemd-udevd.service.d/
	@cp -a addons/systemd-lean/overlay/etc/systemd/system/systemd-logind.service.d/*.conf /rootfs/etc/systemd/system/systemd-logind.service.d/
	@chmod 0644 /rootfs/etc/systemd/system.conf.d/*.conf /rootfs/etc/systemd/system/systemd-udevd.service.d/*.conf /rootfs/etc/systemd/system/systemd-logind.service.d/*.conf
# 板子上没人跑 apt 定时升级、也没人建 man 索引。这些 timer 会在启动后把 IO 抢走几分钟，
# 在实测里和"开机慢、ssh 半天才响应"直接相关；mask 掉不改变功能，只去掉后台噪音。
# Nobody upgrades this board on a timer and nobody indexes man pages on it.  These
# timers grab IO for minutes after boot; masking them removes background noise without
# removing a capability.
	@install -d /rootfs/etc/systemd/system
	@for timer in apt-daily.timer apt-daily-upgrade.timer man-db.timer \
		e2scrub_all.timer motd-news.timer dpkg-db-backup.timer; do \
		ln -sfn /dev/null "/rootfs/etc/systemd/system/$$timer"; done
	@touch $@
