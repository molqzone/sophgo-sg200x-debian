# 快速迭代：`make sync` / The fast loop

## 问题 / The problem

`make image` 会重造一张 4.6 GB 的卡镜像：debootstrap 780 个包（10–15 min）→ 装 addon →
chroot 定制 → `genimage` 打包 4.2 GB 的 ext4 → 写到卡上（4 min）→ 写后回读校验（3 min）。
改一个 shell 脚本要等 25–35 分钟，而且大多数时候改的东西跟"镜像"没有关系。

`make image` rebuilds a 4.6 GB card image: debootstrap, addons, chroot customisation,
genimage, a four-minute write and a three-minute read-back.  Changing a shell script costs
25–35 minutes, and most changes have nothing to do with the image.

## 用法 / Usage

```sh
make sync                      # 推到 debian@10.42.0.1（ssh）
make sync ARGS="--dry-run"     # 只说会改什么，不动板子
make sync ARGS="--board debian@10.42.0.2"
sudo make sync ARGS="--root /run/media/$USER/root1"   # 卡挂在主机上时
```

推到板上时它先问板子每个目标文件的 sha256，只传不一样的那几个，然后
`systemctl daemon-reload`，并且只重启**能安全重启**的单元（目前只有 `bootlog-snap`）。

Over ssh it asks the board for each destination's sha256 first and sends only what
differs, then reloads systemd and restarts only what can be restarted safely.

## 它管什么、不管什么 / What it covers, and what it refuses to

管的（都是运行时文件）：

| 仓库里的文件 | 板上的位置 |
|---|---|
| `scripts/addons/bootlog/bootlogd` | `/usr/local/sbin/bootlogd`（重启 `bootlog-snap`） |
| `scripts/addons/bootlog/bootlog-snap.service` | `/etc/systemd/system/` |
| `components/guidance-fw/tools/rtos-mode` | `/usr/bin/rtos-mode` |
| `components/guidance-fw/tools/cop` | `/usr/bin/cop` |
| `components/guidance-fw/linux/systemd/…/20-guidance-fw.conf.in` | `/etc/systemd/system/load-systemko.service.d/`（按板级设置渲染） |
| `components/guidance-fw/firmware/guidance_contract.json` | `/lib/firmware/`（下次调用就生效） |
| `components/guidance-fw/firmware/c906-mcu.elf` | `/lib/firmware/`（**下次上电**才生效） |

不管的，以及为什么：

- **`/boot` 一个字节都不写。** 那张 FAT 分区是 U-Boot 读内核和 DTB 的地方，环是镜像预分配
  的（见 [boot-log.md](boot-log.md)）。卡里的启动分区内容要改，只能重建镜像——这是刻意的。
- **永不重启 `load-systemko.service`。** 这个镜像没有 stop 路径：对正在跑 SPI 采样的 C906L
  做 stop/restart 会把整块板子挂死（见 [shutdown-behaviour.md](shutdown-behaviour.md)）。
  所以固件 ELF 和 drop-in 只能等下一次上电，脚本会明说。
- 内核、U-Boot、FSBL、分区布局、装包（`apt` 层面的变化）仍然要 `make image`。

It never writes to `/boot` (the boot log ring is image-only on purpose) and never
restarts `load-systemko.service` (there is no stop path for the C906L).  Kernel, U-Boot,
FSBL, partition layout and package changes still need `make image`.

## 为什么它和镜像装的是同一份文件 / Why this is not a second implementation

快路径一旦和发布路径分叉，它就会悄悄给出错误结论，所以两条路装出来的字节能逐字节比对：
`test_dev_sync.py` 用真实的 addon 渲染一遍 drop-in，再与 `dev_sync.render()` 的结果比较；
manifest 里每个源文件、目标路径、模式、以及"哪些改动需要上电"都有断言。

A fast path that drifts from the release path quietly produces wrong conclusions, so the
bytes are compared: `test_dev_sync.py` renders the drop-in through the real addon and
compares it with `dev_sync.render()`, and asserts every manifest entry's source, target,
mode, and which changes need a power cycle.

## 前置条件 / Preconditions

- ssh 模式：`sshpass`（`dnsmasq`/NCM 网络起来后 `debian@10.42.0.1` 可达），板子的 sudo
  密码默认 `rv`，可用 `BOARD_PASSWORD=` 覆盖。
- 卡模式：`--root` 指向**挂载好的** rootfs（`udisksctl mount -b /dev/mmcblk0p2`），写
  `/usr` 下的文件需要 root（用 `sudo make sync …`）。
