/*
 * CSV 数据记录仪：ADC2 采样 -> TF 卡 /adc_log.csv
 *
 * 串联两个战役的成果：
 *   - ADC 篇：rt_adc_enable / rt_adc_read（framework 路径，PC2_C 电位器）
 *   - SDIO 篇：sd0 自动挂载（app.sdmnt）+ DFS/POSIX 文件接口
 *
 * 用法（先确认 [I/app.sdmnt] mount sd0 on / ok）：
 *   adc_csv 1000 10       # 采 1000 点，间隔 10ms
 *   adc_csv 500           # 采 500 点，默认间隔 1ms
 *
 * CSV 格式：index,tick,raw,mv —— 电脑端 Excel/Python 直接打开画图。
 * 写卡节奏：每 32 行 fflush 一次（权衡掉电安全性与写放大）。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>

#define DBG_TAG             "app.csvlog"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define CSVLOG_ADC_DEV      "adc2"
#define CSVLOG_ADC_CH       0
#define CSVLOG_FILE         "/adc_log.csv"
#define CSVLOG_FLUSH_LINES  32
#define CSVLOG_VREF_MV      3300
#define CSVLOG_ADC_MAX      4095        /* ADC2 为 12-bit */

static void adc_csv(int argc, char **argv)
{
    rt_adc_device_t adc;
    FILE *fp;
    rt_uint32_t count, interval = 1, i;
    rt_uint32_t raw, raw_min = CSVLOG_ADC_MAX, raw_max = 0;
    rt_uint64_t raw_sum = 0;
    rt_tick_t t0;

    if (argc < 2)
    {
        rt_kprintf("usage: adc_csv <count> [interval_ms]\n");
        return;
    }
    count = atoi(argv[1]);
    if (count == 0)
    {
        rt_kprintf("count must be > 0\n");
        return;
    }
    if (argc >= 3)
        interval = atoi(argv[2]);

    /* 前置检查：SD 卡挂载与 ADC 设备（错误前置，别采到一半才发现） */
    if (rt_device_find("sd0") == RT_NULL)
    {
        rt_kprintf("sd0 not found - TF card missing? (sdprobe to re-detect)\n");
        return;
    }
    adc = (rt_adc_device_t)rt_device_find(CSVLOG_ADC_DEV);
    if (adc == RT_NULL)
    {
        rt_kprintf("%s not found - enable BSP_USING_ADC2\n", CSVLOG_ADC_DEV);
        return;
    }

    fp = fopen(CSVLOG_FILE, "w");
    if (fp == RT_NULL)
    {
        rt_kprintf("fopen %s failed - card mounted? filesystem FAT32?\n",
                   CSVLOG_FILE);
        return;
    }

    rt_adc_enable(adc, CSVLOG_ADC_CH);
    fprintf(fp, "index,tick,raw,mv\n");

    rt_kprintf("logging %u samples @ %u ms -> %s ...\n", count, interval,
               CSVLOG_FILE);
    t0 = rt_tick_get();

    for (i = 0; i < count; i++)
    {
        raw = rt_adc_read(adc, CSVLOG_ADC_CH);
        fprintf(fp, "%u,%u,%u,%u\n", i, rt_tick_get() - t0, raw,
                raw * CSVLOG_VREF_MV / CSVLOG_ADC_MAX);

        if (raw < raw_min) raw_min = raw;
        if (raw > raw_max) raw_max = raw;
        raw_sum += raw;

        if ((i + 1) % CSVLOG_FLUSH_LINES == 0)
            fflush(fp);
        if (interval)
            rt_thread_mdelay(interval);
    }

    fclose(fp);     /* fclose 自带 flush，最后的残余行不会丢 */

    rt_kprintf("done: %u samples in %u ms, raw min/avg/max = %u/%u/%u\n",
               count, rt_tick_get() - t0, raw_min,
               (rt_uint32_t)(raw_sum / count), raw_max);
}
MSH_CMD_EXPORT_ALIAS(adc_csv, adc_csv,
                     log ADC2 samples to /adc_log.csv: adc_csv <count> [interval_ms]);