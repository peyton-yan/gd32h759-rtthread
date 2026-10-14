/*
 * systick.h —— 官方 demo 移植用垫片
 *
 * demo 的裸机 systick 延时在 RT-Thread 里没有，
 * 映射成线程延时，让 spi_quad_lcd_driver.c 一字节都不用改。
 */
#ifndef __SYSTICK_H
#define __SYSTICK_H

#include <rtthread.h>

#define delay_1ms(x)    rt_thread_mdelay(x)

#endif /* __SYSTICK_H */