# 核间协议 / The inter-core protocol

两页 4 KiB 的共享内存，一颗 RISC-V 大核（Linux）和一颗 C906L（FreeRTOS 固件）之间，只有
这些：

```
0x8FFDB000  4 KiB  console page   固件往这里写 printf 环形缓冲
0x8FFDC000  4 KiB  shared page    页头 + 遥测环 + 名字服务 + 命令区
```

地址只声明一次，在镜像侧是 [configs/maixcam/memmap.py](../configs/maixcam/memmap.py)；DTS 的
`guidance-pages` 节点（`reg-names = "console","shared"`）把它暴露成 uio 映射；固件构建通过
`SG200X_GUIDANCE_PAGE_BASE` 拿到同一个值。`scripts/ci/validate.py` 强制三处相等。
`cop` 从不硬编码 map 序号：它读 `/sys/class/uio/*/maps/mapN/name`，按 `reg-names` 找页。

Two 4 KiB pages, nothing else.  The addresses are declared once (memmap.py on the image
side), exposed to userspace by the DTS node as uio maps named by `reg-names`, and handed
to the firmware build as one value.  `cop` finds the maps by name, never by index.

## 页头 / Page header (32 B, 8 × u32 LE)

| 偏移 | 字段 | 本产品的值 | 含义 |
|---|---|---|---|
| 0 | `magic` | `0x31506461` | 页自描述魔数；`0` = 冷页，其它 = 不是我们的页 |
| 4 | `abi` | `1` | 布局版本，改布局就改它 |
| 8 | `page_size` | 4096 | 页字节数 |
| 12 | `slot_count` | 58 | 遥测环槽数 |
| 16 | `slot_bytes` | 64 | 每槽字节数 |
| 20 | `payload_bytes` | 24 | 命令区负载字节数 |
| 24 | `region_offset` | 4064 | 命令区起始（页头里声明，不是推导出来的） |
| 28 | `wire_tag` | 6 | 线上协议的代号，两端必须一致 |

页头是**自描述**的：`cop status`、`cop dump`、`cop watch` 不需要任何配置文件就能读懂它。
从页头推导出来的还有三处（见 [cop](../components/guidance-fw/tools/cop) 的 `geometry()`）：
遥测环 @64、`head` @3776、`write_state` @3780、名字服务 @3840。

The header is self-describing, so the page can be inspected with no configuration at all.
Everything else is derived from it, and `validate.py` re-derives it with the tool and
compares it against the published contract.

## 遥测环 / Telemetry ring

58 槽 × 64 B，从 @64 开始。每槽 = 16 B 传输头（含 CRC32）+ 48 B 样本（µs 时间戳、g、
rad/s、伺服命令字）。固件是唯一写者，写 `head`（每发布一条 +1）；`write_state` 是非 0 时
表示有一次发布正在进行，读取端要跳过那一条。

58 slots of 64 bytes.  The core is the only writer; it advances `head` per published
sample, and `write_state` marks a sample being published.  A stale non-zero
`write_state` after a core restart is a claim left behind by a dead writer — the firmware
breaks it at bring-up without touching `head` or `seq`.

## 名字服务 / Name service

@3840，184 B：`magic = 0x3130534E`（"NS01"）、`count`、`generation`，然后最多 4 条 42 B 的项：
`key:u32`（端点键，与 LibXR Topic 一致）、`endpoint:u32`、`flags:u32`、`name_len:u16`、
`name:28B`。本产品公告一条：`imu`。

The table is written by the core at bring-up; `cop status` prints what is actually
announced, so "the core is up but publishes nothing" is visible instead of inferred.

## 命令区 / Command region

@4064，24 B：

```
4064  magic:u32 = 0x31444D47 ("GMD1")
4068  type:u16          4088  write_state:u32
4070  seq:u16           4092  seq:u32（最近一次的序号）
4072  payload:16 B
```

Linux 是唯一写者。`seq` 每次 +1 并回写 4092；`cop send` 用它和门铃回执对账。

Linux is the only writer: it fills the slot, bumps and stores `seq`, then rings the
doorbell.

## 门铃 / Doorbell

命令**不是**通过共享页通知的，而是厂商 cmdqu：`/dev/cvi-rtos-cmdqu` 上一次
`RTOS_CMDQU_SEND_WAIT`（`ip_id = 6`，`cmd = 0x7F`，bit7 = 阻塞，3 s 超时），固件把 `seq`
原样回给 `param`；`0xFFFFFFFF` 表示拒绝。**IRQ 101 属于厂商 `cv181x_rtos_cmdqu` 驱动**，
所以 uio 页没有中断：用户态靠轮询 `head`/`index` 发现新数据（`cop watch`、`cop log`、
`bootlogd` 都是这么读的）。

The mailbox page is a doorbell, not a transport: the command travels in the shared page
and the ioctl only says "look".  IRQ 101 belongs to the vendor driver, so the uio maps
carry no interrupt and readers poll.

## 所有权 / Who owns what

| 事实 | 唯一声明处 |
|---|---|
| 页地址 | [memmap.py](../configs/maixcam/memmap.py)，DTS `guidance-pages`，契约 JSON |
| 页布局（魔数/偏移/槽数） | 固件（页头），契约 JSON 是它的副本 |
| 谁格式化页 | **固件**：它在 bring-up 里 `Format()` 并证明自己写进去了 |
| 谁写遥测/名字服务 | 固件 |
| 谁写命令区 | Linux |
| 谁通知谁 | Linux → 固件：cmdqu 门铃；固件 → Linux：无（轮询） |

因此 `cop` **没有 `format` 动词**：一页没有人建，就是一个要报告的事实，不是可以从另一边
覆盖的东西。`cop status` 打印 `UNFORMATTED`（全零冷页）/ `FOREIGN`（不是我们的页）/
`FORMATTED`，调用方自己决定这意味着什么。镜像侧不再有任何"放核前先格式化"的步骤。

所以"页没建立"在板子上是可见的：`bootlogd` 每 15 s 记一次 console 环的 `index`，冻结的
`index` 就是冻结的核——这条记录比任何 `dmesg` 尾部都更直接。

There is no `format` verb: a page nobody built is a fact to report, not something to
overwrite from the other side.  The image has no "format before releasing the core" step
at all; a core that never formatted leaves `UNFORMATTED`, and the boot log's console
index freezing is the freeze signature.

## 契约 JSON / The contract file

`/lib/firmware/guidance_contract.json`（与 ELF 同目录，由镜像 pin 住）携带页头**装不下**的
东西：帧几何与校验、通道名、命令取值、console 环几何。`cop send` 需要它；`cop status`
不需要。

构建期三方对账（[validate.py](../scripts/ci/validate.py)）：DTS `reg` = `memmap.py` =
契约的地址；契约声明的偏移必须等于 `cop` 从页头推导出的偏移；名字服务/命令区必须整段落在
页内且不重叠；通道条目的名字、键、端点必须自洽。任何一项不一致都是红构建——因为这类漂移在
板子上的表现是"页错位"，而在代码里看不出来。

The contract carries only what the header cannot.  The build re-derives the layout with
the tool and compares, checks that every region fits and that entries are self-consistent;
drift here shows up on a board as a misaligned page, which no amount of reading the code
would reveal.

## 怎么读 / Reading it

```sh
cop list                 # 页与映射（只看设备树）
cop status               # 页头、布局、通道、head/seq
cop dump --offset 3776 --bytes 8
cop log --new            # console 环（无中断，轮询）
cop watch                # 新遥测槽，按裸字节打印
cop send --contract /lib/firmware/guidance_contract.json --type 1 --payload ''
```
