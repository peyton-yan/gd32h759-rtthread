/*
 * ADC2 + DAC 联合测试（仪器版）
 * 接线: JP71 短接; 杜邦线 PA5 <-> PC2_C
 *
 * 命令:
 *   adc_dac_test        DC 链路自检（DAC 写阶梯, ADC 读回）
 *   dac_raw_test [val]  裸寄存器 DAC 输出（绕过框架, 配合万用表）
 *   dac_wave <freq>      TIMER4->TRIGSEL->DAC DMA 正弦波输出
 *   adc_sample [fs]     TIMER3->TRIGSEL->ADC2 DMA 采样(默认 51200Hz), 打印 min/max/avg
 *   adc_dump            打印采样缓冲(头16点+间隔64抽取)
 *   adc_fft             1024 点 FFT, 打印 top-3 谱峰
 *
 * 踩坑修复全部内嵌:
 *   - RCU_DMAMUX 时钟必须开, 否则路由寄存器写入被静默忽略
 *   - DMA 位宽必须显式调 dma_memory_width_config/dma_periph_width_config
 *     (结构体 periph_memory_width 字段无效, 否则 8-bit 字节复制)
 *   - adc2_dma_start 一次性守卫, 热重 init 会产生垃圾帧
 *   - adc_snapshot 只 invalidate cache, 不停 DMA 通道(否则后续全读到陈旧缓冲)
 *   - 不碰 SYSCFG PC2_C 模拟开关(复位默认即正确)
 *   - DAC 不调 dac_mode_config(实测会写死输出)
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "gd32h7xx.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define DBG_TAG "app.adcdac"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define ADC_BUF_SIZE   1024
#define WAVE_POINTS    128
#define ADC_DEFAULT_FS 51200

static rt_uint16_t adc_buf[ADC_BUF_SIZE] __attribute__((aligned(32)));
static rt_uint16_t wave_buf[WAVE_POINTS] __attribute__((aligned(32)));
static rt_uint32_t g_adc_fs = ADC_DEFAULT_FS;

/* ------------------------------------------------------------------ */
/* 公共: 定时器 TRGO0 = update 事件, 输出给定频率                       */
/* ------------------------------------------------------------------ */
static void timer_trgo_setup(uint32_t timer_periph, rcu_periph_enum rcu, rt_uint32_t update_freq)
{
    rt_uint32_t tclk, div, psc, arr;

    tclk = rcu_clock_freq_get(CK_APB1);
    {
        rt_uint32_t psc_field = GET_BITS(RCU_CFG0, 10, 12);   /* RCU_CFG0_APB1PSC = BITS(10,12) */
        rt_bool_t mul4 = ((RCU_CFG1 & RCU_CFG1_TIMERSEL) != 0U);
        /* GD32H7 定时器时钟规则:
           APB1 分频(域值>=4): 定时器时钟 = CK_APB1 x2 (TIMERSEL=0) 或 x4 (TIMERSEL=1)
           APB1 不分频(域值<4):  定时器时钟 = CK_APB1 (=CK_AHB), TIMERSEL=1 时 x2 */
        if (psc_field >= 4U) {
            tclk *= (mul4 ? 4U : 2U);
        } else if (mul4) {
            tclk *= 2U;
        }
    }

    div = tclk / update_freq;
    if (div == 0U) div = 1U;
    psc = 1U;
    while ((div / psc) > 65536U) {        /* TIMER3/4 是 16 位 */
        psc <<= 1;
    }
    arr = div / psc;
    if (arr == 0U) arr = 1U;

    rcu_periph_clock_enable(rcu);
    timer_deinit(timer_periph);
    timer_prescaler_config(timer_periph, psc - 1U, TIMER_PSC_RELOAD_NOW);
    timer_autoreload_value_config(timer_periph, arr - 1U);
    timer_master_output0_trigger_source_select(timer_periph, TIMER_TRI_OUT0_SRC_UPDATE);
    timer_enable(timer_periph);
}

/* ------------------------------------------------------------------ */
/* 命令1: DC 链路自检                                                   */
/* ------------------------------------------------------------------ */
void adc_dac_test(int argc, char **argv)
{
    rt_adc_device_t adc;
    rt_dac_device_t dac;
    static const rt_uint32_t steps[] = { 0, 1024, 2048, 3072, 4095 };
    rt_uint32_t i, k, sum, avg;
    rt_int32_t diff;

    adc = (rt_adc_device_t)rt_device_find("adc2");
    dac = (rt_dac_device_t)rt_device_find("dac");
    if ((adc == RT_NULL) || (dac == RT_NULL)) {
        rt_kprintf("device not found: adc2=%p dac=%p\n", adc, dac);
        return;
    }

    rt_adc_enable(adc, 0);
    rt_dac_enable(dac, 1);

    rt_kprintf("step | DAC write | ADC read(avg of 8) | diff\n");
    for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        rt_dac_write(dac, 1, steps[i]);
        rt_thread_mdelay(2);

        sum = 0;
        for (k = 0; k < 8; k++) {
            sum += rt_adc_read(adc, 0);
        }
        avg = sum / 8;
        diff = (rt_int32_t)avg - (rt_int32_t)steps[i];
        rt_kprintf("  %lu  |    %4lu   |        %4lu        | %+ld\n",
                   i, steps[i], avg, diff);
    }

    rt_dac_write(dac, 1, 0);
    rt_kprintf("note: 两端被 VR1(1K) 负载压缩属物理规律, 中段准即链路正常\n");
}
MSH_CMD_EXPORT(adc_dac_test, ADC2 DAC joint selftest);

/* ------------------------------------------------------------------ */
/* 命令2: 裸寄存器 DAC 输出（复刻官方例程 10）                          */
/* ------------------------------------------------------------------ */
void dac_raw_test(int argc, char **argv)
{
    rt_uint32_t v = 4095;
    if (argc == 2) v = (rt_uint32_t)atoi(argv[1]);

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_DAC);
    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_5);

    dac_deinit(DAC0);
    dac_trigger_disable(DAC0, DAC_OUT1);
    dac_wave_mode_config(DAC0, DAC_OUT1, DAC_WAVE_DISABLE);
    dac_enable(DAC0, DAC_OUT1);
    dac_data_set(DAC0, DAC_OUT1, DAC_ALIGN_12B_R, (uint16_t)(v & 0x0FFFU));

    rt_kprintf("DAC0_OUT1 <- %lu, DO readback = %u, 期望电压约 %lu mV\n",
               v, dac_output_value_get(DAC0, DAC_OUT1), v * 3300U / 4095U);
}
MSH_CMD_EXPORT(dac_raw_test, raw DAC output bypass framework);

/* ------------------------------------------------------------------ */
/* 命令3: DAC DMA 正弦波  TIMER4 TRGO0 -> TRIGSEL -> DAC0_OUT1          */
/* ------------------------------------------------------------------ */
void dac_wave(int argc, char **argv)
{
    rt_uint32_t freq = 1000;
    int i;
    dma_single_data_parameter_struct cfg;

    if (argc == 2) freq = (rt_uint32_t)atoi(argv[1]);
    if (freq == 0U) freq = 1000;

    for (i = 0; i < WAVE_POINTS; i++) {
        wave_buf[i] = (rt_uint16_t)(2048.0 + 2047.0 * sin(2.0 * 3.14159265358979 * i / WAVE_POINTS));
    }
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, wave_buf, sizeof(wave_buf));

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_DAC);
    rcu_periph_clock_enable(RCU_DMA1);
    rcu_periph_clock_enable(RCU_DMAMUX);
    rcu_periph_clock_enable(RCU_TRIGSEL);

    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_5);

    dac_deinit(DAC0);
    dac_wave_mode_config(DAC0, DAC_OUT1, DAC_WAVE_DISABLE);
    dac_trigger_source_config(DAC0, DAC_OUT1, DAC_TRIGGER_EXTERNAL);

    /* DMA1 CH0: memory -> DAC_OUT1_R12DH, 循环 */
    dma_deinit(DMA1, DMA_CH0);
    dma_single_data_para_struct_init(&cfg);
    cfg.request             = DMA_REQUEST_DAC_CH1;            /* 82 */
    cfg.periph_addr         = (rt_uint32_t)&DAC_OUT1_R12DH(DAC0);
    cfg.periph_inc          = DMA_PERIPH_INCREASE_DISABLE;
    cfg.memory0_addr        = (rt_uint32_t)wave_buf;
    cfg.memory_inc          = DMA_MEMORY_INCREASE_ENABLE;
    cfg.circular_mode       = DMA_CIRCULAR_MODE_ENABLE;
    cfg.direction           = DMA_MEMORY_TO_PERIPH;
    cfg.number              = WAVE_POINTS;
    cfg.priority            = DMA_PRIORITY_HIGH;
    dma_single_data_mode_init(DMA1, DMA_CH0, &cfg);
    /* 位宽字段无效, 必须显式配置 */
    dma_memory_width_config(DMA1, DMA_CH0, DMA_MEMORY_WIDTH_16BIT);
    dma_periph_width_config(DMA1, DMA_CH0, DMA_PERIPH_WIDTH_16BIT);
    /* 保险: 手动确认 DMAMUX 路由（通道 8 = DMA1 CH0） */
    DMAMUX_RM_CH8CFG = (DMAMUX_RM_CH8CFG & ~0x7FU) | 82U;
    dma_channel_enable(DMA1, DMA_CH0);

    /* TRIGSEL: TIMER4 TRGO0 -> DAC_OUT1 外部触发 */
    trigsel_init(TRIGSEL_OUTPUT_DAC_OUT1_EXTRG, TRIGSEL_INPUT_TIMER4_TRGO0);

    dac_dma_enable(DAC0, DAC_OUT1);
    dac_trigger_enable(DAC0, DAC_OUT1);
    dac_enable(DAC0, DAC_OUT1);

    timer_trgo_setup(TIMER4, RCU_TIMER4, freq * WAVE_POINTS);

    rt_kprintf("dac_wave: %lu Hz, %u 点/周期, DAC 更新率 %lu Hz\n",
               freq, WAVE_POINTS, freq * WAVE_POINTS);
}
MSH_CMD_EXPORT(dac_wave, DAC DMA sine wave output);

/* ------------------------------------------------------------------ */
/* ADC2 DMA 采样管线  TIMER3 TRGO0 -> TRIGSEL -> ADC2, DMA1 CH1        */
/* ------------------------------------------------------------------ */
static void adc2_dma_start(rt_uint32_t fs)
{
    static rt_bool_t inited = RT_FALSE;   /* 一次性守卫: 热重 init 会产生垃圾帧 */
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

    /* DMA1 CH1: ADC_RDATA -> adc_buf, 循环（通道 9 = DMA1 CH1） */
    dma_deinit(DMA1, DMA_CH1);
    dma_single_data_para_struct_init(&cfg);
    cfg.request             = DMA_REQUEST_ADC2;               /* 123 */
    cfg.periph_addr         = (rt_uint32_t)&ADC_RDATA(ADC2);
    cfg.periph_inc          = DMA_PERIPH_INCREASE_DISABLE;
    cfg.memory0_addr        = (rt_uint32_t)adc_buf;
    cfg.memory_inc          = DMA_MEMORY_INCREASE_ENABLE;
    cfg.circular_mode       = DMA_CIRCULAR_MODE_ENABLE;
    cfg.direction           = DMA_PERIPH_TO_MEMORY;
    cfg.number              = ADC_BUF_SIZE;
    cfg.priority            = DMA_PRIORITY_HIGH;
    dma_single_data_mode_init(DMA1, DMA_CH1, &cfg);
    dma_memory_width_config(DMA1, DMA_CH1, DMA_MEMORY_WIDTH_16BIT);
    dma_periph_width_config(DMA1, DMA_CH1, DMA_PERIPH_WIDTH_16BIT);
    DMAMUX_RM_CH9CFG = (DMAMUX_RM_CH9CFG & ~0x7FU) | 123U;
    dma_channel_enable(DMA1, DMA_CH1);

    /* TRIGSEL: TIMER3 TRGO0 -> ADC2 规则组外部触发 */
    trigsel_init(TRIGSEL_OUTPUT_ADC2_REGTRG, TRIGSEL_INPUT_TIMER3_TRGO0);

    adc_dma_mode_enable(ADC2);
    adc_dma_request_after_last_enable(ADC2);

    adc_enable(ADC2);
    rt_thread_mdelay(1);
    adc_calibration_mode_config(ADC2, ADC_CALIBRATION_OFFSET);
    adc_calibration_number(ADC2, ADC_CALIBRATION_NUM1);
    adc_calibration_enable(ADC2);

    g_adc_fs = fs;
    timer_trgo_setup(TIMER3, RCU_TIMER3, fs);

    rt_kprintf("adc2 dma pipeline started, fs = %lu Hz\n", fs);
}

static void adc_snapshot(void)
{
    /* 循环 DMA 持续在跑, 不停通道; 读前 invalidate 即可 */
    rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, adc_buf, sizeof(adc_buf));
}

/* ------------------------------------------------------------------ */
/* 命令4: 采样统计                                                      */
/* ------------------------------------------------------------------ */
void adc_sample(int argc, char **argv)
{
    rt_uint32_t fs = ADC_DEFAULT_FS;
    rt_uint32_t i, mn = 4095, mx = 0, sum = 0;

    if (argc == 2) fs = (rt_uint32_t)atoi(argv[1]);
    adc2_dma_start(fs);
    rt_thread_mdelay(30);           /* 等缓冲填满一轮以上 */
    adc_snapshot();

    for (i = 0; i < ADC_BUF_SIZE; i++) {
        if (adc_buf[i] < mn) mn = adc_buf[i];
        if (adc_buf[i] > mx) mx = adc_buf[i];
        sum += adc_buf[i];
    }
    rt_kprintf("[ADC] %u samples @ %lu Hz: min=%lu max=%lu avg=%lu\n",
               ADC_BUF_SIZE, g_adc_fs, mn, mx, sum / ADC_BUF_SIZE);
    rt_kprintf("[ADC] 电压: min=%lu mV max=%lu mV avg=%lu mV (3.3V 参考)\n",
               mn * 3300U / 4095U, mx * 3300U / 4095U, (sum / ADC_BUF_SIZE) * 3300U / 4095U);
}
MSH_CMD_EXPORT(adc_sample, ADC2 DMA sample stats);

/* ------------------------------------------------------------------ */
/* 命令5: 缓冲内容                                                      */
/* ------------------------------------------------------------------ */
void adc_dump(int argc, char **argv)
{
    rt_uint32_t i;
    adc_snapshot();

    rt_kprintf("head16:");
    for (i = 0; i < 16; i++) rt_kprintf(" %u", adc_buf[i]);
    rt_kprintf("\nstride64:");
    for (i = 0; i < ADC_BUF_SIZE; i += 64) rt_kprintf(" %u", adc_buf[i]);
    rt_kprintf("\n");
}
MSH_CMD_EXPORT(adc_dump, dump ADC DMA buffer);

/* ------------------------------------------------------------------ */
/* 命令6: 1024 点 FFT（Hann 窗, 迭代 radix-2, top-3 谱峰）              */
/* ------------------------------------------------------------------ */
void adc_fft(int argc, char **argv)
{
    static float re[ADC_BUF_SIZE], im[ADC_BUF_SIZE];
    float mag[ADC_BUF_SIZE / 2];
    rt_uint32_t i, j, k, n = ADC_BUF_SIZE;
    float fs = (float)g_adc_fs;
    int rank_idx[3] = { -1, -1, -1 };

    adc2_dma_start(ADC_DEFAULT_FS);
    rt_thread_mdelay(30);
    adc_snapshot();

    /* Hann 窗 + 去直流 */
    {
        rt_uint32_t sum = 0;
        float mean;
        for (i = 0; i < n; i++) sum += adc_buf[i];
        mean = (float)sum / (float)n;
        for (i = 0; i < n; i++) {
            float w = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * (float)i / (float)(n - 1)));
            re[i] = ((float)adc_buf[i] - mean) * w;
            im[i] = 0.0f;
        }
    }

    /* bit-reverse */
    for (i = 1, j = 0; i < n; i++) {
        rt_uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    /* 蝶形 */
    for (k = 2; k <= n; k <<= 1) {
        float ang = -2.0f * 3.14159265f / (float)k;
        float wr_step = cosf(ang), wi_step = sinf(ang);
        rt_uint32_t half = k >> 1, base;
        for (base = 0; base < n; base += k) {
            float wr = 1.0f, wi = 0.0f;
            for (i = 0; i < half; i++) {
                rt_uint32_t a = base + i, b = base + i + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
                { float twr = wr * wr_step - wi * wi_step;
                  wi = wr * wi_step + wi * wr_step; wr = twr; }
            }
        }
    }

    for (i = 0; i < n / 2; i++) {
        mag[i] = sqrtf(re[i] * re[i] + im[i] * im[i]);
    }

    /* top-3 谱峰（跳过 DC 及相邻 bin, 要求局部极大） */
    for (k = 0; k < 3; k++) {
        float best = 0.0f;
        int best_i = -1;
        for (i = 2; i < (int)(n / 2) - 2; i++) {
            int skip = 0;
            for (j = 0; j < k; j++) {
                if (abs((int)i - rank_idx[j]) <= 2) { skip = 1; break; }
            }
            if (skip) continue;
            if (mag[i] > best && mag[i] >= mag[i - 1] && mag[i] >= mag[i + 1]) {
                best = mag[i]; best_i = (int)i;
            }
        }
        rank_idx[k] = best_i;
        if (best_i > 0) {
            rt_kprintf("[FFT] peak%d: %lu Hz (bin %d), magnitude %ld\n",
                       k + 1, (rt_uint32_t)(best_i * (int)fs / (int)n), best_i, (rt_int32_t)best);
        }
    }
    rt_kprintf("[FFT] fs=%lu Hz, 分辨率 %lu Hz/bin\n",
               g_adc_fs, g_adc_fs / ADC_BUF_SIZE);
}
MSH_CMD_EXPORT(adc_fft, 1024pt FFT of ADC buffer);