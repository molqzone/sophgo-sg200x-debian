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

## 九、2026-11-07 夜：把"放核即冻死"二分到一行

### 9.1 先纠正三个被我自己带偏的结论

1. **IMU 无罪。** 不启动模块图、不碰 SPI2、不发 DMA 的最小固件照样冻。
2. **`clk_ignore_unused` 不是解药。** 全时钟门开着（EN1/EN2 = `0xFFFFFFFF`）一样冻；只打开
   `clk_sdma_axi` 也不够。
3. **`5f5de2e`（UART0 焊盘 / PWM14/15）无罪。** 不含它们的 `8d006384` 一样冻。

还有一个更根本的教训：**6 秒的观察窗口会给出假阳性。** 阶段 0 盯了 90 秒、阶段 4 盯了
65 秒、阶段 12/15 各盯 45–60 秒，这四条才可信；之前"6 秒没死就算通过"的判定作废。

### 9.2 运行时阶段探针（`GUIDANCE_STAGE_PROBE`）

每次改固件都要重编 + 换卡 + 断电，成本高到无法二分。现在阶段号是**运行时**的：Linux 用
`/dev/mem` 往 boot-trace 页偏移 `0x800` 写一个数（`cvitek-rproc` 只用该页前 16 字节），
固件在 `CreateDefaultTask` 第一句读它，走到指定阶段就自旋。

```sh
# 放核前选阶段（0–16），掉电后该字归 0，所以冷启动永远停在阶段 0
devmem 0x8FFFF800 32 15
echo start > /sys/class/remoteproc/remoteproc0/state
```

配套的 `/usr/bin/rtos-mode` 闸门只加载模块、不放核，所以"放核"是一次显式操作，冷启动
永远回到有网络、可控的状态——这是能连续做二十多次实验的前提。

### 9.3 结果

| 阶段 | 内容 | 结果 |
|---|---|---|
| 0 | 只放核 | ✅ 90 s |
| 4 | DMAC quiesce（RCC 门 + DMAC 寄存器） | ✅ 65 s |
| 5–9 | SPI2 焊盘/时钟/控制器、GPIO、PWM10/舵面×4/屏幕 14·15、存储 | ✅ |
| 10 | 页 `Format()` + 读回（`cop status` 实测 FORMATTED、`imu` 已通告） | ✅ |
| 12 | + 两个定时器 | ✅ 45 s |
| 15 | + app/命令线程（命令线程阻塞在门铃上） | ✅ 60 s |
| **16** | **同上，但主线程用 `vTaskDelay` 让出 CPU** | ❌ **10 s 内冻死** |

**结论：固件初始化的一切都是干净的，缺的只有一件事——主线程把 CPU 让出去。**
用 `nop` 忙等停住，板子活；用 `vTaskDelay` 停住，板子死。

### 9.4 下一步该查什么（按优先级）

1. **小核 tick 与大核 tick 是否互相踩。** SDK 的 port 用
   `mtimecmp = configMTIMECMP_BASE_ADDRESS + mhartid * 8`，写法正确；但**两颗核报出来的
   `mhartid` 是不是同一个值**没验证过。若小核把自己当成 hart 0，它就会每 1 ms 改写大核的
   `mtimecmp`——Linux 的 tick 停、ssh 立刻失去响应，而小核自己照常跑（正好是观测到的样子）。
   验证方法：从大核读 `/proc/cpuinfo` 与设备树 `cpus` 节点，再让固件把 `mhartid` 写进页里
   对比。**这是目前最值得先查的一条。**
2. 若 hart 不冲突，再看"让出 CPU 后才会发生的东西"：libxr 定时器任务在同一时刻的
   `SleepUntil` 路径、以及 `LibXR::Thread` 的让出是否碰了共享的 systimer。
3. 只有 1、2 都排除后，才回到 mailbox 硬件自旋锁（`SPIN_UART`）那条线：内核补丁里
   `CVITEK_SPIN_UART` 的注释写着 "this spinlock field is used for linux & rtos"，如果固件
   在持有它的时候被挂住，大核任何一次 printk 都会卡死。

### 9.5 这次一并落地的仓库改动

- 固件：`GUIDANCE_STAGE_PROBE`（运行时阶段探针）、`GUIDANCE_APP_TIMERS`（定时器开关）、
  `GUIDANCE_START_MODULE_GRAPH`（模块图开关）、`GUIDANCE_BRINGUP_PARK`（空探针）。
- 镜像：`clk_ignore_unused` 回到 cmdline（实测不是解药，但它让时钟状态可预期，先留着）。

### 9.6 结论（2026-11-07 22:2x）：元凶是小核的 idle 任务

§9.3 里"阶段 16 冻、15 活"的差别，最终落在一件事上：**FreeRTOS 的 idle 任务有没有被调度**。

- 所有"停住"的阶段都用 `nop` 忙等——app_main 一直占着 CPU，**idle 永远得不到调度**，全部存活；
- 一旦主线程 `vTaskDelay` 让出 CPU，或完整跑到 `vTaskSuspend`，idle 开始跑，**大核 5–10 秒内失去响应**；
- 去掉 libxr 的定时器任务（不调 `PlatformInit`）**无效**——不是它；
- 放一个优先级 1 的自旋任务挤掉 idle（优先级 0）→ **阶段 20 存活 60 秒，固件完整可用**。

**一次被我自己弄坏的实验值得记下来**：第一次测"挤掉 idle"时结果是"无效"，因为
`LibXR::Thread::Create()` 返回 `void`（失败走内部 ASSERT）、`stack_depth` 的单位是**字节**
而我传了 512，而且我没检查任务是否真的建起来。修正后日志给出 `keep-awake: tasks 4 -> 5`，
实验才成立。**凡是"加了个东西却没效果"的结论，先证明那个东西真的存在。**

修复后实测（同一块板、同一张卡）：

```
$ cop status
page      : FORMATTED magic=0x31506461
geometry  : abi=1 size=4096 slots=58 slot=64 payload=24 region=4064 tag=6
channels  : 1 announced (gen 1) at 3840
            imu          endpoint=0x400 flags=1 key=0xb784461b
head      : 7048 → 8623        # 两次读之间，遥测一直在发布
```

固件控制台里也能看到这次真正生效的那些功能：

```
bring-up: screen PWM14/15 owned (pad 436/440, no panel present)
bring-up: board ok
page layout: abi=1 size=4096 slots=58 sample=64 payload=24 region=4064 tag=6
Shared page formatted; IMU telemetry publishing to the ring.
keep-awake: tasks 4 -> 5
```

**仍未解释**：为什么小核空转会拖死大核。这颗 SoC 上没有任何文档说 idle 会影响另一个核，
port 里也没有 `WFI`（`configUSE_TICKLESS_IDLE=0`）。可疑方向按顺序：

1. 硬件自旋锁：内核补丁定义了 `CVITEK_SPIN_UART` / `CVITEK_SPIN_LINUX_RTOS` /
   `CVITEK_SPIN_MBOX` 三个两核共享的锁（注释原话 "this spinlock field is used for linux
   & rtos"）。已在固件里加了 `GUIDANCE_DOORBELL_SIGNAL`，可以在不敲铃的情况下复测阶段 20。
2. C906L 空闲时的时钟/电源握手把 AXI 拖住（需要示波器或厂商文档才能确认）。

**代价**：keep-awake 是自旋，小核不再省电。作为工程解它能让固件稳定运行；作为根因它只是
把问题挪开——真要用在功耗敏感的产品上，得先弄清第 1、2 条。

### 9.7 修好之后：IMU 真的通了

keep-awake 修好后的第一件事就是把**完整产品固件**（探针关、模块图开）放上去跑。实测 60 秒板子
稳定，然后固件控制台给出：

```
IMU gyro rate: 999 Hz
IMU telemetry: entering Printf
IMU g +0.005 -0.012 +0.001 rad/s | a -0.045 +0.177 +0.978 g
IMU g +0.004 +0.002 -0.004 rad/s | a -0.049 +0.181 +0.979 g
```

重力落在 Z 轴 ~0.98 g、角速度近零——数据是真的，BMI088 走 SPI2 在 999 Hz 采样，并已发布到
共享页（`cop status`：`FORMATTED`、`imu` 通道已通告、`head` 在涨）。

**这也解释了今天反复出现的 `BMI088: Init failed. Try again.`**：它不是"这块板子没有 IMU"，
而是**固件从来没进入稳态**的那个循环。固件完整跑起来之后，IMU 一次就初始化成功。

### 9.8 另一条线：heartbeat 灯不亮（已单独修掉）

- 用户 LED 是 **GPIOA14**，而那颗焊盘（`0x03001038`）的主功能是 **`SD0_PWR_EN`**：
  `SD0_PWR_EN__SDIO0_PWR_EN = 0`、`SD0_PWR_EN__XGPIOA_14 = 3`；
- u-boot 的 `cvi_board_init()` 写了 3，但**随后它要从 SD 卡读内核**，那次 MMC 初始化又按
  `PINMUX_SDIO0` 把它设回 0；
- 于是 Linux 侧一切看着正常（`trigger=[heartbeat]`、`brightness` 在翻转），但电平到不了引脚；
- 同一次读数里 `0x03001070/74`（UART1 的两个焊盘，同一段代码设的）**也是 0**——所以
  `console=ttyS1` 从来没有输出，不是"没接线"，而是**焊盘没复用成 UART1**。

修法：`scripts/addons/maixcam-pinmux/`（`local-fs` 之后的 oneshot，补 `0x38=3`、`0x70=1`、
`0x74=1`），已进 `IMAGE_ADDITIONS` 并提交（`a68dee4`）。

### 9.9 正常启动路径下的端到端验证（2026-11-07 22:3x）

`rtos-mode` 恢复原版（开机自动放核）、卡内固件 `04e364e7`（探针关、模块图开、keep-awake 开）后，
一次普通断电重启上验到的：

```
uptime 0 min（NCM/ssh 正常）
remoteproc0 state = running                  # 正常启动路径放下了小核
/lib/firmware/c906-mcu.elf = 04e364e77709
devmem 0x03001038 = 0x00000003               # 灯焊盘被 oneshot 补成 GPIOA14
/dev/cvi-rtos-cmdqu 存在                      # 厂商模块链这次装齐了

$ cop status
page      : FORMATTED magic=0x31506461
channels  : imu  endpoint=0x400 flags=1 key=0xb784461b
head      : 1020 → 1105（3 秒内）              # 遥测在持续发布

$ cop log
IMU gyro rate: 999 Hz
IMU g +0.003 -0.009 -0.002 rad/s | a -0.045 +0.183 +0.977 g

$ cop send --contract /usr/lib/firmware/guidance_contract.json --type 1
sent type=1 seq=1, reply=0x00000001
sent type=1 seq=2, reply=0x00000002
sent type=1 seq=3, reply=0x00000003
```

**L1 与 L2 全部通过，IMU 在 999 Hz 采样并发布，命令一问一答。**

### 9.10 交接时的状态与待办

- 卡内固件 `04e364e7`；`/usr/bin/rtos-mode` 为原版（自动放核）；`/boot/c906-state` 与闸门脚本
  仍在但已不被调用，可删；
- 卡上 cmdline 仍带 `clk_ignore_unused`（调试期加回的），未验证是否仍必要——**下一次做单变量
  把它去掉再测一遍**；
- `maixcam-pinmux` 这个 oneshot 目前只在卡上手工装过 + 仓库里有 addon；**要 `make image`
  才会进正式镜像**；
- **根因仍未解**：为什么 C906L 的 idle 任务会拖死大核（§9.6 末）。keep-awake 是工程解，不是解释。

### 9.11 舵机上电"打满"的真正原因与修复

`PWM4`/`PWM5` 的焊盘就是 **UART0_TX / UART0_RX**，而 UART 空闲电平是**高**、RX 更是输入（悬空）。
所以从**上电到固件接管**（约 16 秒）之间，接在这两个脚上的两个舵机分别表现为"**直接打满**"和
"**保持力矩 + 偶尔抽一下**"——另外两个舵机在 `GPIO_RTX`/`GPIO_ZQ` 上，复位后不是 UART，所以没事。
这解释了为什么只有这两个出问题（实测四个通道寄存器完全一致，都是 150 Hz / 1.5 ms 中位）。

修复分两层：

1. `u-boot` 的 `misc_init_r`（交棒给 Linux 之前）：把 PWM4/PWM5 配成 150 Hz、只高 1 tick
   （= 无脉冲），再切焊盘，最后使能。顺序与固件 LL 驱动一致，避免半个周期的怪电平。
   窗口从 ~16 s 缩到 u-boot 自己那 2-3 s。**必须同时打开 `CONFIG_MISC_INIT_R`**，否则
   `--gc-sections` 会把整个函数回收掉——代码在源码里、却不在二进制里，这个坑很容易漏。
2. `maixcam-servo-early.service`（卡上）：systemd `sysinit.target` 即补一遍同样的值，
   在 u-boot 修复还没落地时把窗口压到 1-2 s。

### 9.12 一个设计层面的缺口：制导的输入源不存在

`User/xrobot_main.hpp` 里的链路是完整的：

```
"vision_target" -> ProNavGuidance -> "png_yaw" -> SurfaceActuator -> PWM1/2/4/5
```

但**没有任何东西发布 `vision_target`**：共享页契约里只有 `imu` 一个通道（小核→大核），
小核内部也没有生产者。于是 `png_yaw` 恒为 0，`SurfaceActuator` 把四个舵面钉在
`center=1500 µs`——看起来像"制导没跑"，其实是**没有输入**。

要让舵面真的跟着目标动，先得定：视觉目标由 Linux 通过共享页下发（契约里加大核→小核的通道），
还是小核自己接摄像头。这是设计缺口，不是 bug。
