SIZE_1M = 0x100000
SIZE_1K = 1024


# Only attributes in class MemoryMap are generated to .h
class MemoryMap:
    # No prefix "CVIMMAP_" for the items in _no_prefix[]
    _no_prefix = [
        "CONFIG_SYS_TEXT_BASE"  # u-boot's CONFIG_SYS_TEXT_BASE is used without CPP.
    ]

    DRAM_BASE = 0x80000000
    DRAM_SIZE = 256 * SIZE_1M

    # ==============
    # C906L FreeRTOS
    # ==============
    FREERTOS_SIZE = 2 * SIZE_1M
    # FreeRTOS is at the end of DRAM
    FREERTOS_ADDR = DRAM_BASE + DRAM_SIZE - FREERTOS_SIZE
    FSBL_C906L_START_ADDR = FREERTOS_ADDR

    # Layout of the 2 MiB band, bottom to top: firmware image, the 132 KiB vendor
    # window, then the boot-trace page.
    #
    # 这 132 KiB **不是**“相机环”。它是 `rtos_cmdqu` 节点的 `shared-memory` 资源
    # （`reg-names = "mailbox","shared-memory"`），也就是 Linux 与 C906L 之间命令队列的
    # 窗口。相机桥（sg2002-ipc）退役时曾把它当成相机环并入 fast_image，结果板子再也进不
    # 了系统（实测：基线 + 只改这一组内存映射 → 起不来；基线 + uio 节点 → 正常启动）。
    # 退役相机桥不等于这块保留区可以消失，它必须保持独立、no-map、并且带自己的 reg。
    #
    # This 132 KiB is not the camera ring: it is the `rtos_cmdqu` node's `shared-memory`
    # resource, the command-queue window between Linux and the C906L.  Folding it into
    # fast_image when the camera bridge was retired stopped the board from booting.
    RTOS_BOOT_TRACE_SIZE = 4 * SIZE_1K
    IPC_SHARED_SIZE = 132 * SIZE_1K
    RTOS_FIRMWARE_SIZE = FREERTOS_SIZE - RTOS_BOOT_TRACE_SIZE - IPC_SHARED_SIZE
    IPC_SHARED_ADDR = FREERTOS_ADDR + RTOS_FIRMWARE_SIZE
    RTOS_BOOT_TRACE_ADDR = IPC_SHARED_ADDR + IPC_SHARED_SIZE
    assert RTOS_BOOT_TRACE_ADDR + RTOS_BOOT_TRACE_SIZE == (
        FREERTOS_ADDR + FREERTOS_SIZE
    )

    # 两个页的物理地址是与固件握手的那一个事实：镜像侧在这里声明一次，固件构建用
    # SG200X_GUIDANCE_PAGE_BASE 把它拿走（同一个值生成链接脚本、C++ 编译定义与发布的
    # guidance_contract.json），固件自己不推导。scripts/ci/validate.py 强制三处相等：
    # 这两个数、DTS `guidance-pages` 的 reg、以及 guidance-fw 发布的契约 JSON。
    # The two page addresses are the single fact shared with the firmware.  The image
    # declares them here; the firmware build is handed the value
    # (SG200X_GUIDANCE_PAGE_BASE) and derives nothing itself, and validate.py fails the
    # build unless these numbers, the DTS `guidance-pages` reg and the published
    # guidance_contract.json all agree.
    GUIDANCE_PAGE_SIZE = 4 * SIZE_1K
    GUIDANCE_SHARED_PAGE_ADDR = 0x8FFDC000
    GUIDANCE_CONSOLE_PAGE_ADDR = GUIDANCE_SHARED_PAGE_ADDR - GUIDANCE_PAGE_SIZE
    assert GUIDANCE_CONSOLE_PAGE_ADDR + GUIDANCE_PAGE_SIZE == GUIDANCE_SHARED_PAGE_ADDR

    # ==============================
    # OpenSBI | arm-trusted-firmware
    # ==============================
    # Monitor is at the begining of DRAM
    MONITOR_ADDR = DRAM_BASE

    ATF_SIZE = 512 * SIZE_1K
    OPENSBI_SIZE = 512 * SIZE_1K
    OPENSBI_FDT_ADDR = MONITOR_ADDR + OPENSBI_SIZE

    # =========================
    # memory@DRAM_BASE in .dts.
    # =========================
    # Ignore the area of FreeRTOS in u-boot and kernel
    KERNEL_MEMORY_ADDR = DRAM_BASE
    KERNEL_MEMORY_SIZE = DRAM_SIZE - FREERTOS_SIZE

    # =================
    # Multimedia buffer. Used by u-boot/kernel/FreeRTOS
    # =================
    # Headless camera route is using 720p-oriented middleware defaults, but
    # the vendor ISP path still needs some headroom above the 22 MiB floor.
    ION_SIZE = 28 * SIZE_1M
    H26X_BITSTREAM_SIZE = 2 * SIZE_1M
    H26X_ENC_BUFF_SIZE = 0
    ISP_MEM_BASE_SIZE = 20 * SIZE_1M
    FREERTOS_RESERVED_ION_SIZE = H26X_BITSTREAM_SIZE + H26X_ENC_BUFF_SIZE + ISP_MEM_BASE_SIZE

    # ION after FreeRTOS
    ION_ADDR = FREERTOS_ADDR - ION_SIZE

    # Buffers of the fast image are inside the ION buffer
    H26X_BITSTREAM_ADDR = ION_ADDR
    H26X_ENC_BUFF_ADDR = H26X_BITSTREAM_ADDR + H26X_BITSTREAM_SIZE
    ISP_MEM_BASE_ADDR = H26X_ENC_BUFF_ADDR + H26X_ENC_BUFF_SIZE

    assert ISP_MEM_BASE_ADDR + ISP_MEM_BASE_SIZE <= ION_ADDR + ION_SIZE

    # Headless default: do not reserve a framebuffer/bootlogo carveout.
    BOOTLOGO_SIZE = 0
    BOOTLOGO_ADDR = ION_ADDR - BOOTLOGO_SIZE
    FRAMEBUFFER_SIZE = BOOTLOGO_SIZE
    FRAMEBUFFER_ADDR = BOOTLOGO_ADDR

    # ===================
    # FSBL and u-boot-2021
    # ===================
    CVI_UPDATE_HEADER_SIZE = SIZE_1K
    UIMAG_SIZE = 16 * SIZE_1M

    # kernel image loading buffer
    UIMAG_ADDR = DRAM_BASE + 24 * SIZE_1M
    CVI_UPDATE_HEADER_ADDR = UIMAG_ADDR - CVI_UPDATE_HEADER_SIZE

    # FSBL decompress buffer
    FSBL_UNZIP_ADDR = UIMAG_ADDR
    FSBL_UNZIP_SIZE = UIMAG_SIZE

    assert UIMAG_ADDR + UIMAG_SIZE <= BOOTLOGO_ADDR

    # u-boot's run address and entry point
    CONFIG_SYS_TEXT_BASE = DRAM_BASE + 2 * SIZE_1M
    # u-boot's init stack point is only used before board_init_f()
    CONFIG_SYS_INIT_SP_ADDR = UIMAG_ADDR + UIMAG_SIZE
