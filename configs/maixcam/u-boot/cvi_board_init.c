__attribute__((optimize("O0")))
// 1.26ms
void suck_loop(uint64_t loop) {
        loop = loop * 50 * 100;
        uint64_t a;
        while (loop > 0) {
                a = loop / (uint64_t)99;
                a = loop / (uint64_t)77;
                a = loop / (uint64_t)55;
                a = loop / (uint64_t)33;
                a = loop / (uint64_t)11;
                a = loop / (uint64_t)999;
                a = loop / (uint64_t)777;
                a = loop / (uint64_t)555;
                a = loop / (uint64_t)333;
                a = loop / (uint64_t)111;
                a = loop / (uint64_t)9999;
                a = loop / (uint64_t)7777;
                a = loop / (uint64_t)5555;
                a = loop / (uint64_t)3333;
                a = loop / (uint64_t)1111;
                a = loop / (uint64_t)99999;
                a = loop / (uint64_t)77777;
                a = loop / (uint64_t)55555;
                a = loop / (uint64_t)33333;
                a = loop / (uint64_t)11111;
                loop--;
        }
}

int cvi_board_init(void)
{
        uint32_t val;

        // LED 
        //mmio_write_32(0x03001038, 0x3); // SD0_PWR_EN/XGPIOA_14
        PINMUX_CONFIG(SD0_PWR_EN, XGPIOA_14);

        // 调试串口 UART1（GPIOA28 TX / GPIOA29 RX）：厂商默认是注释掉的，于是内核
        // console=ttyS1 一个字都出不来——这就是我们一直没有串口可看的原因。
        // Debug UART1 on GPIOA28/A29.  The vendor leaves this commented out, which is why
        // console=ttyS1 has never produced a single byte on this board.
        mmio_write_32(0x03001070, 0x1); // GPIOA 28 UART1 TX
        mmio_write_32(0x03001074, 0x1); // GPIOA 29 UART1 RX
        //mmio_write_32(0x03001068, 0x4); // GPIOA 18 UART1 CTS
        //mmio_write_32(0x03001064, 0x4); // GPIOA 19 UART1 RTS

        // PWM
        //mmio_write_32(0x03001068, 0x2); // GPIOA 18 PWM 6

        // lcd reset
        mmio_write_32(0x030010A4, 0x0); // PWRGPIO 0 GPIO_MODE

        // lcd backlight
        //mmio_write_32(0x030010EC, 0x0); // GPIOB 0 PWM0_BUCK
        mmio_write_32(0x030010EC, 0x3); // GPIOB 0 GPIO_MODE
        val = mmio_read_32(0x03021004); // GPIOB DIR
        val |= (1 << 0); // output
        mmio_write_32(0x03021004, val);
        val = mmio_read_32(0x03021000); // signal level
        val |= (1 << 0); // set level to high
        mmio_write_32(0x03021000, val);

        // camera function
        mmio_write_32(0x0300116C, 0x5); // RX4N CAM_MCLK0

        // camera/tp i2c
        mmio_write_32(0x03001090, 0x5); // PWR_GPIO6 IIC4_SCL
        mmio_write_32(0x03001098, 0x5); // PWR_GPIO8 IIC4_SDA

        // tp function
        mmio_write_32(0x03001084, 0x3); // PWR_SEQ1 PWR_GPIO[3]
        mmio_write_32(0x03001088, 0x3); // PWR_SEQ2 PWR_GPIO[4]
        mmio_write_32(0x05027078, 0x11);// Unlock PWR_GPIO[3]
        mmio_write_32(0x0502707c, 0x11);// Unlock PWR_GPIO[4]

        // bitbang i2c for maixcam
#ifdef MAIXCAM
        mmio_write_32(0x0300105C, 0x3);// GPIOA 23 GPIO_MODE
        mmio_write_32(0x03001060, 0x3);// GPIOA 24 GPIO_MODE
#endif
        // wait hardware bootup
        suck_loop(100);
        return 0;
}

/*
 * 交棒给内核之前，把两个舵面焊盘从 UART0 切到 PWM4/PWM5，并让它们输出"无脉冲"（≈0 占空）。
 *
 * 为什么必须在这里：这两个焊盘就是 **UART0_TX / UART0_RX**，而 UART 的空闲电平是**高**。
 * 从上电到固件接管（约 16 秒）之间，舵机/电调输入看到的是恒高，中间还夹着 FSBL 的 `RT:`
 * 输出和 u-boot 自己的启动日志（115200 的方波流）——**上电就满行程**就是这么来的。u-boot
 * 的 console 也在 UART0，所以不能更早，只能在 `misc_init_r`（bootcmd 之前）这一刻压掉。
 *
 * Before handing off to Linux, move the two surface pads from UART0 to PWM4/PWM5 and drive
 * them at ~0 duty.  Those pads *are* UART0_TX/RX and a UART idles **high**, so between
 * power-on and the firmware's bring-up (~16 s) a servo/ESC input sees a constant high --
 * with the FSBL's `RT:` output and u-boot's own boot log mixed in.  That is why the surface
 * goes to full travel at power-on.  u-boot's console is on UART0 too, so this cannot happen
 * earlier than `misc_init_r`, just before bootcmd.
 */
#define PWM1_BASE      0x03061000UL
#define PWM1_HL(ch)    (PWM1_BASE + (ch) * 8UL)
#define PWM1_PERIOD(ch) (PWM1_BASE + (ch) * 8UL + 4UL)
#define PWM1_POLARITY   (PWM1_BASE + 0x40UL)
#define PWM1_START      (PWM1_BASE + 0x44UL)
#define PWM1_UPDATE     (PWM1_BASE + 0x4CUL)
#define PWM1_OE         (PWM1_BASE + 0xD0UL)

/* 150 Hz @ 100 MHz → 666 667 ticks；inactive = 666 666 → 只高 1 tick，也就是没有脉冲。
   150 Hz @ 100 MHz -> 666,667 ticks; inactive = 666,666 leaves one tick high: no pulse. */
#define SERVO_PERIOD_TICKS   0xA2C2BUL
#define SERVO_INACTIVE_TICKS 0xA2C2AUL

static void servo_pads_safe_low(void)
{
	/* PWM4/PWM5 是 pwm1 的本地通道 0/1。顺序照 LL 驱动：先写周期，再提交、再启动，
	 * 最后才切焊盘——这样引脚上不会出现半个周期的怪电平。
	 * PWM4/PWM5 are pwm1's local channels 0/1.  Same order as the firmware's LL driver:
	 * period first, then commit, then start, and the pad last, so the pin never shows a
	 * half-programmed level. */
	mmio_write_32(PWM1_HL(0), SERVO_INACTIVE_TICKS);
	mmio_write_32(PWM1_PERIOD(0), SERVO_PERIOD_TICKS);
	mmio_write_32(PWM1_HL(1), SERVO_INACTIVE_TICKS);
	mmio_write_32(PWM1_PERIOD(1), SERVO_PERIOD_TICKS);
	mmio_write_32(PWM1_POLARITY, 0x0);	/* active high */
	mmio_write_32(PWM1_UPDATE, 0x3);	/* commit channels 0/1 */
	mmio_write_32(PWM1_UPDATE, 0x0);
	mmio_write_32(PWM1_START, 0x0);
	mmio_write_32(PWM1_OE, 0x3);
	mmio_write_32(PWM1_START, 0x3);

	/* UART0_TX → PWM4 (function 2)、UART0_RX → PWM5 (function 2) */
	mmio_write_32(0x03001040, 0x2);
	mmio_write_32(0x03001044, 0x2);
}

int misc_init_r(void)
{
	servo_pads_safe_low();
	return 0;
}
