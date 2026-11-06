# 启动日志写的是 U-Boot 读内核和 DTB 的那张 FAT 分区。FAT 没有日志，所以运行时**建**
# 文件、**增长**文件、改名、删除，每一次都可能让这张分区在掉电时留下不一致——实测就发生
# 过：`fsck.fat` 报 snap.log 的目录项写 23418 字节、簇链却有 24576 以上，随后的启动从
# U-Boot 里读出短文件（DTB 是 21376 字节，标记文件只有 376）然后起不来。
# 所以环在**镜像里**预分配好：运行时 bootlogd 只按整条记录就地覆盖，文件大小和簇链一次
# 都不变。掉电最多丢掉正在写的那一条，FAT 表和目录项永远不被改。
# The boot log lives on the FAT partition U-Boot reads the kernel and DTB from.  FAT has
# no journal, so creating, growing, renaming or deleting a file at runtime can leave that
# partition inconsistent after a power cut -- measured: fsck.fat reported snap.log's
# directory entry claiming 23418 bytes with a cluster chain of more than 24576, and the
# following boots read short files out of U-Boot (376 bytes where the DTB is 21376) and
# failed.  The ring is preallocated here instead: at runtime bootlogd only overwrites
# whole records in place, so the file's size and cluster chain never change.
BOOTLOG_RING := $(ROOTFS)/boot/bootlog.bin
BOOTLOG_RECORD := 8192
BOOTLOG_SLOTS := 512

$(BUILDDIR)/bootlog-stamp:
	@echo "$(COLOUR_GREEN)Installing bootlog$(END_COLOUR)"
	@install -d "$(ROOTFS)/usr/local/sbin" "$(ROOTFS)/etc/systemd/system" \
		"$(ROOTFS)/boot"
	@install -m 0755 addons/bootlog/bootlogd "$(ROOTFS)/usr/local/sbin/bootlogd"
	@install -D -m 0644 addons/bootlog/bootlog-snap.service \
		"$(ROOTFS)/etc/systemd/system/bootlog-snap.service"
	@dd if=/dev/zero of="$(BOOTLOG_RING)" bs=$(BOOTLOG_RECORD) count=$(BOOTLOG_SLOTS) \
		status=none
	@test -s "$(BOOTLOG_RING)"
	@test "$$(stat -c %s '$(BOOTLOG_RING)')" -eq \
		$$(( $(BOOTLOG_RECORD) * $(BOOTLOG_SLOTS) )) || \
		{ echo "boot log ring is not $(BOOTLOG_RECORD)x$(BOOTLOG_SLOTS) bytes" >&2; \
		  exit 1; }
	@mkdir -p "$(ROOTFS)/tmp/install/"
	@grep -q "bootlog-snap" "$(ROOTFS)/tmp/install/systemd-enable" 2>/dev/null || \
		echo " bootlog-snap" >> "$(ROOTFS)/tmp/install/systemd-enable"
	@touch $@
