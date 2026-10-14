/*
 * GD32H7 DAC 驱动 —— RT-Thread DAC 框架
 * 本板使用: DAC0_OUT1 = PA5, 12-bit (经 JP71 到扩展口)
 *
 * 硬件要点:
 * 1. dac_mode_config(NORMAL_PIN_BUFFON) 必须配置, 否则输出不驱动外部引脚;
 * 2. DAC 一旦 enable 就驱动 PA5, 会与其他用途(GPIO/ADC 测试)抢引脚 ——
 *    框架路径下由 ops->enabled/disabled 负责占有/释放;
 * 3. 框架路径不用触发源, 写数据立即输出。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "gd32h7xx.h"

#define DBG_TAG "drv.dac"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#if defined(BSP_USING_DAC) && defined(SOC_SERIES_GD32H7xx)

static struct rt_dac_device h7_dac_device;

/* channel 约定: 1 = DAC0_OUT1 (PA5) */

static rt_err_t h7_dac_enabled(struct rt_dac_device *device, rt_uint32_t channel)
{
    if (channel != 1) return -RT_EINVAL;

    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_DAC);

    gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_5);

    dac_deinit(DAC0);
    dac_trigger_disable(DAC0, DAC_OUT1);
    dac_wave_mode_config(DAC0, DAC_OUT1, DAC_WAVE_DISABLE);
    /* 注意: 不要调 dac_mode_config —— 实测它会把输出写死(踩坑记录) */
    dac_enable(DAC0, DAC_OUT1);

    LOG_I("DAC0_OUT1 enabled (PA5, 12-bit)");
    return RT_EOK;
}

static rt_err_t h7_dac_disabled(struct rt_dac_device *device, rt_uint32_t channel)
{
    if (channel != 1) return -RT_EINVAL;
    dac_disable(DAC0, DAC_OUT1);
    return RT_EOK;
}

static rt_err_t h7_dac_convert(struct rt_dac_device *device, rt_uint32_t channel, rt_uint32_t *value)
{
    if (channel != 1) return -RT_EINVAL;
    dac_data_set(DAC0, DAC_OUT1, DAC_ALIGN_12B_R, (uint16_t)(*value & 0x0FFFU));
    return RT_EOK;
}

static rt_uint8_t h7_dac_get_resolution(struct rt_dac_device *device)
{
    return 12;
}

static const struct rt_dac_ops h7_dac_ops = {
    .disabled       = h7_dac_disabled,
    .enabled        = h7_dac_enabled,
    .convert        = h7_dac_convert,
    .get_resolution = h7_dac_get_resolution,
};

static int rt_hw_dac_init(void)
{
    return rt_hw_dac_register(&h7_dac_device, "dac", &h7_dac_ops, RT_NULL);
}
INIT_DEVICE_EXPORT(rt_hw_dac_init);

#endif /* BSP_USING_DAC && SOC_SERIES_GD32H7xx */