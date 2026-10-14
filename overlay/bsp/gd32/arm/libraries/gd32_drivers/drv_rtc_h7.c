/*
 * drv_rtc_h7.c - GD32H7xx RTC 驱动（RT-Thread RTC 设备框架）
 *
 * H7 的 RTC 是 STM32 风格的日历型 RTC（影子寄存器 + 初始化模式 +
 * 写保护钥匙），与 F4 老 GD32 的计数器式 RTC 完全不同，单独成文。
 *
 * 设计要点：
 * 1. 时钟源策略：优先 LXTAL（板载 32.768kHz，PC14/PC15），起振失败
 *    自动退 IRC32K——没焊晶振的板子也能用，精度差点而已；
 * 2. 无纽扣电池场景：RTC 只要 VDD 在就走，复位不丢（后备域不断电），
 *    断电才丢。用 RTC_BKP0 里的 magic 区分"冷启动"（初始化日历）
 *    与"复位重启"（保留原时间继续走）；
 * 3. 时间换算：UTC 秒数 <-> 分解时间用 Howard Hinnant 的
 *    days-from-civil 算法，不依赖 newlib 的 mktime（时区陷阱）；
 * 4. 读时间用影子寄存器（默认），读前先 register_sync_wait；
 *    写时间必须走"写保护钥匙 -> 初始化模式 -> 写 -> 退出"全流程。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_RTC)

#define DBG_TAG             "drv.rtc"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define RTC_BKP_MAGIC       0x52AC2026U     /* 后备寄存器 magic：日历已初始化 */
#define LXTAL_STB_TIMEOUT   0x000FFFFFU     /* LXTAL 起振等待预算 */

static struct rt_rtc_device rtc_dev;

/* ---------- BCD <-> BIN ---------- */
static rt_uint8_t bin2bcd(rt_uint8_t v) { return (rt_uint8_t)(((v / 10) << 4) | (v % 10)); }
static rt_uint8_t bcd2bin(rt_uint8_t v) { return (rt_uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

/* ---------- civil <-> days（Howard Hinnant 算法，公历，epoch=1970-01-01） ---------- */
static rt_int32_t days_from_civil(rt_int32_t y, rt_uint32_t m, rt_uint32_t d)
{
    rt_int32_t era, yoe, doy, doe;
    y -= (m <= 2) ? 1 : 0;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                    /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;   /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

static void civil_from_days(rt_int32_t z, rt_int32_t *y, rt_uint32_t *m, rt_uint32_t *d)
{
    rt_int32_t era, doe, yoe, y_, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;                                 /* [0, 146096] */
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y_  = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);          /* [0, 365] */
    mp  = (5 * doy + 2) / 153;                              /* [0, 11] */
    *d  = doy - (153 * mp + 2) / 5 + 1;                     /* [1, 31] */
    *m  = mp + (mp < 10 ? 3 : -9);                          /* [1, 12] */
    *y  = y_ + (*m <= 2 ? 1 : 0);
}

/* ---------- 时钟源：LXTAL 优先，IRC32K 兜底 ---------- */
static void rtc_clock_config(void)
{
    rcu_periph_clock_enable(RCU_PMU);
    pmu_backup_write_enable();          /* 后备域写解锁（BKPWEN） */

    /* 试 LXTAL：使能 + 等起振 */
    rcu_osci_on(RCU_LXTAL);
    if (rcu_osci_stab_wait(RCU_LXTAL) == SUCCESS)
    {
        rcu_rtc_clock_config(RCU_RTCSRC_LXTAL);
        LOG_I("rtc clock: LXTAL 32.768kHz");
    }
    else
    {
        /* 没晶振/起振失败：IRC32K 兜底（精度 ±几个百分点，能走就行） */
        rcu_osci_on(RCU_IRC32K);
        rcu_osci_stab_wait(RCU_IRC32K);
        rcu_rtc_clock_config(RCU_RTCSRC_IRC32K);
        LOG_W("rtc clock: LXTAL failed, fallback to IRC32K (low precision)");
    }
    rcu_periph_clock_enable(RCU_RTC);
    rtc_register_sync_wait();
}

/* ---------- RT-Thread RTC ops ---------- */

static rt_err_t h7_rtc_init(void)
{
    rtc_clock_config();

    if (RTC_BKP0 != RTC_BKP_MAGIC)
    {
        /* 冷启动（断电丢过后备域）：初始化预分频 + 一个默认时间，
         * 32768Hz -> 127(asyn) x 255(syn) = 1Hz */
        rtc_parameter_struct para = { 0 };
        para.factor_asyn = 127;
        para.factor_syn  = 255;
        para.display_format = RTC_24HOUR;
        para.am_pm = 0;
        para.year = bin2bcd(26); para.month = bin2bcd(1); para.date = bin2bcd(1);
        para.day_of_week = 4;    /* 2026-01-01 周四 */
        para.hour = 0; para.minute = 0; para.second = 0;
        if (rtc_init(&para) == ERROR)
        {
            LOG_E("rtc_init failed");
            return -RT_ERROR;
        }
        RTC_BKP0 = RTC_BKP_MAGIC;
        LOG_I("rtc cold start: calendar initialized to 2026-01-01");
    }
    else
    {
        LOG_I("rtc warm start: calendar kept across reset");
    }
    return RT_EOK;
}

static rt_err_t h7_rtc_get_secs(time_t *secs)
{
    rtc_parameter_struct now;
    rt_int32_t days;

    rtc_register_sync_wait();
    rtc_current_time_get(&now);
    days = days_from_civil(2000 + bcd2bin(now.year),
                           bcd2bin(now.month), bcd2bin(now.date));
    *secs = (time_t)days * 86400 +
            bcd2bin(now.hour) * 3600 +
            bcd2bin(now.minute) * 60 +
            bcd2bin(now.second);
    return RT_EOK;
}

static rt_err_t h7_rtc_set_secs(time_t *secs)
{
    rtc_parameter_struct para;
    rt_int32_t days, y;
    rt_uint32_t m, d, rem;

    days = (rt_int32_t)(*secs / 86400);
    rem  = (rt_uint32_t)(*secs % 86400);
    civil_from_days(days, &y, &m, &d);

    para.factor_asyn = 127;
    para.factor_syn  = 255;
    para.display_format = RTC_24HOUR;
    para.am_pm = 0;
    para.year   = bin2bcd((rt_uint8_t)(y - 2000));
    para.month  = bin2bcd((rt_uint8_t)m);
    para.date   = bin2bcd((rt_uint8_t)d);
    /* 1970-01-01 是周四：wd = (days + 4) % 7，0=周日 -> RTC 的 1..7（一..日） */
    para.day_of_week = (rt_uint8_t)(((days + 4) % 7 + 7) % 7);
    para.day_of_week = para.day_of_week ? para.day_of_week : 7;
    para.hour   = bin2bcd((rt_uint8_t)(rem / 3600));
    para.minute = bin2bcd((rt_uint8_t)((rem % 3600) / 60));
    para.second = bin2bcd((rt_uint8_t)(rem % 60));

    /* rtc_init 内部走完整流程：写保护钥匙 -> 初始化模式 -> 写 -> 退出 */
    if (rtc_init(&para) == ERROR)
        return -RT_ERROR;
    RTC_BKP0 = RTC_BKP_MAGIC;
    return RT_EOK;
}

/* v5.2.2 的 RTC 框架：ops 结构体叫 rt_rtc_ops（见 drivers/dev_rtc.h），
 * 注册函数是 rt_hw_rtc_register（旧版的 rt_rtc_dev_register 早已改名） */
static const struct rt_rtc_ops h7_rtc_ops =
{
    .init        = h7_rtc_init,
    .get_secs    = h7_rtc_get_secs,
    .set_secs    = h7_rtc_set_secs,
    .get_alarm   = RT_NULL,
    .set_alarm   = RT_NULL,
    .get_timeval = RT_NULL,
    .set_timeval = RT_NULL,
};

int rt_hw_rtc_init(void)
{
    rtc_dev.ops = &h7_rtc_ops;
    return rt_hw_rtc_register(&rtc_dev, "rtc", RT_DEVICE_FLAG_RDWR, RT_NULL);
}
INIT_DEVICE_EXPORT(rt_hw_rtc_init);

#endif /* BSP_USING_RTC */