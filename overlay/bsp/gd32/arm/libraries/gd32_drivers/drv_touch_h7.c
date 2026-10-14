/*
 * GD32H759I-EVAL GT911 touch driver (software I2C) for RT-Thread
 *
 * 设计说明：
 *   官方 29_TLI_Touch_Draw demo 用 bit-bang I2C（SCL=PH7/SDA=PF9 开漏，
 *   RST=PF6/INT=PH6），本驱动按同一方案移植为 RT-Thread 版本：
 *   - 复位时序决定 7 位地址：RST 拉高时 INT 保持低 >5ms -> 0xBA/0x28
 *   - 16 位寄存器寻址：状态 0x814E、坐标 0x814F、ID 0x8140
 *   - 开漏输出读引脚不回切输入模式（GD32 输入通路常开，写 1 即释放）
 *   - demo 的 EXTI 宏有误（写 GPIOA/PIN5，实际 INT=PH6），本驱动采用
 *     轮询（10ms），触摸坐标的实时性需求远低于波形刷新，够用且稳
 *
 * 依赖：BSP_USING_TLI（touch_test 在屏上画触点；纯读坐标可单独用）
 */

#include <rthw.h>
#include <rtthread.h>

#if defined(BSP_USING_TOUCH)

#include "gd32h7xx_gpio.h"
#include <finsh.h>
#include <stdlib.h>

#define DBG_TAG             "drv.touch"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

/* ===================== 配置区 ===================== */

#define GTP_SCL_PORT        GPIOH
#define GTP_SCL_PIN         GPIO_PIN_7
#define GTP_SDA_PORT        GPIOF
#define GTP_SDA_PIN         GPIO_PIN_9
#define GTP_RST_PORT        GPIOF
#define GTP_RST_PIN         GPIO_PIN_6
#define GTP_INT_PORT        GPIOH
#define GTP_INT_PIN         GPIO_PIN_6

#define GTP_ADDRESS         0xBA        /* 8 位写地址（7 位 0x5D << 1 | R/W） */

#define GT911_ID_REG        0x8140      /* 产品 ID，应读出 "911" */
#define GT911_STATUS_REG    0x814E      /* bit7=有数据，bit3:0=触点数 */
#define GT911_POINT_REG     0x814F      /* 每点 8 字节：XL XH YL YH SL SH */
#define GT911_CFG_START     0x8047      /* 配置表起始地址 */

/* bit-bang 半周期延时：600MHz 下约 2~4us。TLI 的 DMA 常占总线后
 * GPIO 翻转延迟变大、时序余量变窄（实测 ID 第三字节 0x31 误读 0x7f），
 * 放宽到 ~150kHz 换稳定——GT911 对速率毫无要求 */
#define I2C_DELAY_LOOP      400

/* ===================== bit-bang I2C 底层 ===================== */

#define SCL_H()  gpio_bit_set(GTP_SCL_PORT, GTP_SCL_PIN)
#define SCL_L()  gpio_bit_reset(GTP_SCL_PORT, GTP_SCL_PIN)
#define SDA_H()  gpio_bit_set(GTP_SDA_PORT, GTP_SDA_PIN)
#define SDA_L()  gpio_bit_reset(GTP_SDA_PORT, GTP_SDA_PIN)
#define SDA_RD() gpio_input_bit_get(GTP_SDA_PORT, GTP_SDA_PIN)
#define SCL_RD() gpio_input_bit_get(GTP_SCL_PORT, GTP_SCL_PIN)

static void i2c_delay(void)
{
    volatile rt_uint32_t i = I2C_DELAY_LOOP;
    while (i--) __asm volatile ("nop");
}

/* 时钟延展处理：SCL_H 只是主机松手，从机忙时会把 SCL 摁在低电平
 * （GT911 在 TLI DMA 高负载下响应变慢，实测不处理会读出确定性
 * 错字节——ID 第三位 0x31 恒读成 0x7f）。必须等线真的变高 */
static void scl_release_wait(void)
{
    volatile rt_uint32_t timeout = 100000;

    SCL_H();
    while (SCL_RD() == RESET && timeout--)
        __asm volatile ("nop");
}

static void i2c_start(void)
{
    SDA_H(); SCL_H(); i2c_delay();
    SDA_L(); i2c_delay();
    SCL_L(); i2c_delay();
}

static void i2c_stop(void)
{
    SDA_L(); SCL_H(); i2c_delay();
    SDA_H(); i2c_delay();
}

/* 发送一字节并读 ACK，返回 SET=收到 ACK */
static FlagStatus i2c_sendbyte(rt_uint8_t byte)
{
    rt_int32_t i;
    FlagStatus ack;

    for (i = 7; i >= 0; i--)
    {
        if (byte & (1 << i)) SDA_H(); else SDA_L();
        i2c_delay();
        scl_release_wait(); i2c_delay();
        SCL_L();
    }
    SDA_H();                    /* 释放 SDA 给从机拉 ACK */
    i2c_delay();
    scl_release_wait();
    ack = (SDA_RD() == RESET) ? SET : RESET;
    i2c_delay();
    SCL_L();
    return ack;
}

/* 读一字节，ack=SET 发 ACK（还要读），RESET 发 NACK（最后一字节） */
static rt_uint8_t i2c_readbyte(FlagStatus ack)
{
    rt_int32_t i;
    rt_uint8_t byte = 0;

    SDA_H();                    /* 释放总线 */
    for (i = 7; i >= 0; i--)
    {
        scl_release_wait(); i2c_delay();
        if (SDA_RD()) byte |= (1 << i);
        SCL_L(); i2c_delay();
    }
    if (ack == SET) SDA_L(); else SDA_H();
    i2c_delay();
    SCL_H(); i2c_delay();
    SCL_L();
    SDA_H();
    return byte;
}

/* 写 16 位寄存器地址 + 数据 */
static rt_err_t gt911_write_reg(rt_uint16_t reg, const rt_uint8_t *buf,
                                rt_uint8_t len)
{
    rt_uint8_t i;

    i2c_start();
    if (i2c_sendbyte(GTP_ADDRESS) == RESET) goto _fail;
    if (i2c_sendbyte(reg >> 8) == RESET)   goto _fail;
    if (i2c_sendbyte(reg & 0xFF) == RESET) goto _fail;
    for (i = 0; i < len; i++)
        if (i2c_sendbyte(buf[i]) == RESET) goto _fail;
    i2c_stop();
    return RT_EOK;
_fail:
    i2c_stop();
    return -RT_EIO;
}

/* 读 16 位寄存器地址 + 数据 */
static rt_err_t gt911_read_reg(rt_uint16_t reg, rt_uint8_t *buf, rt_uint8_t len)
{
    rt_uint8_t i;

    i2c_start();
    if (i2c_sendbyte(GTP_ADDRESS) == RESET)     goto _fail;
    if (i2c_sendbyte(reg >> 8) == RESET)        goto _fail;
    if (i2c_sendbyte(reg & 0xFF) == RESET)      goto _fail;
    i2c_stop();

    i2c_start();
    if (i2c_sendbyte(GTP_ADDRESS | 0x01) == RESET) goto _fail;
    for (i = 0; i < len; i++)
        buf[i] = i2c_readbyte(i == len - 1 ? RESET : SET);
    i2c_stop();
    return RT_EOK;
_fail:
    i2c_stop();
    return -RT_EIO;
}

/* ===================== GT911 配置表（官方 demo 原值，480x272） ===================== */

static const rt_uint8_t gt911_cfg_params[] = {
0x5A,0xE0,0x01,0x10,0x01,0x01,0x8D,0x00,0x01,0x08,0x28,0x05,
0x55,0x32,0x03,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x89,0x2A,0x0B,0x17,0x15,0x31,0x0D,0x00,0x00,
0x02,0xB9,0x04,0x2C,0x00,0x00,0x00,0x00,0x00,0x03,0x64,0x32,
0x00,0x00,0x00,0x0F,0x94,0x94,0xC5,0x02,0x07,0x00,0x00,0x04,
0x8D,0x13,0x00,0x5C,0x1E,0x00,0x3C,0x30,0x00,0x29,0x4C,0x00,
0x20,0x78,0x00,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x02,0x04,0x06,0x08,0x0A,0x0C,0x0E,0x10,
0x12,0x14,0x16,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x24,0x22,
0x21,0x20,0x1F,0x1E,0x1D,0x1C,0x18,0x16,0x12,0x10,0x0F,0x0A,
0x08,0x06,0x04,0x02,0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
0xFF,0xFF,0xFF,0xFF,0x21,0x01
};

/* ===================== 对外接口 ===================== */

/* 读取第一点触摸坐标。返回 RT_EOK 且有触摸时 *x/*y 有效。
 * 协议要点：状态报告的 N 个触点必须全部读走再清 0x814E——只读部分
 * 触点芯片不接受清位，状态位挂死导致无触摸也疯狂重报（实测：上电
 * 残留状态 0xFF 报 15 点，只读 1 点清不掉；真触摸一次后触点数变 1
 * 恰好读完才恢复正常）。GT911 最多 5 点，>5 判非法帧丢弃 */
rt_err_t gt911_read_point(rt_uint16_t *x, rt_uint16_t *y)
{
    rt_uint8_t status, buf[40];
    rt_uint8_t clear = 0;
    rt_uint8_t n;

    if (gt911_read_reg(GT911_STATUS_REG, &status, 1) != RT_EOK)
        return -RT_EIO;

    if (status & 0x80)
    {
        n = status & 0x0F;
        if (n >= 1 && n <= 5)
        {
            if (gt911_read_reg(GT911_POINT_REG, buf, 8 * n) != RT_EOK)
                return -RT_EIO;
            /* 每点 8 字节布局：byte0=轨迹 ID，byte1/2=X 低/高，
             * byte3/4=Y 低/高，byte5/6=触摸面积。坐标从 byte1 起，
             * 从 byte0 起算会把 ID 拼进坐标低位（实测错出 x=3328） */
            *x = ((rt_uint16_t)buf[2] << 8) | buf[1];
            *y = ((rt_uint16_t)buf[4] << 8) | buf[3];
        }
        gt911_write_reg(GT911_STATUS_REG, &clear, 1);   /* 全量读完再清 */
        return (n >= 1 && n <= 5) ? RT_EOK : -RT_EEMPTY;
    }
    if (status)
        gt911_write_reg(GT911_STATUS_REG, &clear, 1);
    return -RT_EEMPTY;
}

/* ===================== 初始化 ===================== */

int rt_hw_touch_init(void)
{
    rt_uint8_t id[4];
    rt_uint8_t cfg[sizeof(gt911_cfg_params)];
    rt_uint8_t checksum = 0;
    rt_uint32_t i;

    /* GPIO：SCL/SDA 开漏输出，RST 推挽，INT 先推挽（复位时序用） */
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOH);
    gpio_mode_set(GTP_SCL_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GTP_SCL_PIN);
    gpio_output_options_set(GTP_SCL_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, GTP_SCL_PIN);
    gpio_mode_set(GTP_SDA_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, GTP_SDA_PIN);
    gpio_output_options_set(GTP_SDA_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_60MHZ, GTP_SDA_PIN);
    gpio_mode_set(GTP_RST_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLDOWN, GTP_RST_PIN);
    gpio_output_options_set(GTP_RST_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, GTP_RST_PIN);
    gpio_mode_set(GTP_INT_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLDOWN, GTP_INT_PIN);
    gpio_output_options_set(GTP_INT_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, GTP_INT_PIN);
    SCL_H(); SDA_H();

    /* 复位时序：RST 低 1ms（INT 保持低）-> RST 高 >5ms -> 地址定为 0xBA，
     * 之后 INT 回输入态 */
    gpio_bit_reset(GTP_RST_PORT, GTP_RST_PIN);
    gpio_bit_reset(GTP_INT_PORT, GTP_INT_PIN);
    rt_thread_mdelay(1);
    gpio_bit_set(GTP_RST_PORT, GTP_RST_PIN);
    rt_thread_mdelay(6);
    gpio_mode_set(GTP_INT_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GTP_INT_PIN);

    /* 读 ID 验证 I2C 链路。GT911 复位后到 I2C 可应答需要启动时间，
     * 50ms 实测不够（读出全 0xFF），放到 150ms 且重试间隔 50ms。
     * 判定放宽到前两字节 '9','1'；失败只警告不判死——运行时坐标
     * 读取已被证明独立可用，本校验仅为尽早暴露接线故障 */
    rt_thread_mdelay(150);
    {
        rt_int32_t retry = 3;
        rt_bool_t id_ok = RT_FALSE;

        while (retry--)
        {
            if (gt911_read_reg(GT911_ID_REG, id, 4) == RT_EOK &&
                id[0] == '9' && id[1] == '1')
            {
                id_ok = RT_TRUE;
                if (id[2] != '1')
                    LOG_W("gt911 id byte2 read 0x%x (expect 0x31), tolerated",
                          id[2]);
                break;
            }
            rt_thread_mdelay(50);
        }
        if (!id_ok)
        {
            LOG_W("gt911 id read failed (0x%x 0x%x 0x%x), continue anyway",
                  id[0], id[1], id[2]);
        }
    }

    /* 下载配置表（官方 demo 值），补校验和与生效标志 */
    rt_memcpy(cfg, gt911_cfg_params, sizeof(cfg));
    for (i = 0; i < sizeof(cfg) - 2; i++)
        checksum += cfg[i];
    cfg[sizeof(cfg) - 2] = (~checksum) + 1;
    cfg[sizeof(cfg) - 1] = 1;
    if (gt911_write_reg(GT911_CFG_START, cfg, sizeof(cfg)) != RT_EOK)
    {
        /* 出厂默认配置恰好匹配本面板（480x272），下载失败不判死 */
        LOG_W("gt911 config download failed, use factory defaults");
    }
    rt_thread_mdelay(10);

    /* 排干上电残留状态：复位后 0x814E 常为 0xFF 之类的脏值，
     * 读不清会在首次触摸前持续误报 */
    {
        rt_uint8_t retry = 5;
        rt_uint16_t dx, dy;
        while (retry-- && gt911_read_point(&dx, &dy) != -RT_EEMPTY)
            rt_thread_mdelay(5);
    }

    LOG_I("gt911 ready (id=%c%c%c, ver=0x%x)", id[0], id[1], id[2], id[3]);
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_touch_init);

/* ===================== 测试命令 ===================== */

#if defined(BSP_USING_TLI)
extern void tli_draw_point(rt_uint16_t x, rt_uint16_t y, rt_uint16_t color);
extern void tli_fill(rt_uint16_t color);
#endif

/* touch_id：运行时再读一次 ID——验证"init 读不到"只是上电时序问题 */
static void touch_id(int argc, char **argv)
{
    rt_uint8_t id[4];

    if (gt911_read_reg(GT911_ID_REG, id, 4) == RT_EOK)
        rt_kprintf("gt911 id: %c%c%c ver=0x%02x\n", id[0], id[1], id[2], id[3]);
    else
        rt_kprintf("gt911 id read failed\n");
}
MSH_CMD_EXPORT_ALIAS(touch_id, touch_id, read gt911 product id at runtime);

/* touch_test [seconds]：打印坐标并在屏上画点（默认 10 秒） */
static void touch_test(int argc, char **argv)
{
    rt_uint32_t rounds = 1000;      /* 10ms * 1000 = 10s */
    rt_uint16_t x, y;

    if (argc >= 2)
        rounds = atoi(argv[1]) * 100;

    rt_kprintf("touch the panel (draw mode)...\n");
#if defined(BSP_USING_TLI)
    tli_fill(0x0000);
#endif
    while (rounds--)
    {
        if (gt911_read_point(&x, &y) == RT_EOK)
        {
            rt_kprintf("x=%d y=%d\n", x, y);
#if defined(BSP_USING_TLI)
            if (x < 480 && y < 272)
            {
                tli_draw_point(x, y, 0xFFE0);
                tli_draw_point(x > 0 ? x - 1 : x, y, 0xFFE0);
                tli_draw_point(x, y > 0 ? y - 1 : y, 0xFFE0);
                tli_draw_point(x < 479 ? x + 1 : x, y, 0xFFE0);
                tli_draw_point(x, y < 271 ? y + 1 : y, 0xFFE0);
            }
#endif
        }
        rt_thread_mdelay(10);
    }
    rt_kprintf("touch_test done\n");
}
MSH_CMD_EXPORT_ALIAS(touch_test, touch_test,
                     read gt911 and draw on lcd: touch_test [seconds]);

#endif /* BSP_USING_TOUCH */