/*
 * 波形大屏显示（三战役会师应用）：ADC2 DMA 采样 -> TLI LCD 实时波形
 *
 * 数据链路：TIMER3 TRGO0 -> TRIGSEL -> ADC2 -> DMA1 CH1 循环缓冲
 *           -> 软件触发找上升沿 -> 480 点/帧 -> LCD 逐列重绘
 *
 * 交互：
 *   触摸屏幕任意位置 = RUN / STOP 切换（示波器第一按钮）
 *   wave_show [fs]   启动（默认 51200Hz），再敲一次或 Ctrl+C 退出命令
 *
 * 接线：JP71 短接 + 杜邦线 PA5(已让位 LCD_R4? 注意!) <-> PC2_C
 *   —— LCD 战役后 PA5 是 LCD_R4，闭环演示改用 VR1 电位器当信号源，
 *      或者把 dac_wave 的杜邦线拔掉、单看 ADC 悬浮噪声都可以。
 *
 * 设计要点：
 *   - 采样核心与 adc_dac_test.c 同款（TIMER/DMA 参数全部验证过），
 *     但用自己的 adc_buf，adc_sample 等命令互不干扰；
 *   - 软件触发：从 DMA 写指针向前找中点上升沿，周期信号显示稳定不滚动；
 *   - 逐列重绘（擦旧列->补网格->画新点），不整屏填充，600MHz 下 ~20fps；
 *   - 帧缓冲在 SDRAM（MPU non-cacheable），画完即所见，无需 cache 维护。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "gd32h7xx.h"
#include <stdlib.h>

#if defined(BSP_USING_TLI)

#define DBG_TAG             "app.wave"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

extern void tli_draw_point(rt_uint16_t x, rt_uint16_t y, rt_uint16_t color);
extern void tli_draw_vline(rt_uint16_t x, rt_uint16_t y, rt_uint16_t len,
                           rt_uint16_t color);
extern void tli_fill(rt_uint16_t color);
extern rt_err_t gt911_read_point(rt_uint16_t *x, rt_uint16_t *y);

/* ===================== 配置区 ===================== */

#define WAVE_ADC_BUF_SIZE   1024        /* DMA 循环缓冲（2 的幂） */
#define WAVE_DEFAULT_FS     51200       /* 默认采样率 */
#define WAVE_FPS_MS         50          /* 帧间隔 ~20fps */

#define LCD_W               480
#define LCD_H               272
#define COLOR_BG            0x0000      /* 黑底 */
#define COLOR_GRID          0x4208      /* 深灰网格 */
#define COLOR_WAVE          0x07E0      /* 绿色波形（示波器经典配色） */
#define COLOR_MID           0xF800      /* 红色中位线 */

#define ADC_MID_CODE        2048        /* 12-bit 中点，软件触发电平 */

/* ===================== ADC2 DMA 采样管线（与 adc_dac_test.c 同款） ===================== */

static rt_uint16_t wave_adc_buf[WAVE_ADC_BUF_SIZE] __attribute__((aligned(32)));
static rt_uint16_t frame_buf[LCD_W];    /* 当前帧的 y 坐标（已换算） */

static void timer3_trgo_setup(rt_uint32_t update_freq)
{
    rt_uint32_t tclk, div, psc, arr;

    tclk = rcu_clock_freq_get(CK_APB1);
    {
        rt_uint32_t psc_field = GET_BITS(RCU_CFG0, 10, 12);   /* H7: BITS(10,12) */
        rt_bool_t mul4 = ((RCU_CFG1 & RCU_CFG1_TIMERSEL) != 0U);
        if (psc_field >= 4U)
            tclk *= (mul4 ? 4U : 2U);
        else if (mul4)
            tclk *= 2U;
    }

    div = tclk / update_freq;
    if (div == 0U) div = 1U;
    psc = 1U;
    while ((div / psc) > 65536U)
        psc <<= 1;
    arr = div / psc;
    if (arr == 0U) arr = 1U;

    rcu_periph_clock_enable(RCU_TIMER3);
    timer_deinit(TIMER3);
    timer_prescaler_config(TIMER3, psc - 1U, TIMER_PSC_RELOAD_NOW);
    timer_autoreload_value_config(TIMER3, arr - 1U);
    timer_master_output0_trigger_source_select(TIMER3, TIMER_TRI_OUT0_SRC_UPDATE);
    timer_enable(TIMER3);
}

static void wave_adc_start(rt_uint32_t fs)
{
    static rt_bool_t inited = RT_FALSE;   /* 一次性守卫：热重 init 会产生垃圾帧 */
    dma_single_data_parameter_struct cfg;

    if (inited) return;
    inited = RT_TRUE;

    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_ADC2);
    rcu_periph_clock_enable(RCU_DMA1);
    rcu_periph_clock_enable(RCU_DMAMUX);
    rcu_periph_clock_enable(RCU_TRIGSEL);

    adc_deinit(ADC2);
    adc_clock_config(ADC2, ADC_CLK_SYNC_HCLK_DIV6);
    adc_special_function_config(ADC2, ADC_SCAN_MODE, DISABLE);
    adc_special_function_config(ADC2, ADC_CONTINUOUS_MODE, DISABLE);
    adc_resolution_config(ADC2, ADC_RESOLUTION_12B);
    adc_data_alignment_config(ADC2, ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(ADC2, ADC_REGULAR_CHANNEL, 1);
    adc_regular_channel_config(ADC2, 0, ADC_CHANNEL_0, 480);
    adc_external_trigger_config(ADC2, ADC_REGULAR_CHANNEL, EXTERNAL_TRIGGER_RISING);

    /* DMA1 CH1: ADC_RDATA -> wave_adc_buf, 循环（DMAMUX 通道 9，请求 123） */
    dma_deinit(DMA1, DMA_CH1);
    dma_single_data_para_struct_init(&cfg);
    cfg.request             = DMA_REQUEST_ADC2;
    cfg.periph_addr         = (rt_uint32_t)&ADC_RDATA(ADC2);
    cfg.periph_inc          = DMA_PERIPH_INCREASE_DISABLE;
    cfg.memory0_addr        = (rt_uint32_t)wave_adc_buf;
    cfg.memory_inc          = DMA_MEMORY_INCREASE_ENABLE;
    cfg.circular_mode       = DMA_CIRCULAR_MODE_ENABLE;
    cfg.direction           = DMA_PERIPH_TO_MEMORY;
    cfg.number              = WAVE_ADC_BUF_SIZE;
    cfg.priority            = DMA_PRIORITY_HIGH;
    dma_single_data_mode_init(DMA1, DMA_CH1, &cfg);
    /* 位宽字段无效，必须显式配置（否则 8-bit 字节复制） */
    dma_memory_width_config(DMA1, DMA_CH1, DMA_MEMORY_WIDTH_16BIT);
    dma_periph_width_config(DMA1, DMA_CH1, DMA_PERIPH_WIDTH_16BIT);
    DMAMUX_RM_CH9CFG = (DMAMUX_RM_CH9CFG & ~0x7FU) | 123U;
    dma_channel_enable(DMA1, DMA_CH1);

    trigsel_init(TRIGSEL_OUTPUT_ADC2_REGTRG, TRIGSEL_INPUT_TIMER3_TRGO0);

    adc_dma_mode_enable(ADC2);
    adc_dma_request_after_last_enable(ADC2);

    adc_enable(ADC2);
    rt_thread_mdelay(1);
    adc_calibration_mode_config(ADC2, ADC_CALIBRATION_OFFSET);
    adc_calibration_number(ADC2, ADC_CALIBRATION_NUM1);
    adc_calibration_enable(ADC2);

    timer3_trgo_setup(fs);
    LOG_I("wave adc pipeline started, fs = %lu Hz", fs);
}

/* ===================== 显示 ===================== */

/* 画一列的背景（黑底 + 该列可能压着的网格点/中线点） */
static void wave_erase_column(rt_uint16_t x)
{
    rt_uint16_t y;

    for (y = 0; y < LCD_H; y++)
    {
        rt_uint16_t c = COLOR_BG;
        if (y == LCD_H / 2)
            c = COLOR_MID;                          /* 中位线 1.65V */
        else if ((x % 48 == 0) || (y % 34 == 0))
            c = COLOR_GRID;
        tli_draw_point(x, y, c);
    }
}

static void wave_draw_frame(void)
{
    rt_uint16_t x;

    for (x = 0; x < LCD_W; x++)
    {
        wave_erase_column(x);
        tli_draw_point(x, frame_buf[x], COLOR_WAVE);
        /* 与前一列连线，波形不断裂 */
        if (x > 0)
        {
            rt_int32_t y0 = frame_buf[x - 1], y1 = frame_buf[x];
            if (y1 > y0 + 1)
                tli_draw_vline(x, y0 + 1, y1 - y0 - 1, COLOR_WAVE);
            else if (y0 > y1 + 1)
                tli_draw_vline(x, y1 + 1, y0 - y1 - 1, COLOR_WAVE);
        }
    }
}

/* 从循环缓冲取一帧：软件触发——从写指针前找中点上升沿，取其后 480 点 */
static void wave_acquire_frame(void)
{
    rt_uint32_t wpos, start, i;

    /* DMA 循环在跑，读前 invalidate（不停通道，停了后续全是陈旧数据） */
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, wave_adc_buf,
                         sizeof(wave_adc_buf));
    wpos = WAVE_ADC_BUF_SIZE - (DMA_CHCNT(DMA1, DMA_CH1) & 0xFFFFU);

    /* 从写指针向前（旧数据方向）搜中点上升沿，最多搜半缓冲 */
    start = wpos;
    for (i = 1; i < WAVE_ADC_BUF_SIZE / 2; i++)
    {
        rt_uint32_t cur = (wpos - i) & (WAVE_ADC_BUF_SIZE - 1);
        rt_uint32_t pre = (wpos - i - 1) & (WAVE_ADC_BUF_SIZE - 1);

        if (wave_adc_buf[pre] < ADC_MID_CODE &&
            wave_adc_buf[cur] >= ADC_MID_CODE)
        {
            start = cur;
            break;
        }
    }

    for (i = 0; i < LCD_W; i++)
    {
        rt_uint32_t idx = (start + i) & (WAVE_ADC_BUF_SIZE - 1);
        rt_uint32_t code = wave_adc_buf[idx];

        if (code > 4095U) code = 4095U;
        /* code 0 -> 屏幕底部(LCD_H-1)，code 4095 -> 顶部(0) */
        frame_buf[i] = (rt_uint16_t)((LCD_H - 1) -
                                     code * (LCD_H - 1) / 4095U);
    }
}

/* ===================== 命令 ===================== */

static void wave_show(int argc, char **argv)
{
    rt_uint32_t fs = WAVE_DEFAULT_FS;
    rt_bool_t running = RT_TRUE;
    rt_uint16_t tx = 0, ty = 0;
    rt_bool_t was_touched = RT_FALSE;

    if (argc >= 2)
        fs = (rt_uint32_t)atoi(argv[1]);

    wave_adc_start(fs);
    tli_fill(COLOR_BG);
    rt_kprintf("wave_show: fs=%lu Hz, touch screen = RUN/STOP\n", fs);

    while (1)
    {
        if (running)
        {
            wave_acquire_frame();
            wave_draw_frame();
        }

        /* 触摸 = RUN/STOP，带按下沿检测（按住不连发） */
#if defined(BSP_USING_TOUCH)
        if (gt911_read_point(&tx, &ty) == RT_EOK)
        {
            if (!was_touched)
            {
                running = !running;
                was_touched = RT_TRUE;
                rt_kprintf("%s\n", running ? "RUN" : "STOP");
            }
        }
        else
        {
            was_touched = RT_FALSE;
        }
#endif
        rt_thread_mdelay(WAVE_FPS_MS);
    }
}
MSH_CMD_EXPORT_ALIAS(wave_show, wave_show,
                     adc waveform on lcd: wave_show [fs]);

#endif /* BSP_USING_TLI */