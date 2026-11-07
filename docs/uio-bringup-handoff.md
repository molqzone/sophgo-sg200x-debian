# UIO 联调交接 / UIO bring-up handoff

这份文档记录 2026-11-06/07 两天里把 `guidance-pages`（uio + 共享页）在真机上跑通到
"第 1 层通过、第 2 层定位到根因"的过程。**结论比过程重要**，所以先给结论，再给证据。

## 一、当前状态

| 层次 | 内容 | 状态 |
|---|---|---|
| L1 管道 | 设备树 → uio → mmap → `cop` | ✅ **通过**（见 §3） |
| L2 协议 | 页头/通道表/门铃回执 | ✅ **通过 2026-11-07 20:05**（见 §8） |

> **2026-11-07 更正**：§3.4 / §3.6 里"产品固件把板子挂死"的结论是错的，板子从来没死过。
> 真正的两处根因见 §8：固件把 UART0 的焊盘认领给了 PWM4/5（于是内核控制台消失，看起来
> 像停机），以及 `dcache.cipa` 在这个核上会把没进 cache 的写丢掉（于是共享页永远格式化
> 不成功）。两处都修了，L2 实测通过。

## 二、两天里被推翻的假设（别再重走）

| 假设 | 判决 | 证据 |
|---|---|---|
| "镜像/DTB 坏了，所以不启动" | ✗ | 卡上 fip/vmlinuz/DTB 与镜像 sha256 完全一致；串口显示 U-Boot 每次都把内核完整读入 |
| "内存映射改动导致不启动" | ✗ | 恢复原内存映射后仍不启动；真因是卡在 ROM 层初始化失败（§3.1） |
| "小核没复位，把 DDR 初始化撞死" | ✗ | 给 FSBL 加了"DDR 之前按住小核"（`Hold C906L` 在串口可见），热复位依旧失败 |
| "reboot 必死" | ✗ | 惰性固件下 `systemctl reboot` 35 秒正常回来；只在**产品固件**在跑时才死 |
| "DMAC 时钟被内核关掉，固件一碰就挂" | ✗ | 加 `clk_ignore_unused` 后仍在小核起来后停住 |
| "GUIDANCE_FW_RPROC_MODE=load 是死因" | — | 已修（推导 + 守卫），但与本问题无关 |

## 三、已经确证的事实

### 3.1 卡没插到位时，ROM 就失败了

串口（UART0，115200）能看到 boot ROM 的原文：

```
E:bm_sd_send_cmd_without_data CMD: 8 INT_STAT: 0x
E:bm_sd_send_cmd_without_data CMD: 0 INT_STAT: 0x
E:SD initailizing failed
E:eMMC init failed, 3
E:load param1 (-5)
E:Boot failed (8).
E:RESET:plat/mars/platform.c:114
```

**判据**：`E:SD initailizing failed` ⇒ 卡没插好 / 卡接触不良，此时讨论镜像毫无意义。
同一张卡在读卡器上也出现过 `mmc0: error -110 whilst initialising SD card`。

### 3.2 正常启动链（卡插好时）

```
FSBL … Hold C906L … BL2 end
U-Boot 2021.10 … Sipeed MaixCAM
Loading Environment from FAT... OK
Found /extlinux/extlinux.conf
Retrieving file: /vmlinuz-…        4708669 bytes read
Starting kernel ...
（内核日志，前提是 console 指向 UART0，见 §5）
```

### 3.3 L1：uio 管道

```
cop list     guidance-pages (/sys/class/uio/uio0)
             map 0  console  addr=0x8ffdb000 size=4096
             map 1  shared   addr=0x8ffdc000 size=4096
cop status   page : FOREIGN magic=0x08004400     ← 惰性固件不建页，工具如实报告
cop dump     0000  00 44 00 08 …                 ← 读的是真实物理页
dmesg        remoteproc0: Started from 0x8fe00000
             remoteproc0: remote processor cv181x-c906_1 is now up
```

`FOREIGN` 不是 bug：惰性固件（`02f36c3a…`，379 872 B）不建页，`cop` 拒绝伪造
`FORMATTED` 正是设计意图。

### 3.4 L2：产品固件挂死的精确位置

```
[16.50] remoteproc1: Booting fw image c906-mcu.elf, size 1278632
[16.73] cvitek-rproc: boot trace host sentinel at 0x8ffff000
[25.70] RT: checksum fail (0000adff, 00002e61)
[25.71] RT: use default confi setting
（之后：串口静默、USB 消失、无 panic）
```

**这是"总线被挂住"的签名**：没有 oops/panic，因为 CPU 停在一个永远不回的 AXI 事务上，
连 printk 都打不出来。

### 3.5 第一层根因（已修）：FSBL 不再写 `transfer_config`

> **2026-11-07 19:2x 更新**：这一条已经修好。修完后串口上的 `checksum fail` **消失**，
> RTOS 改为打印 `dump_print_enable & log will not print` 并继续往下走——说明配置被接受。
> 但板子仍在约 25 秒后停摆，所以还有第二层原因（见 §3.6）。

原始分析保留如下。

`configs/common/patches/fsbl/0001-fix-compile-error.patch` 把
`plat/cv181x/bl2/bl2_main.c` 里 `init_comm_info()` 的**函数体**用 `#if 0` 关掉了：

```c
int init_comm_info(int ret)
{
#if 0
        struct transfer_config_t *transfer_config = (struct transfer_config_t *)MAILBOX_FIELD;
        ...
        transfer_config_s.checksum = checksum;
        memcpy(transfer_config, &transfer_config_s, sizeof(struct transfer_config_t));
#endif
        return ret;
}
```

关掉的原因是三个宏不存在（它们来自厂商 RTOS 的配置头，不在 FSBL 构建里）：

| 宏 | 语义 | 我们的取值 |
|---|---|---|
| `RTOS_FAST_IMAGE_TYPE` | fast-image 类型（JPEG/H264/H265） | `0`（不做 fast image） |
| `RTOS_DUMP_PRINT_ENABLE` | 是否启用 RTOS 的 `RT:` 打印环 | `1`（要保留可见性） |
| `RTOS_DUMP_PRINT_SZ_IDX` | 打印环大小索引 | `0` |

后果链：FSBL 不写 → mailbox 里是残值 → RTOS `checksum fail` → **退回内置默认配置** →
默认配置里的 DMA 参数不属于它 → 野 DMA → 总线挂死 → 整板停摆。

`use default confi setting` 这句在**厂商预编译库**里（源码树里搜不到，只在 ELF 里），
所以**无法从固件源码侧改成 fail-stop**；必须在 FSBL 侧把配置填对，让默认路径永不触发。

### 3.6 第二层根因（未解决）：产品固件自己的运行时

修好 comm-info 之后：

```
[16.19] remoteproc1: Booting fw image c906-mcu.elf, size 1278632
[16.42] boot trace host sentinel at 0x8ffff000
[25.39] RT: dump_print_enable & log will not print     ← RTOS 接受了配置（不再 checksum fail）
（之后：串口静默、USB 消失、无 panic）
```

同一块板、同一份 FSBL、同一个镜像，换成**惰性固件**（`02f36c3a…`，不碰 DMA）就能一路
到 ssh。所以第二层的原因在产品固件的运行时——最可能是它的 IMU/SPI2 DMA 路径
（固件侧 `sg200x_dma.hpp` 用通道 4–7 + 硬件请求线 20/21）。

判据：如果固件把 IMU/DMA 关掉后板子能活，就确认在这里；然后逐步加回去定位到具体一步。

## 四、待办（按顺序，做完就能验 L2）

### 4.1 FSBL：填一份"明确没有 fast-image / 没有编码缓冲"的配置

在 `configs/common/patches/fsbl/` 里恢复 `init_comm_info()` 的主体，并：

1. 补上 §3.5 的三个宏（写进补丁，带注释说明出处）；
2. **所有 DMA 缓冲地址/大小显式置 0**（`isp_buffer_*`、`encode_img_*`、`encode_buf_*`），
   而不是照抄 `CVIMMAP_*`：我们的 memmap 里这些区域要么是 0 长度、要么属于别的用途，
   照抄会把内核的地址交给 RTOS 去 DMA；
3. `image_type = 0`，`mcu_status = MCU_STATUS_NONOS_DONE`（厂商原值）；
4. checksum 仍按原逻辑对 `conf_size` 字节求和；

### 4.2 验证顺序

1. 重编 FSBL → 新 `fip.bin`（只换卡上 FAT 根目录那一份，不需要重打镜像）；
2. 串口确认 `RT:` 不再打印 `checksum fail`；
3. `cop status` 期望 `FORMATTED` + `channels: 1 announced (imu)`；
4. `cop send --contract /lib/firmware/guidance_contract.json --type 1` → 回执 `reply == seq`；
5. `bootlogd --show /boot/bootlog.bin` 里 `console_index` 应开始增长。

### 4.3 如果 RTOS 拿到配置后仍然挂

下一步的判据：串口上 `RT:` 输出是否继续（说明 RTOS 还活着）。
- 继续输出、只是门铃不回 → 协议问题（查 cmdqu/seq）；
- 直接静默 → 配置里的某个地址仍被用于 DMA，逐项置 0 重试（先 `isp_buffer`，再 `encode_*`）。

### 3.7 第三层（已实现，未上板验证）：固件的 DMA 边界断言

`bsp-guidance-action`（提交 `b7b8be6`）：

- `Core/sg200x_memmap.hpp` 新增固件自己的 DMA 域（`OWNED_DDR_PHYS/BYTES` = 2 MiB `fast_image`），
  并用 `static_assert` 钉住"域尾就是 DRAM 尾"；
- `driver/sg200x_dma.hpp` 在**碰任何寄存器之前**检查：内存侧必须在自己的域内，外设侧必须是
  DRAM 以下的 MMIO；否则返回 `ARG_ERR`，不使能通道。

判定逻辑有宿主侧单元测试（起点/终点/两侧各越界一字节/跨边界/空指针/零长度/SPI 与 DMAC 寄存器地址）。

**未完成的是那次构建**：固件构建需要 `SG200X_SDK_DIR` 指向一个完整的 SDK 源码树，而
`build/sdk-worktree/freertos` 目前没有 HEAD（`git rev-parse HEAD` 失败：
`fatal: ambiguous argument 'HEAD'`），所以 `scripts/sg200x-sdk/build.sh` 起不来。
下一步先 `bash scripts/sg200x-sdk/prepare.sh <worktree>` 重建工作树，再：

```sh
SG200X_SDK_DIR=$PWD/build/sdk-worktree \
SG200X_CROSS_COMPILE=/home/keruth/Development/xpack-riscv-none-elf-gcc-15.2.0-1/bin/riscv-none-elf- \
SG200X_GUIDANCE_PAGE_BASE=0x8FFDC000 \
  bash scripts/sg200x-sdk/build.sh
```

预期：如果 IMU 初始化时那条 DMA 的地址在域外，串口/固件就会拒绝它而不是挂总线；如果它
照样挂死，说明非法之处不在地址上（那就查通道/请求线配置）。

## 五、环境与工具（明天直接用）

### 5.1 串口（**最重要的工具**）

- 板子调试串口 = **UART0**，115200 8N1（CP2102 接在宿主 `/dev/ttyUSB0`）；
- 内核 cmdline 必须 `console=ttyS0,115200 earlycon=uart8250,mmio32,0x04140000,115200`；
  镜像里 `setup_rootfs.sh` 已改成同时带 `ttyS0` 和 `ttyS1`；
- **注意**：后台读进程会在每次工具调用结束时被杀。要抓启动必须
  `stty -F /dev/ttyUSB0 115200 raw -echo -icanon min 1 time 0` 之后
  **在同一次调用里** `timeout 80 cat /dev/ttyUSB0 > /tmp/cap.log`。

### 5.2 卡与镜像

- `bash scripts/ci/flash-card.sh <dev> output/maixcam_sd.img`：写 + 逐字节比对 + 双 fsck；
- 卡上 FAT 在 `sector 1`（不是 1 MiB）；`mcopy -i <img>@@512` 或直接 `mount` 分区；
- 换 DTB/固件/fip 这类单文件，**不需要重打镜像**，直接改卡上文件即可。

### 5.3 构建（网络时通时断）

- 上游 clone 可用但会断；离线镜像在 `offline-cache/git/`（linux 2 GB、u-boot、fsbl、opensbi、
  osdrv、cvi_mpi）；
- `make cache-mirror` 填充/更新镜像；`scripts/ci/seed-source-tarball.sh <COMPONENT>` 在
  `git clone` 完全不通时用 GitHub API 的 tarball 重建（内容一致、提交号不同，需要
  `BUILD_OVERRIDES` 覆盖 `*_COMMIT`）；
- `make clean`/`*-clean` 现在都要 `CONFIRM=1`；
- `make cache-status` 一条命令看哪些 stamp 在。

### 5.4 板子/固件

- 惰性固件：`components/guidance-fw/firmware/c906-mcu.elf`（`02f36c3a…`）——**板子能稳定跑**，
  但页永远是 `FOREIGN`/`UNFORMATTED`；
- 产品固件：`bsp-guidance-action/build/firmware/c906-mcu.elf`（`cb9016d…`）——建页、公告通道、
  但在当前 FSBL 下会把整板拖死（§3.5）；
- 换固件只需 `scp` + `sudo install` 到 `/lib/firmware/c906-mcu.elf`，然后**断电重上**（不 reboot）。

## 六、这两天已经落地的改动（都在 `baseline/camera-only-firmware` 上）

| 提交 | 内容 |
|---|---|
| `f06c974`/`1745047` | DMA 通道分区结论：固件 `OWNED_CHANNEL_MASK=0xF0`（0–3 Linux / 4–7 C906L）；实测内核一 probe DMAC 就挂，所以节点保持 disabled，通道数 4 只作为将来放开时的约束，`validate.py` 守卫 |
| `5b1dff4`/`0003` | FSBL 在 `load_ddr()` **之前**按住 C906L（热复位不复位小核，DDR 重训练会撞车） |
| `a34ab55`/`7653c33`/`8ff83e5`/`5f1f483` | 缓存工具链：`make cache-mirror`、`seed-source-tarball.sh`、`BUILD_OVERRIDES`、`insteadOf` 镜像路由、`*-clean` 守卫、`cache.py` 指名报错 |
| `f06c974` 前 | `&dmac` 曾短暂改成 `dma-channels=4 + okay` → **实测挂死**（7.4 s、无 panic），已回退并写进注释 |
| `2241f64` | 熵注入（`RNDADDENTROPY`）、uio udev 规则给 `plugdev`、`boot-time` 重试 `systemd-analyze` |
| `dd4efe7` | 删掉两个不是旋钮的旋钮（`RPROC_MODE`/`DOORBELL_OWNER`）、swapfile 1 GB → 256 MB |
| `0674423` | rootfs 去掉开发工具链（2.8 GB → 397 MB，镜像 4.6 GB → 593 MB） |
| `44b6f72` | `cop` 迁到 `molqzone/cop` 仓库，镜像侧按 commit+sha256 vendored |
| `f0d7648` | `cop` 去掉 `format` 动词、补测试、`docs/inter-core-protocol.md`、契约漂移检查 |
| `e44f91a`/`2241f64` | journal 持久化（`/var/log/journal`，16 MiB 上限）——挂死后能读关机尾巴 |
| `2241f64` 后 | 板级 `cvi_board_init.c` 打开 UART1 引脚（A28/A29），cmdline 同时带 `ttyS0`/`ttyS1` |

## 七、明天的第一件事

```sh
# 1. 改 FSBL 补丁（§4.1）
# 2. 只重编 FSBL，拿新 fip.bin
make fsbl BOARD=maixcam
ls -l .cache/build/boards/maixcam-sd/fip.bin
# 3. 换到卡上（不需要重打镜像）
# 4. 冷启动，串口抓 80 秒（§5.1）
# 5. 串口确认没有 checksum fail，然后：
cop status
cop send --contract /lib/firmware/guidance_contract.json --type 1 --payload ''
```

## 八、2026-11-07 20:05：L2 通过，以及被推翻的"挂死"

### 8.1 板子从来没有挂过

串口在 `[16.5] remoteproc1: Booting fw image` 之后出现一行
`RT: dump_print_enable & log will not print`，然后就再无输出。把它当成"板子停摆"是
错的——同一时刻板子：

```
uptime 3 min, load 3.29
/sys/class/remoteproc/remoteproc1/state = running
/dev/uio0 在位
NCM 网卡起来，192.168.100.2 ping 通、10.42.0.1 ssh 通（debian/rv）
dmesg 一直记到 227 s（我们登录触发的那条 journald 消息）
```

所以"产品固件把 Linux 打死"这条假设彻底作废；§3.4/§3.6 作废。串口消失只是**控制台
没了**，不是内核没了。

### 8.2 根因一：固件把 UART0 的焊盘认领给了 PWM4/5

`User/app_main.cpp` 的舵面 PWM 表前两路是 `UART0_RX → PWM5`、`UART0_TX → PWM4`，而
`SG200XPWM` 在构造函数里就写 pinmux。表上的注释还写着"本固件不用 UART0，Linux 控制台是
`ttyS1`"——那是旧前提；镜像已经把控制台搬到 `console=ttyS0`，于是固件一起来就把控制台
从引脚上摘掉。

证据链：
1. Linux 往 `/dev/ttyS0` 写，`rc=0`，`stty` 115200 正常，但线上一个字节都没有。
2. UART0 的 `LSR = 0x60`（THRE + TEMT 都置位）——字节真的移出去了。
3. 因此不是 UART，是**引脚**：认领 PWM4/5 就是把 UART0 从那两个焊盘上挪走。

修法：`TAIL_PWM_CLAIMS_UART0_PADS = false`，bring-up 不认领这两个焊盘，并在日志里明说
"surface N has no pad (UART0 console kept)"，而不是假装这两路舵面有输出。要恢复，得先
把镜像控制台搬回 UART1，或者把这两路改到 PWM14/15（触摸屏的 `IIC2_SCL/SDA`）。

### 8.3 根因二：`dcache.cipa` 在这个核上会丢数据

页格式化失败的表面症状是固件日志

```
E app_main.cpp:760 Shared page would not take a format; IMU telemetry is NOT running
```

`Format()` 的机器码核对过：从页对象读 7 个布局字段写进页头、`sw magic,0(page)`、
然后对整页做一次 `dcache.cipa`，紧接着 `Check()` 先 `invalidate(32)` 再读 magic。
问题是 `cipa`（一条指令做 clean+invalidate）执行时，刚写的 store 可能还在 store buffer
里：clean 那半看不到任何脏行，invalidate 那半照常把行作废，数据永久丢失。于是：

- 固件自己的 `Check()` 读到 FOREIGN → `Ready()` 假 → 整条遥测链停掉；
- 从 Linux 经 `/dev/mem` 读 `0x8ffdc000`，看到的是上电残留（`magic=0x08604460`），
  而不是页头。

修法（`driver/shared_page/sg200x_thead_coherence.hpp`）：`PageSync` 拆成
`fence iorw,iorw` → `dcache.cpa(范围)` → `dcache.ipa(范围)`；`PagePublish` 也补上前导
fence。顺序不能倒——clean 必须先看见脏行，invalidate 才有资格丢掉已经落到 DRAM 的那份。

顺带说明为什么 console 页一直是好的：它走 `Publish`（只有 clean），而且每次之后还有
`fence_io()`，所以从来没踩到 `cipa`。

### 8.4 验证记录

装上新固件后（`remoteproc` stop/start 即可，**不需要断电**——`Quiesce()` 让 rproc 重启
不再挂板，这点也顺手验证了）：

```
$ cop status
page      : FORMATTED magic=0x31506461
geometry  : abi=1 size=4096 slots=58 slot=64 payload=24 region=4064 tag=6
layout    : ring=64 head=3776 write_state=3780 channels=3840
channels  : 1 announced (gen 1) at 3840
            imu          endpoint=0x400 flags=1 key=0xb784461b
head      : 2001 (write_state 0)          # 两次读之间从 344 涨到 2001，环在前进

$ cop send --contract /usr/lib/firmware/guidance_contract.json --type 1
sent type=1 seq=5, reply=0x00000005
sent type=1 seq=6, reply=0x00000006
```

固件侧日志（经 `/dev/mem` 读 console 页，不需要串口）确认它已经跑过 `Format()` 并进入
IMU 初始化，只是在重试 `BMI088: Init failed. Try again.`——这块 MaixCAM 上没有 BMI088，
属于预期。

### 8.5 还没做的

1. **卡上还是旧固件**（`824e6b91`，没有这两处修复）。当前跑的是 scp 到 `/tmp` 再
   `cp` 进 `/lib/firmware` 的那份（`8d006384`），**冷启动会退回旧固件**。要么断电把卡
   拿出来装，要么就用 `remoteproc` stop/start 迭代。
2. `cop log` 在板子上直接崩：`NameError: name 'console_layout' is not defined`。这是
   **上游 `molqzone/cop` 的 `b40ca7f` 本身就有的 bug**（不是本地截断），`cmd_log` 调用
   了一个从未定义的函数。在修好之前，用 `/dev/mem` 读 `0x8ffdb000` 看固件日志。
3. 固件在 IMU 初始化失败时每 132 µs 打一行日志，4 KiB 的 console 环被刷满，bring-up
   阶段的日志会被挤掉。重试应该退避。
