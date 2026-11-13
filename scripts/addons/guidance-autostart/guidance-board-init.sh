#!/bin/sh
# 从大核配置小核要用的时钟门和焊盘 / Configure the little core's clocks and pads from the
# big core.
#
# 为什么不在小核里做：CLKGEN 的每个 32 位字里装着几十个门（SD 卡、USB 的也在里面），
# PINMUX 又是按引脚共享的。小核没有 MMU、这段地址在 SYSMAP 里可缓存，一次读-改-写会把
# 整条 64 字节缓存行带进来，之后驱逐时**整行**写回，把邻居寄存器里已过期的值一起覆盖到
# 硬件上——内核期间改过其中任何一个就会被打断，大核等 I/O 等到死且没有 oops。
# 大核的访问是一致的，所以这些共享寄存器由大核来写。
# Why not from the little core: each CLKGEN 32-bit word holds dozens of gates (the SD card's
# and USB's among them) and PINMUX is shared per pin.  That core has no MMU and the window is
# SYSMAP-cacheable, so a read-modify-write pulls in a whole 64-byte line and the eviction
# writes all of it back, overwriting neighbours that went stale -- if the kernel changed any
# of them meanwhile, it is interrupted and the big core waits on I/O forever with no oops.
# The big core's accesses are coherent, so it writes these registers.
#
# 地址来自固件侧的表（`sg200x_ll_rcc.h` / `sg2002.h` / `Core/sg200x_board_init.cpp`）：
# CLKGEN 基址 0x03002000，PINMUX 基址 0x03001000。
# Addresses come from the firmware's tables: CLKGEN at 0x03002000, PINMUX at 0x03001000.
set -eu

CLKGEN=0x03002000
PINMUX=0x03001000

# field_set <addr> <shift> <width> <value>
field_set() {
    addr=$1
    shift=$2
    width=$3
    value=$4
    current=$(devmem "$addr" 32)
    mask=$(( ((1 << width) - 1) << shift ))
    updated=$(( (current & ~mask) | ((value << shift) & mask) ))
    devmem "$addr" 32 "$updated"
    after=$(devmem "$addr" 32)
    if [ "$(( (after >> shift) & ((1 << width) - 1) ))" != "$value" ]; then
        echo "guidance-board-init: $addr[$shift:$width] did not take ($after)" >&2
        exit 1
    fi
}

# 时钟门 / Clock gates: APB_PWM (CLK_EN1 b8), APB_SPI2 (CLK_EN1 b11),
# SPI (CLK_EN3 b6), Timer4 (CLK_EN3 b13).
field_set $((CLKGEN + 0x004)) 8 1 1
field_set $((CLKGEN + 0x004)) 11 1 1
field_set $((CLKGEN + 0x00C)) 6 1 1
field_set $((CLKGEN + 0x00C)) 13 1 1

# 焊盘 / Pads: UART0 -> PWM4/5, SD1 -> SPI2, MIPI -> PWM14/15.
field_set $((PINMUX + 0x040)) 0 3 2
field_set $((PINMUX + 0x044)) 0 3 2
field_set $((PINMUX + 0x0DC)) 0 3 1
field_set $((PINMUX + 0x0E0)) 0 3 1
field_set $((PINMUX + 0x0E4)) 0 3 1
field_set $((PINMUX + 0x1B4)) 0 3 5
field_set $((PINMUX + 0x1B8)) 0 3 5

echo "guidance-board-init: clocks and pads configured"
