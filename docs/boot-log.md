# 启动日志环 / The boot log ring

## 为什么不是日志文件 / Why not a log file

`bootlogd` 写的是 **U-Boot 读内核和 DTB 的那张 FAT 分区**。FAT 没有日志：运行时创建文件、
增长文件、改名、删除，每一次都在赌掉电的那一刻不会留下不一致的元数据。

这个赌局输过一次（2026-11-06）：

- `fsck.fat` 报 `snap.log` 的目录项写 23418 字节，簇链却有 24576 以上；
- 随后几轮启动从 U-Boot 里读出短文件——探针文件 `/uboot-loaded` 只有 376 字节，而 DTB 是
  21376 字节——`bootz` 拒收，启动失败；
- 因为失败是间歇的，当时把它误判成「SD 卡 I/O 坏了」，甚至准备换卡。真正的写入者就是正在
  记录这件事的那个守护进程。

日志把被观察的对象弄坏了，这不可接受。所以现在的形状是：

- 环在**镜像构建时**预分配：`/boot/bootlog.bin`，8192 字节一条，512 条（4 MiB）；
- 运行时 `bootlogd` 只做 `pwrite` 整条记录 + `fsync`，**文件大小和簇链一次都不变**；
- 掉电最多丢掉正在写的那一条；FAT 表、目录项永远不被修改，也就不可能留下不一致。

The boot log is written to the FAT partition U-Boot reads the kernel and DTB from.  FAT has
no journal, so creating, growing, renaming or deleting a file at runtime is a bet that a
power cut will not leave inconsistent metadata behind.  That bet was lost: `fsck.fat`
reported `snap.log` claiming 23418 bytes with a cluster chain over 24576, and the following
boots read short files out of U-Boot (a 376-byte probe where the DTB is 21376), which
`bootz` rejected.  The failures were intermittent, so they were blamed on the card.  The
writer was the logger.  Now the ring is preallocated at image build time and the daemon only
overwrites whole records in place with `pwrite` + `fsync`, so the file's size and cluster
chain never change.

## 记录格式 / Record layout

一条记录 8192 字节，小端：

```
b"BLOG" | index:u32 | length:u32 | crc32(payload):u32 | payload | padding
```

`index` 每次写加一，并且**跨重启递增**（守护进程从环里最大的 index 接着写），所以读取端
不需要任何「可变的头部」就能排序——头部一旦可变，它自己也会被写坏。

A record is 8192 bytes: magic, monotonic `index`, `length`, CRC32, payload, padding.  The
index survives reboots, so readers order records without a mutable header that could itself
be torn.

## 怎么读 / How to read it

```sh
# 在板上
bootlogd --show                       # 默认 /boot/bootlog.bin
bootlogd --show /boot/bootlog.bin | tail -80

# 卡插在电脑上（不需要启动板子、不需要 root 之外的任何东西）
sudo mount /dev/mmcblk0p1 /mnt
python3 scripts/addons/bootlog/bootlogd --show /mnt/bootlog.bin
```

`--show` 会跳过 magic/CRC 不匹配的记录（就是掉电时正在写的那一条），不会瞎猜。

`--show` prints the records oldest first and skips records whose magic or CRC does not match
— the torn one from a power cut.

## 卡写完之后先验，再上电 / Verify the card before booting it

```sh
sudo scripts/ci/verify-card.sh /dev/mmcblk0 output/maixcam_sd.img
```

它做三件事，都是「卡的问题」和「镜像的问题」的分界线：

1. 逐字节比对卡上镜像区域与镜像文件——排除「dd 没写完 / 超时被打断」这一类假故障；
2. 对两个分区跑只读检查（`fsck.fat -n`、`e2fsck -fn`）——排除「卡上文件系统本来就坏」；
3. 用 `mtools` 把 `/bootlog.bin` 取出来 dump 成文本——不启动板子也能看上一次启动的记录。

`verify-card.sh` compares the card against the image byte for byte, runs read-only
filesystem checks on both partitions, and dumps the ring with mtools — so an "SD card
problem" report can say which of the three it actually is.

## 卡真的坏了会是什么样 / What a real card fault looks like

真实硬件故障在快照里有自己的一节：`--- mmc:` 只列 `dmesg` 里带 `mmc` 且含
`error/timeout/retry/fail/reset` 的行。写入方造成的 FAT 不一致**不会**出现在那里——
这正是两者要分开记的原因。

A real card fault shows up in the snapshot's `--- mmc:` section, which lists only dmesg
lines that mention `mmc` together with an error word.  A FAT inconsistency caused by the
writer cannot appear there; keeping them apart is the point of that section.
