/*
 * rtc_test.c - RTC 测试（drv_rtc_h7.c 配套）
 *
 * 设置/读取直接用 finsh 内置 date 命令（RT_USING_RTC 自动获得）：
 *   date                          显示当前时间
 *   date 2026 9 16 12 0 0         设置时间
 *
 * 本文件提供：
 *   rtc_watch [秒数]   连续打印走时（默认 10 秒），人眼对表验证 1Hz
 *   rtc_smoke          走时冒烟测试：连读两次间隔 2 秒，差值必须恰好 +2
 *
 * 无纽扣电池场景验证法：
 *   1. date 设置时间 -> rtc_watch 对表（走时准确性）；
 *   2. 按板载 RESET 键（VDD 不断）-> 重启后 date 应继续走且时间正确
 *      （日志应见 "rtc warm start"）——证明后备域+magic 机制正确；
 *   3. 断电重上电 -> 回到默认 2026-01-01（"rtc cold start"），
 *      这是没电池的正常现象，不是 bug。
 */

#include <rtthread.h>
#include <rtdevice.h>

#if defined(BSP_USING_RTC)

#include <time.h>
#include <stdlib.h>

/* rt_rtc_get_secs 在 v5.2.2 未由 rtdevice.h 导出声明，
 * 用 RT_DEVICE_CTRL_RTC_GET_TIME 这条各版本稳定存在的路径 */
static rt_err_t rtc_get_secs(time_t *secs)
{
    rt_device_t dev = rt_device_find("rtc");
    if (dev == RT_NULL)
        return -RT_ENOENT;
    return rt_device_control(dev, RT_DEVICE_CTRL_RTC_GET_TIME, secs);
}

static void rtc_watch(int argc, char **argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 10;
    time_t now;
    struct tm *tm;

    while (n-- > 0)
    {
        if (rtc_get_secs(&now) == RT_EOK)
        {
            tm = localtime(&now);
            rt_kprintf("%04d-%02d-%02d %02d:%02d:%02d\n",
                       tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                       tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
        rt_thread_mdelay(1000);
    }
}
MSH_CMD_EXPORT(rtc_watch, rtc_watch [seconds] - print time every 1s);

static void rtc_smoke(int argc, char **argv)
{
    time_t t0, t1;

    rtc_get_secs(&t0);
    rt_thread_mdelay(2000);
    rtc_get_secs(&t1);

    if (t1 - t0 == 2)
        rt_kprintf("rtc_smoke PASS (t1-t0=%ld, expect 2)\n", (long)(t1 - t0));
    else
        rt_kprintf("rtc_smoke FAIL (t1-t0=%ld, expect 2)\n", (long)(t1 - t0));
}
MSH_CMD_EXPORT(rtc_smoke, verify rtc ticks exactly 1Hz);

#endif /* BSP_USING_RTC */