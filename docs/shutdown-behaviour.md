# 小核的生命周期 / The C906L's lifetime

一句话：**小核没有自己的生命周期**——它由 Linux 用户态放核，活在 SoC 复位之间，跟着
同一次复位一起消失。镜像里没有「停核」这条路，这是实测结论，不是省事。

One sentence: **the small core owns no lifetime.**  Linux userspace releases it, it lives
between two SoC resets, and it dies with the reset that ends the session.  The image has
no stop path, and that is a measured conclusion rather than a shortcut.

## 形态 / The shape

| 边界 | 谁做的 | 结果 |
|---|---|---|
| 大核已经起来 → 才放核 | `load-systemko.service`：`ExecStartPre` 格式化共享页，`ExecStart=rtos-mode <mode>`（`remoteproc` 由 Linux 载入并 `start`） | 小核的起点晚于大核的用户态 |
| 运行中 | 无 | 没有 supervisor、没有心跳、没有 watchdog |
| 会话结束 | SoC 复位（`reboot` / `poweroff` 都走它） | 核连同 DMAC、SPI、PWM 一起复位 |

于是「小核生命周期 ⊆ 大核 boot session」不是靠关机钩子保证的，而是**起点更晚、终点
同源**：它不可能在大核之前出现，也不可能活过结束这次会话的那次复位。

So "lifetime ⊆ boot session" is not enforced by a shutdown hook but by *starting later
and dying with the same reset*.

## `park` 到底是什么 / What `park` really did

一句话：它不是「关机协议」，它是**让 partial reset 安全**的一小段 drain。`rproc stop` 只
复位 C906L，**不复位 DMAC**。DMAC 上的通道 4/5 属于本核，只有在一次传输期间才是使能的：

```
$ busybox devmem 0x04330018 32      # DMAC CHEN，通道 4/5 = SPI TX/RX
0x00000000  0x00000000  0x00000030  0x00000000      # 采样 6 次，抓到一次在传输中
```

所以 bus 上真正危险的不是「通道一直使能」，而是**复位恰好落在一次突发中间**：核没了，
DMAC 还在冲，AXI 就被拖死（实测 2026-11-06：写完 `stop` 整块板子挂死，USB 网卡都不回包）。

`PARK` 的五个动作各管一件事：

| 动作 | 管什么 | 省掉会怎样 |
|---|---|---|
| `parked` 标志 | 两个定时器回调不再碰页/LED | 无害，只是脏 |
| 挂起 app 线程 | 不再发起新的 SPI 传输 | 必然被重新武装 |
| 撤数据就绪中断 | 采样线程不再因中断重试 | 会在 drain 之后重新拉起传输 |
| `spi->Stop()` | SSI 停时钟、CS 释放 | 传输可能被卡在半路 |
| `SG200XDMAC::Quiesce()` | disable+abort 通道 **并等到真的 disabled** | 复位可能落在突发中间 |

也就是说：**前四步都是为了防止第五步刚做完就被重新武装，第五步才是不可省的那一步**。
而第五步的必要性只来自「partial reset」——整机复位（`reboot` / `poweroff`）会把 DMAC
一起复位，那条路径上这个窗口根本不存在：这就是删掉 `park` 之后 reboot 依然干净的原因。

`park` is not a shutdown protocol: it is the drain that makes a **partial reset** safe.
`rproc stop` resets the core but not the DMAC, and the owned channels are enabled only
during a transfer (see the CHEN sample above).  The hazard is a reset landing mid-burst:
no core, but the DMAC still mastering.  Of `park`'s five steps, the first four only keep
the fifth from being undone by a re-armed channel, and the fifth (`Quiesce`: disable and
*_wait_ until disabled*) is the one that matters.  Its necessity comes purely from the
partial reset; a full SoC reset resets the DMAC too, which is why a plain `reboot` is
clean without it.

## 因此不变量是 / So the invariant is

> **不做 partial reset。** 要么整机复位（`reboot` / `poweroff`），要么先 drain 再复位。

这台板子上没有第二个人会 drain（`park` 已删），所以镜像里**没有任何** `stop`：
`load-systemko` 的 drop-in 没有 `ExecStop`，`rtos-mode` 没有 `stop` 动词，
`deploy_rtos_fw.sh` 拒绝在核运行时停核并要求断电换固件。要恢复 partial reset，就得先把
drain 拿回来——放在固件里（按大核存活自停）或放在工具里（从 Linux 写 DMAC 的 CHEN/CHABORT
再等通道停稳），而不是加一句 `echo stop`。

**No partial reset.**  Either reset the whole SoC, or drain first.  Nobody else drains on
this board (`park` is gone), so the image has no `stop` anywhere; to bring a partial reset
back, bring the drain back -- in firmware or as an explicit Linux-side DMAC drain -- not as
a bare `echo stop`.

## 还留着一个硬件问题 / One hardware question stays

`poweroff` 如果只是把大核停住、SoC 仍然带电，那核会继续跑（继续采 SPI、继续驱动 PWM）。
这一点软件层无法回答，只能看板子的电源设计：

- 若 PMIC 在 `poweroff` 后断电 → 核一起掉，没有残留；
- 若保持带电 → 执行器需要**失力安全**（弹簧、刹车、失力保持），而不是指望关机前有人
  把它摆好。

镜像不提供关机钩子，契约里也没有可调用的停机命令：**失力就是安全状态**，这是产品包
必须满足的约束，不是可选项。

If `poweroff` only halts the big core while the SoC stays powered, the core keeps running
(SPI sampling, PWM).  Software cannot answer that; the board's power design does.  Either
the PMIC cuts power, or the actuators must be mechanically safe when de-energized.  The
image offers no shutdown hook and the contract has no stop command.

## 取证 / How to check it again

```sh
# 上一次启动里，核是什么时候起来的、有没有被停过
sudo journalctl -b -1 -k --no-pager | grep -iE 'remoteproc|c906'
# 现在这次
cat /sys/class/remoteproc/remoteproc*/state    # running / offline
cop send --contract /lib/firmware/guidance_contract.json --type 1   # 有回执 = 固件活着
```

如果哪天确实需要「优雅停机」，正确的位置在**固件**（例如按大核的存活信号自停），而不是
在 Linux 加一条会挂板的 `stop`。

If a graceful stop is ever needed, it belongs in the firmware (self-quiescing when the big
core goes away), not in a Linux-side `stop` that wedges the board.

## 实测记录：一次 reboot 卡住（2026-11-06）

在小核正在发布遥测时执行 `systemctl reboot`，出现了这样的状态：**Linux 的心跳灯灭了，
小核的灯还在闪**，网络消失、ssh 只能连上 TCP 就被关闭。也就是说大核已经不在，而小核还活着
——这是“小核生命周期 ⊆ 大核会话”被打破的最朴素形态，不是因为我们停核停坏了，而是因为
关机自己卡住了，于是没有任何人去处理它。

这一状态怎么恢复：断电重上电（已确认有效）。要查卡在哪一步，得先让关机日志留下来——
本镜像的 journal 默认不持久化，`/boot/bootlog` 也只有 `bootlogd` 在写；下次复现前先做
`mkdir -p /var/log/journal && systemd-tmpfiles --create --prefix /var/log/journal`。

Measured on 2026-11-06: a `systemctl reboot` issued while the core was publishing left Linux
gone (heartbeat LED off) and the core alive (its LED still blinking).  That is the plainest
form of "the small core outlived the big core" -- not because the stop was done wrong, but
because the shutdown itself hung and nobody was left to own the core.  Recovery is a power
cycle; finding where it hung needs a persistent journal, which this image does not have by
default.
