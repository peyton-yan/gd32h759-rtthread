/*
 * GD32H7xx TLI LCD driver (480x272 RGB panel) for RT-Thread
 *
 * 设计说明：
 *   TLI 是"自带 DMA 的显示控制器"：初始化后硬件按像素钟持续从帧缓冲
 *   搬数据到面板，CPU 只需往帧缓冲写像素，无需周期性维护。
 *   本驱动以官方 GD32H7xx Demo Suites 24_TLI_IPA 的 tli_config /
 *   tli_gpio_config 为蓝本，改动两处：
 *     1. 帧缓冲从内部 SRAM(0x24005000) 移到 SDRAM(0xC0000000)——
 *        480*272*2=255KB，且 SDRAM 已被 drv_sdram_h7 配成 MPU
 *        non-cacheable，TLI 的 DMA 读取与 CPU 写入天然一致，
 *        不需要 cache 维护（对比 SDIO 的 bounce buffer，两种解法）；
 *     2. demo 的 while(1) 死等换成 LOG_E + return。
 *
 * 板级冲突（博客必读，原理图实锤）：
 *   - PC11 = LCD_B4 / SDIO0_D3，JP52 物理二选一：TLI 需拨 (1,2)；
 *     此时 SDIO 只能跑 1-bit（CLK/CMD/D0 不与 LCD 冲突）
 *   - PG11/PG13/PG14 = LCD_B3/背光/LCD_B0 撞 ENET RMII：开 TLI 必须关 ENET
 *   - PA5 = LCD_R4 撞 DAC0_OUT1：波形显示用 ADC(PC2_C 独占)，DAC 让位
 *
 * 移植指南：
 *   1. Kconfig 打开 BSP_USING_TLI（select BSP_USING_SDRAM，关 BSP_USING_ENET）
 *   2. 跳线：JP52=(1,2)；确认 ENET 相关跳线不挡 LCD 信号
 *   3. 时序参数在配置区，换面板只改时序宏与时钟分频
 */

#include <rthw.h>
#include <rtthread.h>

#if defined(BSP_USING_TLI)

#include "gd32h7xx_tli.h"
#include "drv_gpio.h"
#include <finsh.h>

#define DBG_TAG             "drv.tli"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

/* ===================== 配置区 ===================== */

/* 面板时序（官方 24_TLI_IPA demo 值，全低有效极性） */
#define LCD_HSW             41          /* 水平同步脉宽 */
#define LCD_HBP             2           /* 水平后沿 */
#define LCD_HFP             2           /* 水平前沿 */
#define LCD_VSW             10          /* 垂直同步脉宽 */
#define LCD_VBP             2           /* 垂直后沿 */
#define LCD_VFP             2           /* 垂直前沿 */

#define LCD_WIDTH           480
#define LCD_HEIGHT          272
#define LCD_BPP             2           /* RGB565 = 2 字节/像素 */

/* 像素钟：PLL2R = 25MHz HXTAL /25 *192 /3 = 64MHz，再 8 分频 = 8MHz。
 * 帧率 = 8MHz / ((41+2+480+2) * (10+2+272+2)) ≈ 53Hz */
#define TLI_PLL2_SELF_CONFIG    1

/* 帧缓冲地址：SDRAM 起点（drv_sdram_h7 已配 MPU non-cacheable）。
 * 排障分段：若黑屏，改 0x24005000（官方 demo 验证过的内部 SRAM）
 * 可二分定位——SRAM 能显示 = SDRAM 通路问题；SRAM 也黑 = 时钟/配置问题 */
#define TLI_FB_ADDR         0xC0000000UL

/* ===================== 对外接口 ===================== */

static volatile rt_uint16_t *const tli_fb = (volatile rt_uint16_t *)TLI_FB_ADDR;

/* 写像素（越界丢弃）。面板 RGB888 走线 + 层 RGB565 + 硬件 dither 补位 */
void tli_draw_point(rt_uint16_t x, rt_uint16_t y, rt_uint16_t color)
{
    if (x < LCD_WIDTH && y < LCD_HEIGHT)
        tli_fb[y * LCD_WIDTH + x] = color;
}

/* 整屏填充 */
void tli_fill(rt_uint16_t color)
{
    rt_uint32_t i, total = LCD_WIDTH * LCD_HEIGHT;
    for (i = 0; i < total; i++)
        tli_fb[i] = color;
}

/* 水平线（波形显示的基本件） */
void tli_draw_hline(rt_uint16_t x, rt_uint16_t y, rt_uint16_t len,
                    rt_uint16_t color)
{
    while (len--)
        tli_draw_point(x++, y, color);
}

/* 垂直线 */
void tli_draw_vline(rt_uint16_t x, rt_uint16_t y, rt_uint16_t len,
                    rt_uint16_t color)
{
    while (len--)
        tli_draw_point(x, y++, color);
}

/* ===================== 硬件初始化 ===================== */

static void tli_gpio_config(void)
{
    /* 官方 gpio_config 原样移植：数据/同步线全 AF 推挽 85MHz，
     * PB5 例外 AF3，背光 PG13 普通推挽输出 */
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOG);
    rcu_periph_clock_enable(RCU_GPIOH);

    /* HSYNC(PE15, AF10) VSYNC(PA7) PCLK(PG7) */
    gpio_af_set(GPIOE, GPIO_AF_10, GPIO_PIN_15);
    gpio_af_set(GPIOA, GPIO_AF_14, GPIO_PIN_7);
    gpio_af_set(GPIOG, GPIO_AF_14, GPIO_PIN_7);
    gpio_mode_set(GPIOE, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_15);
    gpio_output_options_set(GPIOE, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_15);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_7);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_7);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_7);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_7);

    /* R7(PG6) R6(PH12) R5(PH11) R4(PA5) R3(PH9) R2(PH8) R1(PH3) R0(PH2) */
    gpio_af_set(GPIOG, GPIO_AF_14, GPIO_PIN_6);
    gpio_af_set(GPIOH, GPIO_AF_14,
                GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_8 | GPIO_PIN_9 |
                GPIO_PIN_11 | GPIO_PIN_12);
    gpio_af_set(GPIOA, GPIO_AF_14, GPIO_PIN_5);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_6);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_6);
    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_8 | GPIO_PIN_9 |
                  GPIO_PIN_11 | GPIO_PIN_12);
    gpio_output_options_set(GPIOH, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_8 | GPIO_PIN_9 |
                            GPIO_PIN_11 | GPIO_PIN_12);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_5);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_5);

    /* G7(PD3) G6(PC7) G5(PC1) G4(PH15) G3(PH14) G2(PH13) G1(PB0) G0(PB1) */
    gpio_af_set(GPIOD, GPIO_AF_14, GPIO_PIN_3);
    gpio_af_set(GPIOC, GPIO_AF_14, GPIO_PIN_7 | GPIO_PIN_1);
    gpio_af_set(GPIOH, GPIO_AF_14,
                GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_af_set(GPIOB, GPIO_AF_14, GPIO_PIN_0 | GPIO_PIN_1);
    gpio_mode_set(GPIOD, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_3);
    gpio_output_options_set(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_3);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_7 | GPIO_PIN_1);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_7 | GPIO_PIN_1);
    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_output_options_set(GPIOH, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_0 | GPIO_PIN_1);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_0 | GPIO_PIN_1);

    /* B7(PB9) B6(PB8) B5(PB5,AF3) B4(PC11) B3(PG11) B2(PG10) B1(PG12) B0(PG14) */
    gpio_af_set(GPIOB, GPIO_AF_14, GPIO_PIN_9 | GPIO_PIN_8);
    gpio_af_set(GPIOB, GPIO_AF_3, GPIO_PIN_5);
    gpio_af_set(GPIOC, GPIO_AF_14, GPIO_PIN_11);
    gpio_af_set(GPIOG, GPIO_AF_14,
                GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_9 | GPIO_PIN_8);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_9 | GPIO_PIN_8);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_5);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_5);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_11);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_11);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_14);

    /* DE(PF10) */
    gpio_af_set(GPIOF, GPIO_AF_14, GPIO_PIN_10);
    gpio_mode_set(GPIOF, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_10);
    gpio_output_options_set(GPIOF, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_10);

    /* 背光 PG13：普通推挽输出，置高点亮（demo 用 GPIO 非 PWM） */
    gpio_mode_set(GPIOG, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, GPIO_PIN_13);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_13);
    gpio_bit_set(GPIOG, GPIO_PIN_13);
}

static rt_err_t tli_hw_config(void)
{
    tli_parameter_struct tli_init_struct;
    tli_layer_parameter_struct layer_init_struct;

    rcu_periph_clock_enable(RCU_TLI);

#if TLI_PLL2_SELF_CONFIG
    /* PLL2R = 25MHz HXTAL /25 *192 /3 = 64MHz，DIV8 后像素钟 8MHz。
     * 症状验证：PLL2 未配置时 TLI 无像素钟，面板全黑且无同步信号 */
    rcu_pll_input_output_clock_range_config(IDX_PLL2, RCU_PLL2RNG_1M_2M,
                                            RCU_PLL2VCO_192M_836M);
    if (ERROR == rcu_pll2_config(25U, 192U, 3U, 3U, 3U))
    {
        LOG_E("PLL2 config failed");
        return -RT_ERROR;
    }
    rcu_pll_clock_output_enable(RCU_PLL2R);
    rcu_tli_clock_div_config(RCU_PLL2R_DIV8);
    rcu_osci_on(RCU_PLL2_CK);
    if (ERROR == rcu_osci_stab_wait(RCU_PLL2_CK))
    {
        LOG_E("PLL2 lock failed");
        return -RT_ERROR;
    }
#endif

    /* 时序：极性全低有效，寄存器值均为实际值-1（官方 demo 原样） */
    tli_init_struct.signalpolarity_hs      = TLI_HSYN_ACTLIVE_LOW;
    tli_init_struct.signalpolarity_vs      = TLI_VSYN_ACTLIVE_LOW;
    tli_init_struct.signalpolarity_de      = TLI_DE_ACTLIVE_LOW;
    tli_init_struct.signalpolarity_pixelck = TLI_PIXEL_CLOCK_TLI;
    tli_init_struct.synpsz_hpsz   = LCD_HSW - 1;
    tli_init_struct.synpsz_vpsz   = LCD_VSW - 1;
    tli_init_struct.backpsz_hbpsz = LCD_HSW + LCD_HBP - 1;
    tli_init_struct.backpsz_vbpsz = LCD_VSW + LCD_VBP - 1;
    tli_init_struct.activesz_hasz = LCD_HSW + LCD_HBP + LCD_WIDTH - 1;
    tli_init_struct.activesz_vasz = LCD_VSW + LCD_VBP + LCD_HEIGHT - 1;
    tli_init_struct.totalsz_htsz  = LCD_HSW + LCD_HBP + LCD_WIDTH + LCD_HFP - 1;
    tli_init_struct.totalsz_vtsz  = LCD_VSW + LCD_VBP + LCD_HEIGHT + LCD_VFP - 1;
    tli_init_struct.backcolor_red   = 0;
    tli_init_struct.backcolor_green = 0;
    tli_init_struct.backcolor_blue  = 0;
    tli_init(&tli_init_struct);

    /* 层 0：全屏窗口，RGB565，帧缓冲在 SDRAM */
    layer_init_struct.layer_window_leftpos   = LCD_HSW + LCD_HBP;
    layer_init_struct.layer_window_rightpos  = LCD_HSW + LCD_HBP + LCD_WIDTH - 1;
    layer_init_struct.layer_window_toppos    = LCD_VSW + LCD_VBP;
    layer_init_struct.layer_window_bottompos = LCD_VSW + LCD_VBP + LCD_HEIGHT - 1;
    layer_init_struct.layer_ppf = LAYER_PPF_RGB565;
    layer_init_struct.layer_sa  = 255;
    layer_init_struct.layer_default_red   = 0;
    layer_init_struct.layer_default_green = 0;
    layer_init_struct.layer_default_blue  = 0;
    layer_init_struct.layer_default_alpha = 0;
    layer_init_struct.layer_acf1 = LAYER_ACF1_SA;
    layer_init_struct.layer_acf2 = LAYER_ACF2_SA;
    layer_init_struct.layer_frame_bufaddr           = TLI_FB_ADDR;
    layer_init_struct.layer_frame_line_length       = LCD_WIDTH * LCD_BPP + 7;
    layer_init_struct.layer_frame_buf_stride_offset = LCD_WIDTH * LCD_BPP;
    layer_init_struct.layer_frame_total_line_number = LCD_HEIGHT;
    tli_layer_init(LAYER0, &layer_init_struct);

    /* RGB565 -> RGB888 走线的低位靠硬件 dither 补，显著改善色带 */
    tli_dither_config(TLI_DITHER_ENABLE);

    tli_layer_enable(LAYER0);
    tli_reload_config(TLI_REQUEST_RELOAD_EN);
    tli_enable();
    return RT_EOK;
}

int rt_hw_tli_init(void)
{
    tli_gpio_config();      /* 严格按官方 demo 顺序：先 GPIO 后控制器 */
    if (tli_hw_config() != RT_EOK)
        return -RT_ERROR;

    tli_fill(0x0000);       /* 上电黑屏，避免显示 SDRAM 随机数据 */
#if TLI_FB_ADDR != 0xC0000000UL
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)tli_fb,
                         LCD_WIDTH * LCD_HEIGHT * LCD_BPP);
#endif
    LOG_I("tli lcd %dx%d @ fb=0x%x", LCD_WIDTH, LCD_HEIGHT, TLI_FB_ADDR);
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_tli_init);

/* ===================== 测试命令 ===================== */

/* tli_test <bars|red|green|blue|white|black|grid> */
static void tli_test(int argc, char **argv)
{
    const char *pat = (argc >= 2) ? argv[1] : "bars";
    rt_uint16_t x, y;

    if (!rt_strcmp(pat, "bars"))
    {
        /* 八色竖条：验证数据线每一位——某色通道断线时对应条纹异常 */
        static const rt_uint16_t bars[8] = {
            0xFFFF, 0xFFE0, 0x07E0, 0x07FF, 0xF800, 0xF81F, 0x001F, 0x0000
        };
        for (x = 0; x < LCD_WIDTH; x++)
            for (y = 0; y < LCD_HEIGHT; y++)
                tli_fb[y * LCD_WIDTH + x] = bars[x / (LCD_WIDTH / 8)];
    }
    else if (!rt_strcmp(pat, "grid"))
    {
        /* 网格：验证行列寻址——frame_stride 错时网格歪斜 */
        tli_fill(0x0000);
        for (x = 0; x < LCD_WIDTH; x += 48)
            tli_draw_vline(x, 0, LCD_HEIGHT, 0x07E0);
        for (y = 0; y < LCD_HEIGHT; y += 34)
            tli_draw_hline(0, y, LCD_WIDTH, 0x07E0);
    }
    else
    {
        rt_uint16_t color = 0x0000;
        if      (!rt_strcmp(pat, "red"))   color = 0xF800;
        else if (!rt_strcmp(pat, "green")) color = 0x07E0;
        else if (!rt_strcmp(pat, "blue"))  color = 0x001F;
        else if (!rt_strcmp(pat, "white")) color = 0xFFFF;
        else if (!rt_strcmp(pat, "black")) color = 0x0000;
        else
        {
            rt_kprintf("usage: tli_test <bars|red|green|blue|white|black|grid>\n");
            return;
        }
        tli_fill(color);
    }
#if TLI_FB_ADDR != 0xC0000000UL
    /* 帧缓冲在内部 SRAM 的排障形态：D-cache 开启，写完必须刷到内存
     * TLI 才能看见（官方 demo 在主循环里周期性 CleanInvalidate 同理） */
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, (void *)tli_fb,
                         LCD_WIDTH * LCD_HEIGHT * LCD_BPP);
#endif
    rt_kprintf("tli_test %s done\n", pat);
}
MSH_CMD_EXPORT_ALIAS(tli_test, tli_test,
                     lcd self test: tli_test <bars|red|green|blue|white|black|grid>);

/* tli_dump：无示波器条件下的 TLI 运行状态取证。
 * 判读指南：
 *   CPPOS 两次读值不同     -> 像素钟活着，TLI 正在扫描（时钟域无罪）
 *   CPPOS 恒为 0/不变      -> TLI 停摆，查 PLL2/时钟/使能
 *   INTF bit2(TEF)=1       -> 读帧缓冲总线事务错误，查 FB 地址/SDRAM
 *   INTF bit1(FEF)=1       -> FIFO 错误，数据供应不上（带宽/时钟比）
 *   L0FBADDR               -> 必须与 TLI_FB_ADDR 一致（reload 生效证据） */
static void tli_dump(int argc, char **argv)
{
    rt_uint32_t pos1, pos2;

    pos1 = TLI_CPPOS;
    rt_thread_mdelay(20);
    pos2 = TLI_CPPOS;

    rt_kprintf("TLI_CTL    = 0x%08x (bit0 TLIEN=%d)\n", TLI_CTL, TLI_CTL & 1);
    rt_kprintf("TLI_SPSZ   = 0x%08x  BPSZ = 0x%08x\n", TLI_SPSZ, TLI_BPSZ);
    rt_kprintf("TLI_ASZ    = 0x%08x  TSZ  = 0x%08x\n", TLI_ASZ, TLI_TSZ);
    rt_kprintf("TLI_STAT   = 0x%08x (VDE/HDE/VS/HS live)\n", TLI_STAT);
    rt_kprintf("TLI_INTF   = 0x%08x (bit1 FEF fifo-err, bit2 TEF bus-err)\n",
               TLI_INTF);
    rt_kprintf("TLI_CPPOS  = 0x%08x -> 0x%08x (%s)\n", pos1, pos2,
               pos1 != pos2 ? "SCANNING, pixel clock alive"
                            : "FROZEN, tli not running");
    rt_kprintf("L0CTL      = 0x%08x  L0FBADDR = 0x%08x (expect 0x%08x)\n",
               TLI_LXCTL(LAYER0), TLI_LXFBADDR(LAYER0), TLI_FB_ADDR);
    rt_kprintf("L0FLLEN    = 0x%08x  L0FTLN   = 0x%08x\n",
               TLI_LXFLLEN(LAYER0), TLI_LXFTLN(LAYER0));
}
MSH_CMD_EXPORT_ALIAS(tli_dump, tli_dump, dump tli registers for diagnosis);

#endif /* BSP_USING_TLI */