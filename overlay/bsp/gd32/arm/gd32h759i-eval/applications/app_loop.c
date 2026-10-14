/*
 * app_loop.c — DAC↔ADC 闭环控制回路实现
 *
 * PID：整数实现（增益 ×100），带积分抗饱和；越限联锁切安全输出。
 * 周期统计用 DWT->CYCCNT（CPU 周期计数）换算 us，反映实时性。
 */
#include "app_loop.h"

#if defined(BSP_USING_ADC) && defined(RT_USING_ADC) && \
    defined(BSP_USING_DAC) && defined(RT_USING_DAC)

#include <rtdevice.h>
#include "gd32h7xx.h"

#define DBG_TAG "app.loop"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

struct app_loop_state g_loop;

#define LOOP_ADC_DEV "adc2"
#define LOOP_ADC_CH  0
#define LOOP_DAC_DEV "dac"
#define LOOP_DAC_CH  1
#define OUT_MIN      0
#define OUT_MAX      4095
#define INTEG_LIMIT  200000L

static rt_adc_device_t loop_adc;
static rt_dac_device_t loop_dac;
static rt_uint32_t     clk_mhz = 600U;

/* DWT 周期计数 -> us */
static rt_uint32_t us_now(void)
{
    return (rt_uint32_t)(DWT->CYCCNT / clk_mhz);
}

static void loop_step(rt_int32_t e)
{
    rt_int32_t out;
    rt_int32_t d;

    /* 联锁：反馈越限 -> 安全输出 + 报警 */
    if (g_loop.feedback < g_loop.fb_lo || g_loop.feedback > g_loop.fb_hi) {
        g_loop.alarm |= 1U;
        g_loop.integ = 0;
        g_loop.prev_err = (rt_int16_t)e;
        out = g_loop.safe_output;
    } else {
        g_loop.alarm &= ~1U;
        d = e - g_loop.prev_err;
        g_loop.integ += e;
        if (g_loop.integ > INTEG_LIMIT)  g_loop.integ = INTEG_LIMIT;
        if (g_loop.integ < -INTEG_LIMIT) g_loop.integ = -INTEG_LIMIT;

        out = (rt_int32_t)g_loop.kp * e / 100
            + (rt_int32_t)g_loop.ki * g_loop.integ / 100
            + (rt_int32_t)g_loop.kd * d / 100;

        /* 输出限幅 + 反计算抗饱和 */
        if (out > OUT_MAX) { out = OUT_MAX; g_loop.integ -= e; }
        if (out < OUT_MIN) { out = OUT_MIN; g_loop.integ -= e; }
        g_loop.prev_err = (rt_int16_t)e;
    }

    if (out < OUT_MIN) out = OUT_MIN;
    if (out > OUT_MAX) out = OUT_MAX;

    g_loop.output = (rt_uint16_t)out;
    if (loop_dac) rt_dac_write(loop_dac, LOOP_DAC_CH, (rt_uint16_t)out);
}

static void loop_thread(void *param)
{
    rt_tick_t next = rt_tick_get();
    rt_uint32_t prev = us_now();
    rt_uint32_t dmin = 0xFFFFFFFFU, dmax = 0;

    (void)param;
    for (;;) {
        rt_uint32_t t = us_now();
        rt_uint32_t dt = t - prev;      /* 上一轮实际周期 */
        prev = t;
        if (dt) {
            if (dt < dmin) dmin = dt;
            if (dt > dmax) dmax = dt;
        }

        if (loop_adc) {
            g_loop.feedback = (rt_uint16_t)rt_adc_read(loop_adc, LOOP_ADC_CH);
        }
        {
            rt_int32_t e = (rt_int32_t)g_loop.setpoint - (rt_int32_t)g_loop.feedback;
            g_loop.error = (rt_int16_t)e;
            if (g_loop.enabled) {
                loop_step(e);
            } else {
                /* 未使能：只监视，不碰 DAC（DAC 由 DAC_CH1_SET 点手动控制） */
                g_loop.integ = 0;
                g_loop.prev_err = (rt_int16_t)e;
            }
            {
                rt_int32_t ae = (e < 0) ? -e : e;
                if (ae > g_loop.max_abs_err) g_loop.max_abs_err = (rt_uint16_t)ae;
            }
        }
        g_loop.iters++;
        g_loop.scan_us = dt;
        g_loop.jitter_us = (dmax >= dmin) ? (dmax - dmin) : 0;

        rt_thread_delay_until(&next, g_loop.period_ms ? g_loop.period_ms : 1);
    }
}

rt_err_t app_loop_init(void)
{
    rt_thread_t tid;

    if (g_loop.started) {
        return RT_EOK;
    }

    loop_adc = (rt_adc_device_t)rt_device_find(LOOP_ADC_DEV);
    loop_dac = (rt_dac_device_t)rt_device_find(LOOP_DAC_DEV);
    if (loop_adc) rt_adc_enable(loop_adc, LOOP_ADC_CH);
    if (loop_dac) rt_dac_enable(loop_dac, LOOP_DAC_CH);

    /* DWT 周期计数，用于 us 级周期/抖动统计 */
    {
        rt_uint32_t f = rcu_clock_freq_get(CK_SYS);
        clk_mhz = (f ? f : 600000000UL) / 1000000UL;
        if (clk_mhz == 0) clk_mhz = 600;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* 默认参数 */
    g_loop.enabled    = RT_FALSE;
    g_loop.setpoint   = 2000;
    g_loop.kp = 120; g_loop.ki = 5; g_loop.kd = 0;
    g_loop.period_ms  = 10;
    g_loop.safe_output = 0;
    g_loop.fb_lo = 30;
    g_loop.fb_hi = 4050;

    tid = rt_thread_create("pid", loop_thread, RT_NULL, 2048, 15, 5);
    if (tid == RT_NULL) {
        LOG_E("create control thread failed");
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);
    g_loop.started = RT_TRUE;
    LOG_I("control loop ready: %s ch%d -> %s ch%d, clk=%uMHz",
          LOOP_ADC_DEV, LOOP_ADC_CH, LOOP_DAC_DEV, LOOP_DAC_CH, (unsigned)clk_mhz);
    return RT_EOK;
}

void app_loop_reset(void)
{
    g_loop.integ = 0;
    g_loop.prev_err = 0;
    g_loop.max_abs_err = 0;
    g_loop.iters = 0;
    g_loop.alarm = 0;
    g_loop.jitter_us = 0;
}

#else  /* ADC/DAC 未同时开启 -> 提供空实现，避免链接错误 */

struct app_loop_state g_loop;

rt_err_t app_loop_init(void) { return -RT_ENOSYS; }
void     app_loop_reset(void) { }

#endif
