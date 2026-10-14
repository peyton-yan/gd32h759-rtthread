/*
 * app_loop.h — DAC↔ADC 闭环控制回路（PID + 越限联锁 + 周期统计）
 *
 * 结构：上位机写设定值 -> 控制线程周期采样 ADC -> PID -> 写 DAC -> ADC 反馈
 * 硬件：DAC0_OUT1(PA5) --杜邦线--> ADC2_IN0(PC2_C)（板上电位器即负载/扰动）
 * 点表：0x0Dxx（见 mb_points.c）
 */
#ifndef APP_LOOP_H
#define APP_LOOP_H

#include <rtthread.h>

struct app_loop_state {
    /* 可写（上位机） */
    volatile rt_bool_t   enabled;      /* 闭环使能 */
    volatile rt_uint16_t setpoint;     /* 目标值（ADC 码 0..4095） */
    volatile rt_int16_t  kp, ki, kd;   /* 增益 ×100 */
    volatile rt_uint16_t period_ms;    /* 控制周期 */
    volatile rt_uint16_t safe_output;  /* 联锁安全输出 */
    volatile rt_uint16_t fb_lo, fb_hi; /* 反馈允许范围（越限->联锁） */

    /* 只读（遥测） */
    volatile rt_uint16_t feedback;     /* ADC 反馈 */
    volatile rt_uint16_t output;       /* DAC 输出 */
    volatile rt_int16_t  error;        /* 偏差 sp - fb */
    volatile rt_uint16_t alarm;        /* bit0: 反馈越限（联锁） */
    volatile rt_uint32_t scan_us;      /* 本次实测周期(us) */
    volatile rt_uint32_t jitter_us;    /* 周期峰-峰抖动(us) */
    volatile rt_uint32_t iters;        /* 迭代次数 */
    volatile rt_uint16_t max_abs_err;  /* |误差| 峰值 */

    /* 内部 */
    rt_int32_t integ;
    rt_int16_t prev_err;
    rt_bool_t  started;
};

extern struct app_loop_state g_loop;

/* 幂等：查找设备并创建控制线程 */
rt_err_t app_loop_init(void);
/* 清积分/峰值/抖动/告警 */
void     app_loop_reset(void);

#endif /* APP_LOOP_H */
