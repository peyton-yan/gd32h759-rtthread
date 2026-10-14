/*
 * app_log.h — ADC 数据记录仪服务（写入 /adc_log.csv）
 * 点表用：使能/周期/累计行数。
 */
#ifndef APP_LOG_H
#define APP_LOG_H

#include <rtthread.h>

rt_err_t    app_log_set(rt_bool_t on);
void        app_log_set_period(rt_uint32_t ms);
rt_uint32_t app_log_period(void);
rt_uint32_t app_log_lines(void);
rt_bool_t   app_log_enabled(void);

#endif /* APP_LOG_H */
