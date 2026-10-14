/*
 * drv_st7789.c - ST7789 240x240 SPI LCD 驱动（RT-Thread SPI 框架）
 *
 * 挂在 SPI4 总线上（EVAL 板 JP30 排针：SCK=PH6/MOSI=PF9/CS=PF6），
 * DC/RES 用普通 GPIO（默认 PA10/PA9，Kconfig 可改），背光 BLK 接地常亮
 * 或接 PG13（PWM 调光留作将来）。
 *
 * ST7789 只收不发：全程写，不读 MISO。
 * 显示链路：开窗(2A/2B) -> 写显存(2C) -> 连续吐 RGB565 字节流。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#if defined(BSP_USING_SPI4)

#define DBG_TAG             "drv.st7789"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define LCD_W               240
#define LCD_H               240

/* 引脚定义（Kconfig 配置项给默认值） */
/* 注意：PA9/PA10 是控制台 USART0，千万别用（血泪教训：一碰 pin 输出全断）。
 * DC=PB5（CAN1_RX，本篇 CAN 关闭）；RES=PB4（JNTRST，SWD 调试用不到）。
 * RES 脚不够时可接 3.3V：定义 BSP_ST7789_NO_RES 宏即可跳过硬件复位，
 * 靠初始化序列里的 SWRESET 软复位兜底。 */
#ifndef BSP_ST7789_DC_PIN
#define BSP_ST7789_DC_PIN   GET_PIN(B, 5)
#endif
#ifndef BSP_ST7789_RES_PIN
#define BSP_ST7789_RES_PIN  GET_PIN(B, 4)
#endif
#ifndef BSP_ST7789_SPI_BUS
#define BSP_ST7789_SPI_BUS  "spi4"
#endif

/* 速率纪律：杜邦飞线先按 8MHz 验证内容通路（官方 demo 的 37.5MHz
 * 是焊在板上的短走线待遇，飞线没这个命）。点亮后逐级上调摸底，
 * 摸到花屏临界值再退回一档——这个摸底过程本身就是博客素材。 */
#ifndef LCD_SPI_MAX_HZ
#define LCD_SPI_MAX_HZ      8000000
#endif

static struct rt_spi_device lcd_spi_dev;    /* cspin 版 attach 由调用方出设备对象 */
static struct rt_spi_device *lcd_spi = &lcd_spi_dev;

/* ---------- GPIO 位 bang 通道（终极对照实验） ----------
 * 把 SCK(PH6)/SDA(PF9)/CS(PF6) 掰成普通 IO，软件逐位模拟 SPI mode 0，
 * 速率 ~几百 kHz：信号完整性、外设时序等一切硬件因素全部免疫。
 * 与卖家的 8051 参考代码工作方式完全一致。
 * 位 bang 能点亮 = SPI 外设波形在模组端有鬼；位 bang 也黑 = 模组坏死。 */
#define BB_SCK_PIN  GET_PIN(H, 6)
#define BB_SDA_PIN  GET_PIN(F, 9)
#define BB_CS_PIN   GET_PIN(F, 6)

static rt_bool_t lcd_bb_mode = RT_FALSE;

static void bb_delay(void)
{
    volatile rt_uint32_t i;
    for (i = 0; i < 800; i++) ;         /* ~1µs @600MHz，位速率 ~250kHz */
}

static void bb_write_byte(rt_uint8_t v)
{
    rt_uint32_t i;
    for (i = 0; i < 8; i++)
    {
        rt_pin_write(BB_SCK_PIN, PIN_LOW);
        rt_pin_write(BB_SDA_PIN, (v & 0x80) ? PIN_HIGH : PIN_LOW);
        bb_delay();
        rt_pin_write(BB_SCK_PIN, PIN_HIGH);     /* 上升沿，屏在此采样 */
        bb_delay();
        v <<= 1;
    }
}

static void bb_gpio_take(void)
{
    rt_pin_mode(BB_SCK_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(BB_SDA_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(BB_CS_PIN,  PIN_MODE_OUTPUT);
    rt_pin_write(BB_CS_PIN,  PIN_HIGH);
    rt_pin_write(BB_SCK_PIN, PIN_LOW);
}

static void bb_write_cmd(rt_uint8_t cmd)
{
    rt_pin_write(BB_CS_PIN, PIN_LOW);
    rt_pin_write(BSP_ST7789_DC_PIN, PIN_LOW);
    bb_write_byte(cmd);
    rt_pin_write(BB_CS_PIN, PIN_HIGH);
}

static void bb_write_data(const rt_uint8_t *data, rt_size_t len)
{
    rt_pin_write(BB_CS_PIN, PIN_LOW);
    rt_pin_write(BSP_ST7789_DC_PIN, PIN_HIGH);
    while (len--) bb_write_byte(*data++);
    rt_pin_write(BB_CS_PIN, PIN_HIGH);
}

/* ---------- 手动 CS 通道（CS 沿干扰鉴别实验） ----------
 * 硬件 SPI 数据通路不变，唯一变量是 CS：
 *   框架 CS：拉低后 ns 级就启动时钟，翻转沿是 60MHz 陡沿；
 *   手动 CS：拉低后给 ~3µs 护栏再发数据，发完 ~3µs 再释放。
 * 若手动 CS 能点亮而框架 CS 不能 = CS 沿耦合/毛刺实锤
 * （30cm 线上 CS 陡沿串进 SCK，屏每 CS 周期多采一位）。
 * hold 模式更进一步：整段初始化 CS 一直拉低（ST7789 允许，
 * 靠 D/C 区分命令/数据），CS 沿减到最少 —— 只亮 hold = CS 沿石锤。 */
#define LCD_CS_PIN  GET_PIN(F, 6)

static rt_bool_t lcd_rawcs_mode = RT_FALSE;
static rt_bool_t lcd_rawcs_hold = RT_FALSE;
static rt_bool_t lcd_rawcs_pre  = RT_TRUE;   /* pre 护栏：CS 低 -> 延时 -> 时钟 */
static rt_bool_t lcd_rawcs_post = RT_TRUE;   /* post 护栏：时钟完 -> 延时 -> CS 高 */

static void rawcs_delay(void)
{
    volatile rt_uint32_t i;
    for (i = 0; i < 2000; i++) ;        /* ~3µs @600MHz */
}

/* ---------- 底层：DC 拉低发命令 / 拉高发数据 ----------
 * CS 由本驱动自管（PF6 当普通 GPIO），不走框架的 cspin：
 * 框架在 CS 有效后 ns 级就启动时钟、末位时钟后 ns 级就释放 CS，
 * 飞线场景下 CS 与 SCK/MOSI 的到达偏差会吃掉建立/保持时间，
 * 现象是"背光亮、全黑、任何频率都黑"。
 * 修复：CS 前后各加 ~3µs 护栏（实验验证 pre/post 任一即可，两个都留
 * 是为了全温区/全批次的裕量；每次传输仅 +6µs，对帧率影响可忽略）。 */
static void lcd_write_cmd(rt_uint8_t cmd)
{
    if (lcd_bb_mode) { bb_write_cmd(cmd); return; }
    if (lcd_rawcs_mode)
    {
        if (!lcd_rawcs_hold)
        {
            rt_pin_write(LCD_CS_PIN, PIN_LOW);
            if (lcd_rawcs_pre) rawcs_delay();
        }
        rt_pin_write(BSP_ST7789_DC_PIN, PIN_LOW);
        rt_spi_send(lcd_spi, &cmd, 1);
        if (!lcd_rawcs_hold)
        {
            if (lcd_rawcs_post) rawcs_delay();
            rt_pin_write(LCD_CS_PIN, PIN_HIGH);
        }
        return;
    }
    rt_pin_write(LCD_CS_PIN, PIN_LOW);
    rawcs_delay();
    rt_pin_write(BSP_ST7789_DC_PIN, PIN_LOW);
    rt_spi_send(lcd_spi, &cmd, 1);
    rawcs_delay();
    rt_pin_write(LCD_CS_PIN, PIN_HIGH);
}

static void lcd_write_data(const rt_uint8_t *data, rt_size_t len)
{
    if (lcd_bb_mode) { bb_write_data(data, len); return; }
    if (lcd_rawcs_mode)
    {
        if (!lcd_rawcs_hold)
        {
            rt_pin_write(LCD_CS_PIN, PIN_LOW);
            if (lcd_rawcs_pre) rawcs_delay();
        }
        rt_pin_write(BSP_ST7789_DC_PIN, PIN_HIGH);
        rt_spi_send(lcd_spi, data, len);
        if (!lcd_rawcs_hold)
        {
            if (lcd_rawcs_post) rawcs_delay();
            rt_pin_write(LCD_CS_PIN, PIN_HIGH);
        }
        return;
    }
    rt_pin_write(LCD_CS_PIN, PIN_LOW);
    rawcs_delay();
    rt_pin_write(BSP_ST7789_DC_PIN, PIN_HIGH);
    rt_spi_send(lcd_spi, data, len);
    rawcs_delay();
    rt_pin_write(LCD_CS_PIN, PIN_HIGH);
}

static void lcd_write_u8(rt_uint8_t v) { lcd_write_data(&v, 1); }

/* ---------- 硬件复位（无 RES 脚时跳过，靠 SWRESET 命令兜底） ---------- */
static void lcd_hw_reset(void)
{
#ifndef BSP_ST7789_NO_RES
    rt_pin_write(BSP_ST7789_RES_PIN, PIN_HIGH);
    rt_thread_mdelay(10);
    rt_pin_write(BSP_ST7789_RES_PIN, PIN_LOW);
    rt_thread_mdelay(20);       /* RES 低脉冲 >= 10us，给足 */
    rt_pin_write(BSP_ST7789_RES_PIN, PIN_HIGH);
#endif
    rt_thread_mdelay(120);      /* 复位后 120ms 内不许发命令 */
}

/* ---------- 初始化序列 ---------- */
static void lcd_init_seq(void)
{
    lcd_hw_reset();

    lcd_write_cmd(0x01);            /* SWRESET 软复位 */
    rt_thread_mdelay(150);

    lcd_write_cmd(0x11);            /* SLPOUT 退出睡眠 */
    rt_thread_mdelay(120);

    lcd_write_cmd(0x3A);            /* COLMOD：RGB565 */
    lcd_write_u8(0x55);

    lcd_write_cmd(0x36);            /* MADCTL：扫描方向 0度，RGB 顺序 */
    lcd_write_u8(0x00);

    /* 240x240 无行/列偏移（若是 240x280 的屏芯才需要 offset） */
    lcd_write_cmd(0xB2);            /* PORCTRL  porch 设置 */
    { rt_uint8_t d[] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xB7);            /* GCTRL 门控电压 */
    lcd_write_u8(0x35);

    lcd_write_cmd(0xBB);            /* VCOMS */
    lcd_write_u8(0x19);

    lcd_write_cmd(0xC0);            /* LCMCTRL */
    lcd_write_u8(0x2C);

    lcd_write_cmd(0xC2);            /* VDVVRHEN */
    lcd_write_u8(0x01);

    lcd_write_cmd(0xC3);            /* VRHS */
    lcd_write_u8(0x12);

    lcd_write_cmd(0xC4);            /* VDVS */
    lcd_write_u8(0x20);

    lcd_write_cmd(0xC6);            /* FRCTRL2：60Hz */
    lcd_write_u8(0x0F);

    lcd_write_cmd(0xD0);            /* PWCTRL1 */
    { rt_uint8_t d[] = {0xA4, 0xA1}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xE0);            /* PVGAMCTRL 正极性 gamma */
    { rt_uint8_t d[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                        0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
      lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xE1);            /* NVGAMCTRL 负极性 gamma */
    { rt_uint8_t d[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                        0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
      lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0x21);            /* INVON 反显（ST7789 必须开） */

    lcd_write_cmd(0x29);            /* DISPON 开显示 */
    rt_thread_mdelay(20);
}

/* ---------- GC9A01 变体初始化 ----------
 * 240x240 无 knob 的圆屏/方屏模组大量混用 GC9A01，外观、引脚、
 * 初始化风格与 ST7789 几乎一样，卖家混发是常态。
 * 两者的关键差异：GC9A01 有一大段私有寄存器（0xEF/0xEB/0x84~0x8F/
 * 0x90/0xC3/0xC4/0x62~0x67/0x74...）必须按序配置，否则永远黑屏；
 * 而它对 ST7789 的私有寄存器写入则直接忽略 —— 所以用 ST7789 序列
 * 初始化 GC9A01 的现象就是"背光亮、全黑、无噪点"，与本案完全吻合。 */
static void lcd_init_seq_gc9a01(void)
{
    lcd_hw_reset();

    lcd_write_cmd(0xEF);
    lcd_write_cmd(0xEB); lcd_write_u8(0x14);

    lcd_write_cmd(0xFE);            /* 解锁内部寄存器组 */
    lcd_write_cmd(0xEF);

    lcd_write_cmd(0xEB); lcd_write_u8(0x14);

    lcd_write_cmd(0x84); lcd_write_u8(0x40);
    lcd_write_cmd(0x85); lcd_write_u8(0xFF);
    lcd_write_cmd(0x86); lcd_write_u8(0xFF);
    lcd_write_cmd(0x87); lcd_write_u8(0xFF);
    lcd_write_cmd(0x88); lcd_write_u8(0x0A);
    lcd_write_cmd(0x89); lcd_write_u8(0x21);
    lcd_write_cmd(0x8A); lcd_write_u8(0x00);
    lcd_write_cmd(0x8B); lcd_write_u8(0x80);
    lcd_write_cmd(0x8C); lcd_write_u8(0x01);
    lcd_write_cmd(0x8D); lcd_write_u8(0x01);
    lcd_write_cmd(0x8E); lcd_write_u8(0xFF);
    lcd_write_cmd(0x8F); lcd_write_u8(0xFF);

    lcd_write_cmd(0xB6);            /* 显示功能控制 */
    { rt_uint8_t d[] = {0x00, 0x20}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0x36);            /* MADCTL */
    lcd_write_u8(0x48);             /* MX + BGR（圆屏常用；颜色不对再调） */

    lcd_write_cmd(0x3A);            /* COLMOD：RGB565 */
    lcd_write_u8(0x05);

    lcd_write_cmd(0x90);
    { rt_uint8_t d[] = {0x08, 0x08, 0x08, 0x08}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xBD); lcd_write_u8(0x06);
    lcd_write_cmd(0xBC); lcd_write_u8(0x00);

    lcd_write_cmd(0xFF);
    { rt_uint8_t d[] = {0x60, 0x01, 0x04}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xC3); lcd_write_u8(0x13);
    lcd_write_cmd(0xC4); lcd_write_u8(0x13);
    lcd_write_cmd(0xC9); lcd_write_u8(0x22);
    lcd_write_cmd(0xBE); lcd_write_u8(0x11);
    lcd_write_cmd(0xE1);
    { rt_uint8_t d[] = {0x10, 0x0E}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0xDF);
    { rt_uint8_t d[] = {0x21, 0x0C, 0x02}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xF0);            /* gamma */
    { rt_uint8_t d[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0xF1);
    { rt_uint8_t d[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0xF2);
    { rt_uint8_t d[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0xF3);
    { rt_uint8_t d[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xED);
    { rt_uint8_t d[] = {0x1B, 0x0B}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0xAE); lcd_write_u8(0x77);
    lcd_write_cmd(0xCD); lcd_write_u8(0x63);

    lcd_write_cmd(0x70);
    { rt_uint8_t d[] = {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03};
      lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0xE8); lcd_write_u8(0x34);

    lcd_write_cmd(0x62);
    { rt_uint8_t d[] = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0x63);
    { rt_uint8_t d[] = {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70}; lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0x64);
    { rt_uint8_t d[] = {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0x66);
    { rt_uint8_t d[] = {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00};
      lcd_write_data(d, sizeof(d)); }
    lcd_write_cmd(0x67);
    { rt_uint8_t d[] = {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98};
      lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0x74);
    { rt_uint8_t d[] = {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0x98);
    { rt_uint8_t d[] = {0x3E, 0x07}; lcd_write_data(d, sizeof(d)); }

    lcd_write_cmd(0x35);            /* TEON */
    lcd_write_cmd(0x21);            /* INVON */

    lcd_write_cmd(0x11);            /* SLPOUT */
    rt_thread_mdelay(120);
    lcd_write_cmd(0x29);            /* DISPON */
    rt_thread_mdelay(20);
}

/* ---------- 开窗 + 写显存 ---------- */
static void lcd_set_window(rt_uint16_t x0, rt_uint16_t y0, rt_uint16_t x1, rt_uint16_t y1)
{
    rt_uint8_t d[4];

    lcd_write_cmd(0x2A);            /* CASET 列地址 */
    d[0] = x0 >> 8; d[1] = x0 & 0xFF; d[2] = x1 >> 8; d[3] = x1 & 0xFF;
    lcd_write_data(d, 4);

    lcd_write_cmd(0x2B);            /* RASET 行地址 */
    d[0] = y0 >> 8; d[1] = y0 & 0xFF; d[2] = y1 >> 8; d[3] = y1 & 0xFF;
    lcd_write_data(d, 4);

    lcd_write_cmd(0x2C);            /* RAMWR 开始写显存 */
}

/**
 * 把一整块 RGB565 像素缓冲推进窗口（摄像头刷帧的主通道）
 * pixels: 显存数据，长度 = (x1-x0+1)*(y1-y0+1)*2 字节
 */
void st7789_blit(rt_uint16_t x0, rt_uint16_t y0, rt_uint16_t x1, rt_uint16_t y1,
                 const rt_uint8_t *pixels)
{
    rt_uint32_t len = (rt_uint32_t)(x1 - x0 + 1) * (y1 - y0 + 1) * 2;
    lcd_set_window(x0, y0, x1, y1);
    lcd_write_data(pixels, len);
}

void st7789_fill(rt_uint16_t color)
{
    rt_uint8_t hi = color >> 8, lo = color & 0xFF;
    rt_uint8_t buf[64];
    int i;

    for (i = 0; i < (int)sizeof(buf); i += 2) { buf[i] = hi; buf[i + 1] = lo; }

    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);

    if (lcd_bb_mode)
    {
        for (i = 0; i < LCD_W * LCD_H * 2 / (int)sizeof(buf); i++)
            bb_write_data(buf, sizeof(buf));
        return;
    }
    /* 像素段也走手动 CS：整帧一对护栏，比每 64B 一对省 ~11ms/帧 */
    rt_pin_write(LCD_CS_PIN, PIN_LOW);
    rawcs_delay();
    rt_pin_write(BSP_ST7789_DC_PIN, PIN_HIGH);
    for (i = 0; i < LCD_W * LCD_H * 2 / (int)sizeof(buf); i++)
        rt_spi_send(lcd_spi, buf, sizeof(buf));
    rawcs_delay();
    rt_pin_write(LCD_CS_PIN, PIN_HIGH);
}

/* config 缓存坑的解法：同配置调 rt_spi_configure 会被框架跳过，
 * 驱动里"重夺引脚"那步就不执行 —— 而 boot 期有个惯偷会偷 SPI4 引脚。
 * 先用哑速率污染缓存再用真速率，迫使 ops->configure 必然执行。 */
static void lcd_force_configure(rt_uint32_t hz)
{
    struct rt_spi_configuration cfg;

    cfg.data_width = 8;
    cfg.mode = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz = hz / 2;
    rt_spi_configure(lcd_spi, &cfg);        /* 污染缓存 */
    cfg.max_hz = hz;
    rt_spi_configure(lcd_spi, &cfg);        /* 真配置，必跑，夺回引脚 */
}

/* ---------- 注册 ---------- */
static int rt_hw_st7789_init(void)
{
    rt_pin_mode(BSP_ST7789_DC_PIN, PIN_MODE_OUTPUT);
#ifndef BSP_ST7789_NO_RES
    rt_pin_mode(BSP_ST7789_RES_PIN, PIN_MODE_OUTPUT);
#endif
    /* CS 由本驱动自管（见 lcd_write_cmd 注释），先拉高 */
    rt_pin_mode(LCD_CS_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(LCD_CS_PIN, PIN_HIGH);

    /* 挂到 SPI4 总线。cspin 给一个哑引脚（PE7，板上未用）：
     * 真 CS 由驱动自管，框架只需要一个占位对象 */
    if (rt_spi_bus_attach_device_cspin(&lcd_spi_dev, "lcd0", BSP_ST7789_SPI_BUS,
                                       GET_PIN(E, 7), RT_NULL) != RT_EOK)
    {
        LOG_E("attach lcd0 to %s failed (spi bus registered?)", BSP_ST7789_SPI_BUS);
        return -RT_ERROR;
    }

    lcd_force_configure(LCD_SPI_MAX_HZ);

    lcd_init_seq();
    st7789_fill(0x0000);            /* 黑屏 */

    LOG_I("st7789 240x240 on %s ready", BSP_ST7789_SPI_BUS);
    return RT_EOK;
}
INIT_APP_EXPORT(rt_hw_st7789_init);
/* 手动触发（取证期）：先让系统进 msh，再 lcd_init 逐步观察。
 * 定位完成后恢复 INIT_APP_EXPORT。
 * lcd_init     -> ST7789 序列；lcd_init gc -> GC9A01 变体序列 */
static void lcd_init(int argc, char **argv)
{
    rt_bool_t use_gc = RT_FALSE;
    rt_uint32_t khz = LCD_SPI_MAX_HZ / 1000;
    int i;

    /* 参数扫描：含 'g' -> GC9A01 序列；纯数字 -> SPI 速率(kHz)。
     * 例：lcd_init 500   = ST7789 序列 @500kHz
     *     lcd_init gc 250= GC9A01 序列 @250kHz */
    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == 'g')
            use_gc = RT_TRUE;
        else if (argv[i][0] >= '0' && argv[i][0] <= '9')
        {
            rt_uint32_t v = 0;
            const char *p = argv[i];
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (v > 0) khz = v;
        }
    }

    if (rt_device_find("lcd0") == RT_NULL)
    {
        rt_pin_mode(BSP_ST7789_DC_PIN, PIN_MODE_OUTPUT);
#ifndef BSP_ST7789_NO_RES
        rt_pin_mode(BSP_ST7789_RES_PIN, PIN_MODE_OUTPUT);
#endif
        rt_pin_mode(LCD_CS_PIN, PIN_MODE_OUTPUT);
        rt_pin_write(LCD_CS_PIN, PIN_HIGH);
        if (rt_spi_bus_attach_device_cspin(&lcd_spi_dev, "lcd0", BSP_ST7789_SPI_BUS,
                                           GET_PIN(E, 7), RT_NULL) != RT_EOK)
        {
            rt_kprintf("lcd_init: attach failed\n");
            return;
        }
    }
    lcd_bb_mode = RT_FALSE;                 /* 回到硬件 SPI 通道 */
    lcd_force_configure(khz * 1000);        /* 强制重配，夺回引脚 */

    rt_kprintf("lcd_init: %s 序列 @ %u kHz\n", use_gc ? "GC9A01" : "ST7789", khz);
    if (use_gc) lcd_init_seq_gc9a01();
    else        lcd_init_seq();

    st7789_fill(0xF800);            /* 初始化完直接填纯红：有红 = 活 */
    rt_kprintf("lcd_init: done, 屏幕应为纯红色\n");
}
MSH_CMD_EXPORT(lcd_init, lcd_init [gc] [khz] - init lcd with variant and speed);

/* 终极对照：GPIO 位 bang 初始化 + 填红。
 * lcd_bb [gc]，与 lcd_init 唯一区别是 SCK/SDA/CS 用软件逐位模拟。
 * 点亮 -> SPI 外设波形在模组端有鬼；也黑 -> 模组坏死/供电带载不足。
 * 跑完后想恢复硬件 SPI，执行一次 lcd_init 即可（会夺回 AF 引脚）。 */
static void lcd_bb(int argc, char **argv)
{
    rt_bool_t use_gc = (argc > 1 && argv[1][0] == 'g');

    rt_pin_mode(BSP_ST7789_DC_PIN, PIN_MODE_OUTPUT);
#ifndef BSP_ST7789_NO_RES
    rt_pin_mode(BSP_ST7789_RES_PIN, PIN_MODE_OUTPUT);
#endif
    bb_gpio_take();
    lcd_bb_mode = RT_TRUE;

    rt_kprintf("lcd_bb: GPIO 位 bang 通道，%s 序列\n", use_gc ? "GC9A01" : "ST7789");
    if (use_gc) lcd_init_seq_gc9a01();
    else        lcd_init_seq();

    st7789_fill(0xF800);            /* 位 bang 填红会慢一些，耐心等 */
    rt_kprintf("lcd_bb: done, 屏幕应为纯红色\n");
}
MSH_CMD_EXPORT(lcd_bb, lcd_bb [gc] - GPIO bitbang init + red fill);

/* CS 沿鉴别：硬件 SPI + 手动 CS(+3µs 护栏)。
 * lcd_raw [gc] [khz] [h] [p] [q]
 *   不带 h：CS 每次传输手动翻转（前后各 3µs 护栏）；
 *   带 h  ：整段初始化 CS 一直拉低（注意：ST7789 靠 CS 上升沿结束每次
 *          传输，全程拉低本来就不该工作，仅作协议验证）；
 *   带 p  ：只保留 pre 护栏（CS 低 -> 3µs -> 时钟）；
 *   带 q  ：只保留 post 护栏（时钟完 -> 3µs -> CS 高）。
 * 设备挂在一个哑 CS(PE7)上，框架不会碰真 CS(PF6)。 */
static struct rt_spi_device lcd_raw_dev;

static void lcd_raw(int argc, char **argv)
{
    struct rt_spi_device *saved = lcd_spi;
    struct rt_spi_configuration cfg;
    rt_bool_t use_gc = RT_FALSE;
    rt_uint32_t khz = LCD_SPI_MAX_HZ / 1000;
    int i;

    lcd_rawcs_hold = RT_FALSE;
    lcd_rawcs_pre  = RT_TRUE;
    lcd_rawcs_post = RT_TRUE;
    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == 'g')      use_gc = RT_TRUE;
        else if (argv[i][0] == 'h') lcd_rawcs_hold = RT_TRUE;
        else if (argv[i][0] == 'p') { lcd_rawcs_pre = RT_TRUE;  lcd_rawcs_post = RT_FALSE; }
        else if (argv[i][0] == 'q') { lcd_rawcs_pre = RT_FALSE; lcd_rawcs_post = RT_TRUE; }
        else if (argv[i][0] >= '0' && argv[i][0] <= '9')
        {
            rt_uint32_t v = 0;
            const char *p = argv[i];
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            if (v > 0) khz = v;
        }
    }

    if (rt_device_find("lcdraw") == RT_NULL)
    {
        if (rt_spi_bus_attach_device_cspin(&lcd_raw_dev, "lcdraw", BSP_ST7789_SPI_BUS,
                                           GET_PIN(E, 7), RT_NULL) != RT_EOK)
        {
            rt_kprintf("lcd_raw: attach failed\n");
            return;
        }
    }
    rt_pin_mode(BSP_ST7789_DC_PIN, PIN_MODE_OUTPUT);
#ifndef BSP_ST7789_NO_RES
    rt_pin_mode(BSP_ST7789_RES_PIN, PIN_MODE_OUTPUT);
#endif
    rt_pin_mode(LCD_CS_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(LCD_CS_PIN, PIN_HIGH);

    cfg.data_width = 8;
    cfg.mode = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz = khz * 1000;

    lcd_spi = &lcd_raw_dev;
    lcd_bb_mode = RT_FALSE;
    lcd_rawcs_mode = RT_TRUE;
    rt_spi_configure(lcd_spi, &cfg);        /* 速率每次可变，缓存必然失效 */

    rt_kprintf("lcd_raw: %s 序列 @ %u kHz, CS %s pre=%d post=%d\n",
               use_gc ? "GC9A01" : "ST7789", khz,
               lcd_rawcs_hold ? "全程拉低" : "手动翻转",
               lcd_rawcs_pre, lcd_rawcs_post);

    if (lcd_rawcs_hold) { rt_pin_write(LCD_CS_PIN, PIN_LOW); rawcs_delay(); }
    if (use_gc) lcd_init_seq_gc9a01();
    else        lcd_init_seq();
    st7789_fill(0xF800);
    if (lcd_rawcs_hold) { rawcs_delay(); rt_pin_write(LCD_CS_PIN, PIN_HIGH); }

    rt_kprintf("lcd_raw: done, 屏幕应为纯红色\n");

    lcd_rawcs_mode = RT_FALSE;
    lcd_rawcs_hold = RT_FALSE;
    lcd_spi = saved;
}
MSH_CMD_EXPORT(lcd_raw, lcd_raw [gc] [khz] [h] - hw SPI with manual CS);

/* ---------- 测试命令 ---------- */
#ifdef RT_USING_FINSH
#include <finsh.h>

/* 彩条 + 纯色轮换 */
static void lcd_test(int argc, char **argv)
{
    static const rt_uint16_t bars[] = {0xF800, 0x07E0, 0x001F, 0xFFFF,
                                       0xFFE0, 0xF81F, 0x07FF, 0x0000};
    rt_uint8_t line[LCD_W * 2];
    int i, x, y;

    /* 竖彩条：每条 30 像素宽。像素段整段一对 CS 护栏 */
    lcd_force_configure(LCD_SPI_MAX_HZ);
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    rt_pin_write(LCD_CS_PIN, PIN_LOW);
    rawcs_delay();
    rt_pin_write(BSP_ST7789_DC_PIN, PIN_HIGH);
    for (y = 0; y < LCD_H; y++)
    {
        for (x = 0; x < LCD_W; x++)
        {
            rt_uint16_t c = bars[x / 30];
            line[x * 2] = c >> 8;
            line[x * 2 + 1] = c & 0xFF;
        }
        rt_spi_send(lcd_spi, line, sizeof(line));
    }
    rawcs_delay();
    rt_pin_write(LCD_CS_PIN, PIN_HIGH);
    rt_kprintf("lcd_test: color bars done\n");

    rt_thread_mdelay(2000);
    for (i = 0; i < 8; i++)
    {
        st7789_fill(bars[i]);
        rt_thread_mdelay(300);
    }
    rt_kprintf("lcd_test: fill rotation done\n");
}
MSH_CMD_EXPORT(lcd_test, st7789 color bar and fill test);

/* 刷屏速度：全屏 blit 若干帧，算 fps。lcd_fps [frames] [khz] */
static void lcd_fps(int argc, char **argv)
{
    int frames = (argc > 1) ? atoi(argv[1]) : 20;
    rt_uint32_t khz = (argc > 2) ? (rt_uint32_t)atoi(argv[2]) : LCD_SPI_MAX_HZ / 1000;
    rt_tick_t start;
    rt_uint8_t *fb;
    int i;

    lcd_force_configure(khz * 1000);
    fb = rt_malloc(LCD_W * LCD_H * 2);
    if (fb == RT_NULL) { rt_kprintf("no mem\n"); return; }
    for (i = 0; i < LCD_W * LCD_H * 2; i++) fb[i] = (rt_uint8_t)(i * 7);

    start = rt_tick_get();
    for (i = 0; i < frames; i++)
        st7789_blit(0, 0, LCD_W - 1, LCD_H - 1, fb);
    rt_uint32_t ms = rt_tick_get() - start;

    rt_kprintf("lcd_fps: %d frames in %u ticks -> %u.%02u fps\n",
               frames, ms,
               frames * RT_TICK_PER_SECOND / ms,
               (frames * RT_TICK_PER_SECOND * 100 / ms) % 100);
    rt_free(fb);
}
MSH_CMD_EXPORT(lcd_fps, lcd_fps [frames] [khz] - full screen blit speed);
#endif /* RT_USING_FINSH */

#endif /* BSP_USING_SPI4 */