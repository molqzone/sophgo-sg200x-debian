# C906L 资源所有权：改设备树，而不是让固件去抢

**镜像仓**：`sbc-linux/sophgo-sg200x-debian`，部署板是 `maixcam`（`maixcam-sc035hgs`
继承它）。这份文档记录固件侧已经依赖的 DT 前提，以及仍然缺的那一处声明。

## 已经声明好的（不需要再改）

- **共享页 / console 页**：`reserved-memory` + 链接脚本钉位。两块 4 KiB 都在
  `fast_image` 之后的保留区里。
- **DMAC 通道**：`configs/maixcam/dts/cv181x_licheervnano_sd.dts` 已经写了

  ```dts
  &dmac {
      /* Linux allocates channels 0-3; C906L owns channels 4-7. */
      dma-channels = /bits/ 8 <4>;
  };
  ```

  这是**按通道划分所有权**，不是关掉控制器：Linux 的 `spinand`/`i2s` 仍可用 0–3，
  固件只用 4–7（`SG200XDMAC::OWNED_CHANNEL_MASK`），并且只改自己那些通道的
  DMAMUX 路由与中断清位。
- **RPMsg 残留**：`vdev0vring0/1`、`vdev0buffer` 已经从这块板的 DT 删除——固件没有
  resource table，走的是 mailbox-only。
- **启动顺序**：`components/guidance-fw` 的 `rtos-mode` 在 systemd 里启动 C906L，
  也就是在内核驱动 probe 之后。这正是固件可以不再等「Linux 释放 DMA」的原因。

## 仍然缺的一处：SPI2

`spi2@041A0000` 在 `cv181x_base.dtsi` 里默认 enabled，而这颗 SPI 就是 C906L 上的
IMU 总线；本板的 Linux 侧没有任何 SPI2 客户端。开着它只会让内核的 `dw-apb-ssi`
probe 并接管 SPI2 的复位与时钟，和远端核抢同一个控制器。

这条已落在镜像仓的 `configs/maixcam/dts/cv181x_licheervnano_sd.dts` 里：

```dts
&spi2 {
	status = "disabled";
};
```

改完之后，SPI2 只有一个主人，固件的 `BoardMsp::Init()`（`Core/sg200x_board_init.cpp`：
DMAC 静默 → 时钟门 → SPI2 复位 → 焊盘）就是普通的「给自己的外设上电」，不再是抢。

## 固件侧因此删掉了什么

| 旧 | 现在 |
|---|---|
| `WaitForLinuxReady()`：等 20 s doorbell 才敢碰 DMA | 已删除 |
| 交接脚本 / 运行时 unbind / pinmux 读回探测 | 已删除 |
| 延后线程的语义 | 变成普通 bring-up 线程（只为栈深保留） |

## 部署顺序与前提

固件假设上面的 DT 与启动顺序；两者是同一个变更集。若将来改成 **FSBL 直接启动
C906L**（早于内核 probe），就得把「等 DMAC」加回来，或者把整台 DMAC 判给 C906L
（`&dmac { status = "disabled"; }`，同时接受 Linux 的 `spinand`/`i2s` 失去 DMA）。

## 验证

```sh
# 内核不再接管 SPI2
dmesg | grep -i 'dw-apb-ssi'        # 不应出现 spi2@041A0000
ls /sys/bus/platform/devices/ | grep 041A0000

# DMAC 仍按通道划分：Linux 只有 4 个通道
cat /sys/class/dma/dma*/? 2>/dev/null
```

板上再看 `guidance-cmd.py ping`（门铃/命令通道）与 `guidance-cmd.py log --new`（console 页；
该脚本已同步到 `WIRE_TAG` 6：`Sample` 48 B、帧头带 CRC32，console 读 `0x8FFDB000` 页）。
