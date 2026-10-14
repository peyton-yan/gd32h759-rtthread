/*
 * app_log.c — ADC 数据记录仪：使能后按周期把 adc2 CH0 写入 /adc_log.csv
 * 文件按行走，累计行数可查。挂载点由系统决定（SD 或 littlefs 都在 /）。
 */
#include "app_log.h"

#if defined(RT_USING_DFS)

#include <rtdevice.h>
#include <stdio.h>

#define LOG_ADC_DEV   "adc2"
#define LOG_ADC_CH    0
#define LOG_FILE      "/adc_log.csv"
#define LOG_VREF_MV   3300
#define LOG_ADC_MAX   4095

static volatile rt_bool_t enabled;
static volatile rt_uint32_t period_ms = 100;
static volatile rt_uint32_t lines;
static rt_thread_t tid;

static void app_log_thread(void *param)
{
    FILE *fp = RT_NULL;
    rt_adc_device_t adc = (rt_adc_device_t)rt_device_find(LOG_ADC_DEV);

    (void)param;
    if (adc != RT_NULL)
        rt_adc_enable(adc, LOG_ADC_CH);

    for (;;)
    {
        if (!enabled)
        {
            if (fp) { fclose(fp); fp = RT_NULL; }
            rt_thread_mdelay(100);
            continue;
        }
        if (fp == RT_NULL)
        {
            fp = fopen(LOG_FILE, "a");
            lines = 0;
            if (fp == RT_NULL)
            {
                enabled = RT_FALSE;     /* 无文件系统/卡，自动关闭 */
                rt_thread_mdelay(200);
                continue;
            }
            fprintf(fp, "index,tick,raw,mv\n");
        }
        if (adc != RT_NULL)
        {
            rt_uint32_t raw = rt_adc_read(adc, LOG_ADC_CH);
            fprintf(fp, "%u,%u,%u,%u\n", lines, rt_tick_get(), raw,
                    raw * LOG_VREF_MV / LOG_ADC_MAX);
            lines++;
            if ((lines % 32) == 0)
                fflush(fp);
        }
        rt_thread_mdelay(period_ms ? period_ms : 1);
    }
}

rt_err_t app_log_set(rt_bool_t on)
{
    if (on && tid == RT_NULL)
    {
        tid = rt_thread_create("adclog", app_log_thread, RT_NULL, 2048, 20, 10);
        if (tid == RT_NULL)
            return -RT_ENOMEM;
        rt_thread_startup(tid);
    }
    enabled = on ? RT_TRUE : RT_FALSE;
    return RT_EOK;
}

void app_log_set_period(rt_uint32_t ms)
{
    if (ms == 0) ms = 1;
    period_ms = ms;
}

rt_uint32_t app_log_period(void)  { return period_ms; }
rt_uint32_t app_log_lines(void)   { return lines; }
rt_bool_t   app_log_enabled(void) { return enabled; }

#else  /* !RT_USING_DFS */

rt_err_t    app_log_set(rt_bool_t on)   { (void)on; return -RT_ENOSYS; }
void        app_log_set_period(rt_uint32_t ms) { (void)ms; }
rt_uint32_t app_log_period(void)  { return 0; }
rt_uint32_t app_log_lines(void)   { return 0; }
rt_bool_t   app_log_enabled(void) { return RT_FALSE; }

#endif /* RT_USING_DFS */
