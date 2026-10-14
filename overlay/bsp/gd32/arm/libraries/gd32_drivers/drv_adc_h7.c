/*
 * GD32H7 ADC 驱动 —— RT-Thread ADC 框架
 * =============================================================================
 * 【移植指南】换 ADC 实例 / 换引脚, 只需修改下方"移植配置区", 驱动主体不动:
 *
 *   1. 查数据手册引脚功能表, 确认目标引脚属于哪个 ADC 的哪个通道。
 *      本板实测映射:
 *        PC2_C = ADC2_IN0   (本板默认, VR1 电位器所在)
 *        PA5   = ADC01_IN19 (与 DAC0_OUT1 复用)
 *        PA4   = ADC01_IN18 (与 DAC0_OUT0 复用)
 *      注意: ADC0/ADC1 是 14-bit, ADC2 是 12-bit, 三者通道表不通用!
 *   2. 修改配置区 5 个宏。
 *   3. 若引脚是 PC2_C / PC3_C / PA0_C / PA1_C 这类"专用模拟焊盘"
 *      (数据手册标注 can only be used as analog pins), 设 ADC_PIN_IS_DEDICATED=1,
 *      跳过 GPIO 配置; 普通引脚设 0 并填好 GPIO 三要素。
 *      另: 不要动 SYSCFG 的 PCx_C 模拟开关, 复位默认即正确(踩坑记录见笔记)。
 *   4. menuconfig 打开 BSP_USING_ADC2, 重新编译。
 *
 * 【框架语义要点(血泪)】
 *   - rt_hw_adc_register 只注册, 框架不会替你初始化硬件;
 *     初始化必须挂 ops->enabled, 或在 convert 里惰性调用(本驱动两者都做)。
 *   - 若 DMA 触发路径跑过, ADC 会残留"外部触发+DMA 请求"状态,
 *     软件触发被静默忽略 -> convert 开头必须回收状态。
 * =============================================================================
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "gd32h7xx.h"

#define DBG_TAG "drv.adc"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#if defined(BSP_USING_CAN) && defined(SOC_SERIES_GD32H7xx)

/* ============================ 移植配置区 ============================ */
#define ADC_INSTANCE            ADC2            /* ADC0 / ADC1 / ADC2  */
#define ADC_RCU                 RCU_ADC2        /* 对应实例的时钟      */
#define ADC_CHANNEL             ADC_CHANNEL_0   /* 引脚对应的通道号    */
#define ADC_DEVICE_NAME         "adc2"          /* 注册到框架的设备名  */
#define ADC_PIN_IS_DEDICATED    1               /* 1=专用模拟焊盘, 0=普通GPIO */
/* 普通引脚才需要填的（ADC_PIN_IS_DEDICATED=0 时生效）: */
#define ADC_GPIO_RCU            RCU_GPIOC
#define ADC_GPIO_PORT           GPIOC
#define ADC_GPIO_PIN            GPIO_PIN_2
/* ADC 内核时钟: 同步模式 HCLK 分频, 须在实例允许范围内 */
#define ADC_CLOCK               ADC_CLK_SYNC_HCLK_DIV6
/* 分辨率: ADC2 最高 ADC_RESOLUTION_12B; ADC0/1 可选 ADC_RESOLUTION_14B */
#define ADC_RESOLUTION          ADC_RESOLUTION_12B
/* 规则组采样时间(ADC 时钟周期数): 信号源内阻越大取值越大 */
#define ADC_SAMPLE_CYCLES       480
/* ==================================================================== */

static struct rt_adc_device h7_adc_device;
static rt_bool_t g_adc_inited = RT_FALSE;

static void h7_adc_hw_init(void)
{
    if (g_adc_inited) return;

    rcu_periph_clock_enable(ADC_RCU);
#if !ADC_PIN_IS_DEDICATED
    /* 普通引脚: 配为纯模拟输入, 断开数字通路 */
    rcu_periph_clock_enable(ADC_GPIO_RCU);
    gpio_mode_set(ADC_GPIO_PORT, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, ADC_GPIO_PIN);
#endif

    adc_deinit(ADC_INSTANCE);
    adc_clock_config(ADC_INSTANCE, ADC_CLOCK);
    adc_special_function_config(ADC_INSTANCE, ADC_SCAN_MODE, DISABLE);
    adc_special_function_config(ADC_INSTANCE, ADC_CONTINUOUS_MODE, DISABLE);
    adc_resolution_config(ADC_INSTANCE, ADC_RESOLUTION);
    adc_data_alignment_config(ADC_INSTANCE, ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(ADC_INSTANCE, ADC_REGULAR_CHANNEL, 1);
    adc_external_trigger_config(ADC_INSTANCE, ADC_REGULAR_CHANNEL, EXTERNAL_TRIGGER_DISABLE);

    adc_enable(ADC_INSTANCE);
    /* 官方例程标准校准序列: 使能后等 1ms 稳定, 再做偏移校准 */
    rt_thread_mdelay(1);
    adc_calibration_mode_config(ADC_INSTANCE, ADC_CALIBRATION_OFFSET);
    adc_calibration_number(ADC_INSTANCE, ADC_CALIBRATION_NUM1);
    adc_calibration_enable(ADC_INSTANCE);

    g_adc_inited = RT_TRUE;
    LOG_I("%s init done (ch %d)", ADC_DEVICE_NAME, (int)ADC_CHANNEL);
}

static rt_err_t h7_adc_enabled(struct rt_adc_device *device, rt_int8_t channel, rt_bool_t enabled)
{
    if (channel != 0) return -RT_EINVAL;
    if (enabled) {
        h7_adc_hw_init();
    } else {
        adc_disable(ADC_INSTANCE);
        g_adc_inited = RT_FALSE;    /* 下次 enable 时重新完整初始化 */
    }
    return RT_EOK;
}

static rt_err_t h7_adc_convert(struct rt_adc_device *device, rt_int8_t channel, rt_uint32_t *value)
{
    rt_tick_t start;

    if (channel != 0) return -RT_EINVAL;

    h7_adc_hw_init();               /* 惰性初始化, 一次性守卫保证只跑一次 */

    /* 回收 DMA/触发路径可能留下的状态, 否则软件触发被静默忽略 */
    adc_external_trigger_config(ADC_INSTANCE, ADC_REGULAR_CHANNEL, EXTERNAL_TRIGGER_DISABLE);
    adc_special_function_config(ADC_INSTANCE, ADC_CONTINUOUS_MODE, DISABLE);
    adc_dma_mode_disable(ADC_INSTANCE);

    adc_regular_channel_config(ADC_INSTANCE, 0, ADC_CHANNEL, ADC_SAMPLE_CYCLES);
    adc_flag_clear(ADC_INSTANCE, ADC_FLAG_EOC);
    adc_software_trigger_enable(ADC_INSTANCE, ADC_REGULAR_CHANNEL);

    start = rt_tick_get();
    while (adc_flag_get(ADC_INSTANCE, ADC_FLAG_EOC) == RESET) {
        if (rt_tick_get() - start > rt_tick_from_millisecond(100)) {
            LOG_E("convert timeout, STAT=0x%08lx", ADC_STAT(ADC_INSTANCE));
            return -RT_ETIMEOUT;
        }
    }
    *value = adc_regular_data_read(ADC_INSTANCE);
    return RT_EOK;
}

static const struct rt_adc_ops h7_adc_ops = {
    .enabled  = h7_adc_enabled,
    .convert  = h7_adc_convert,
};

static int rt_hw_adc_port_init(void)
{
    return rt_hw_adc_register(&h7_adc_device, ADC_DEVICE_NAME, &h7_adc_ops, RT_NULL);
}
INIT_DEVICE_EXPORT(rt_hw_adc_port_init);

#endif /* BSP_USING_ADC2 */