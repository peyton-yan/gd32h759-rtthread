/*
 * drv_i2c_h7.c - GD32H7xx 硬件 I2C 主机驱动（RT-Thread I2C 框架，多实例）
 *
 * H7 的 I2C 是全新 IP（TIMING/RDATA/TDATA/RELOAD/AUTOEND），
 * 与 GD32F4 系列老 IP 不兼容，无法沿用旧 drv_i2c.c，故单独成文。
 *
 * 架构：传输引擎一份（h7_i2c_xfer），实例四份（I2C0~I2C3）——
 * 每个实例 = 外设基地址 + RCU 时钟 + SCL/SDA 引脚三元组（端口/脚位/AF 号），
 * 全部集中在 i2c_config[] 配置表，Kconfig 的 BSP_USING_I2Cx 逐个使能。
 * 换板子只改表，不动引擎。
 *
 * 传输设计决策：
 * 1. 轮询而非 DMA：I2C 外设单笔传输通常几到几十字节，DMA 通道申请 +
 *    cache 维护的成本远超收益，轮询还天然规避 M7 D-cache 一致性问题。
 * 2. TIMING 寄存器不填魔法数：init 时按 rcu_clock_freq_get(CK_APB1) 实算
 *    PSC/SCLH/SCLL，时钟树改了不用回来改驱动。
 * 3. RT-Thread 消息模型忠实映射到 RELOAD/AUTOEND：
 *    - 非最后一条消息            -> RELOAD（发完保持总线，等下一阶段）
 *    - 最后一条且不带 NO_STOP    -> AUTOEND（发完硬件自动 STOP）
 *    每条消息都置 START：总线被 RELOAD 保持时再置 START 即 repeat-start，
 *    AT24 随机读（写地址 -> 重启 -> 读数据）正是这个形态。
 * 4. NACK 是信息不是崩溃：从机不应答返回 -RT_EIO，设备探测/ACK polling 靠它。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_I2C0) || defined(BSP_USING_I2C1) || \
    defined(BSP_USING_I2C2) || defined(BSP_USING_I2C3)

#define DBG_TAG             "drv.i2c"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

/* 传输超时：100kHz 下单字节含 ACK 约 90us，给 10ms/字节 + 10ms 底数 */
#define I2C_TIMEOUT_BASE_MS     10
#define I2C_TIMEOUT_PER_BYTE_MS 10

/* TIMING 寄存器域宽 */
#define TIMING_PSC_MAX      15U
#define TIMING_SCL_MAX      255U

/* 一组 I2C 引脚描述：端口 RCU、GPIO 端口、脚位、AF 号 */
struct h7_i2c_pin
{
    rcu_periph_enum rcu;
    uint32_t        port;
    uint32_t        pin;
    uint32_t        af;
};

struct h7_i2c_config
{
    const char       *name;
    uint32_t          periph;
    rcu_periph_enum   rcu;
    struct h7_i2c_pin scl;
    struct h7_i2c_pin sda;
};

struct h7_i2c_bus
{
    struct rt_i2c_bus_device parent;
    const struct h7_i2c_config *cfg;
};

/*
 * 实例配置表：换板子/换引脚只动这里。
 *
 * 诚实声明（博客读者注意）：本表只收录在 GD32H759 数据手册 AF 表中
 * 确证过的 引脚+AF 组合。EVAL 板上只有 I2C1 接了真实器件（AT24C02），
 * 其余实例的引脚基本都被板载功能占用（I2C0 的 PB6=OSPI0_NCS、PB7=LED），
 * 因此 I2C0/2/3 在本板只验证"引擎可挂载"，电气连通需自画板或飞线。
 * 候选引脚（手册 AF 表确证）：
 *   I2C0: SCL=PB6/PB8(AF4)   SDA=PB7/PB9(AF4)
 *   I2C2: SCL=PA8(AF4)/PH7   SDA=PC9(AF4)/PH8
 *   I2C3: SCL=PB6/PB8(AF6)   SDA=PB7/PB9(AF6)   ← 注意 I2C3 是 AF6 不是 AF4
 * 自画板启用某实例时，把对应三元组换成自己的引脚即可，引擎零改动。
 */
static const struct h7_i2c_config i2c_config[] =
{
#ifdef BSP_USING_I2C0
    {
        "i2c0", I2C0, RCU_I2C0,
        .scl = { RCU_GPIOB, GPIOB, GPIO_PIN_6, GPIO_AF_4 },
        .sda = { RCU_GPIOB, GPIOB, GPIO_PIN_7, GPIO_AF_4 },
    },
#endif
#ifdef BSP_USING_I2C1
    {
        "i2c1", I2C1, RCU_I2C1,
        .scl = { RCU_GPIOH, GPIOH, GPIO_PIN_4, GPIO_AF_4 },   /* AT24C02 */
        .sda = { RCU_GPIOB, GPIOB, GPIO_PIN_11, GPIO_AF_4 },
    },
#endif
#ifdef BSP_USING_I2C2
    {
        "i2c2", I2C2, RCU_I2C2,
        .scl = { RCU_GPIOA, GPIOA, GPIO_PIN_8, GPIO_AF_4 },
        .sda = { RCU_GPIOC, GPIOC, GPIO_PIN_9, GPIO_AF_4 },
    },
#endif
#ifdef BSP_USING_I2C3
    {
        "i2c3", I2C3, RCU_I2C3,
        .scl = { RCU_GPIOB, GPIOB, GPIO_PIN_8, GPIO_AF_6 },
        .sda = { RCU_GPIOB, GPIOB, GPIO_PIN_9, GPIO_AF_6 },
    },
#endif
};

static struct h7_i2c_bus i2c_buses[sizeof(i2c_config) / sizeof(i2c_config[0])];

/* ---------------- 底层等待原语（全部带超时） ---------------- */

/* I2C_TRACE=1：失败点现场取证（调试期用，稳定后关 0） */
#define I2C_TRACE 0  /* 调试取证开关：1=失败点打印现场 */

static rt_err_t i2c_wait_flag_set(uint32_t periph, uint32_t flag,
                                  rt_tick_t deadline, const char *stage)
{
    while (!(I2C_STAT(periph) & flag))
    {
        if (rt_tick_get() > deadline)
        {
#if I2C_TRACE
            rt_kprintf("[i2c] TIMEOUT @%s STAT=0x%08lx CTL1=0x%08lx\n",
                       stage, I2C_STAT(periph), I2C_CTL1(periph));
#endif
            return -RT_ETIMEOUT;
        }
    }
    return RT_EOK;
}

/* 总线恢复（仅 _err 逃生通道调用）：
 * 教训——STOP 不能乱补：空闲总线上补 STOP 会产生多余 STPDET，
 * 且 STOP 位未执行完时下一条消息就置 START，硬件把排队中的 STOP
 * 夹在传输中途执行，传输被腰斩（扫描后紧跟的写必死案）。
 * 规矩：仅总线真忙才补 STOP；补完必须等 STOP 位自清（硬件发完才清），
 * 并清掉这次 STOP 产生的 STPDET，不把状态垃圾留给下一条消息。 */
static void i2c_bus_recover(uint32_t periph)
{
    rt_tick_t deadline;

    I2C_STATC(periph) = I2C_STAT(periph) & (I2C_STAT_NACK | I2C_STAT_BERR |
                                            I2C_STAT_LOSTARB |
                                            I2C_STAT_TIMEOUT |
                                            I2C_STAT_STPDET);
    i2c_reload_disable(periph);

    if (I2C_STAT(periph) & I2C_STAT_I2CBSY)
    {
        deadline = rt_tick_get() + rt_tick_from_millisecond(I2C_TIMEOUT_BASE_MS);
        I2C_CTL1(periph) |= I2C_CTL1_STOP;
        while (I2C_CTL1(periph) & I2C_CTL1_STOP)   /* STOP 发完硬件自清 */
        {
            if (rt_tick_get() > deadline)
                break;
        }
        I2C_STATC(periph) = I2C_STATC_STPDETC;     /* 这发 STOP 的 STPDET 也清掉 */
    }
}

/* 检查错误标志（NACK/BERR/LOSTARB/TIMEOUT），只报告不处理——
 * 调用者一律 goto _err，由 i2c_bus_recover 统一收拾现场。 */
static rt_err_t i2c_check_error(uint32_t periph, const char *stage)
{
    uint32_t err = I2C_STAT(periph) & (I2C_STAT_NACK | I2C_STAT_BERR |
                                       I2C_STAT_LOSTARB | I2C_STAT_TIMEOUT);
    if (err)
    {
#if I2C_TRACE
        /* NACK 是日常（探测/polling），只给真正的异常留黑匣子 */
        if (err & ~I2C_STAT_NACK)
            rt_kprintf("[i2c] ERR @%s STAT=0x%08lx CTL1=0x%08lx\n",
                       stage, I2C_STAT(periph), I2C_CTL1(periph));
#endif
        return -RT_EIO;
    }
    return RT_EOK;
}

/* ---------------- 单条消息传输 ---------------- */

static rt_err_t i2c_xfer_one_msg(uint32_t periph,
                                 const rt_uint8_t *buf, rt_uint16_t len,
                                 rt_uint16_t addr, rt_bool_t is_read,
                                 rt_bool_t last_and_stop)
{
    rt_tick_t deadline = rt_tick_get() +
        rt_tick_from_millisecond(I2C_TIMEOUT_BASE_MS +
                                 I2C_TIMEOUT_PER_BYTE_MS * (len + 1));
    rt_err_t ret;
    rt_uint16_t i;

    /* 总线被上一阶段 RELOAD 保持时 I2CBSY 置位是正常的；
     * START 位置 1 后硬件自己会等总线可用，此处不预等空闲。 */

    /* 状态卫生第一条：开局清掉可能残留的 STPDET，
     * 防止陈旧 STOP 标志让本条的 AUTOEND 等待"提前撞线" */
    I2C_STATC(periph) = I2C_STATC_STPDETC;

    i2c_master_addressing(periph, (uint32_t)addr << 1,
                          is_read ? I2C_MASTER_RECEIVE : I2C_MASTER_TRANSMIT);
    i2c_transfer_byte_number_config(periph, len);
    /* RELOAD/AUTOEND 不是互斥位，固件库的 enable 不会清另一位——
     * 上一条消息若用了 RELOAD，必须先清场，否则 AUTOEND 不生效 */
    I2C_CTL1(periph) &= ~(I2C_CTL1_RELOAD | I2C_CTL1_AUTOEND);
    if (last_and_stop)
        i2c_automatic_end_enable(periph);       /* 发完自动 STOP */
    else
        i2c_reload_enable(periph);              /* 保持总线，等下一条消息 */
    i2c_start_on_bus(periph);

    if (!is_read)
    {
        for (i = 0; i < len; i++)
        {
            /* TI：TDATA 空且需要续传（含首字节） */
            ret = i2c_wait_flag_set(periph, I2C_STAT_TI, deadline, "tx-ti");
            if (ret != RT_EOK)
                goto _err;
            ret = i2c_check_error(periph, "tx-loop");
            if (ret != RT_EOK)
                goto _err;
            i2c_data_transmit(periph, buf[i]);
        }
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            /* RBNE 与 NACK 谁先到算谁：NACK 是合法终局（设备探测、
             * EEPROM 写周期 ACK polling），不该为它干等整个超时 */
            for (;;)
            {
                uint32_t stat = I2C_STAT(periph);
                if (stat & I2C_STAT_RBNE)
                    break;
                if (stat & I2C_STAT_NACK)
                {
                    ret = -RT_EIO;      /* NACK 是合法终局，由调用者判读 */
                    goto _err;
                }
                if (rt_tick_get() > deadline)
                {
#if I2C_TRACE
                    rt_kprintf("[i2c] TIMEOUT @rx-rbne STAT=0x%08lx\n",
                               I2C_STAT(periph));
#endif
                    ret = -RT_ETIMEOUT;
                    goto _err;
                }
            }
            ((rt_uint8_t *)buf)[i] = (rt_uint8_t)i2c_data_receive(periph);
        }
    }

    /* 阶段收尾——三种"完成"语义，别等错了（本驱动最值钱的一课）：
     *   TCR   ：RELOAD 阶段传完 N 字节、总线被保持 -> 等它；
     *   TC    ：仅用于 AUTOEND=0 的软件结束模式（手动发 STOP）；
     *   STPDET：AUTOEND=1 时硬件自动发完 STOP 的标志 -> AUTOEND 等它！
     * AUTOEND 模式下 TC 永不置位，等 TC = 等一封不会来的信。
     * （NACK 场景 STPDET 同样置位，不存在的地址秒回，不用干等超时。） */
    ret = i2c_wait_flag_set(periph,
            last_and_stop ? I2C_STAT_STPDET : I2C_STAT_TCR,
            deadline, "end-stpdet/tcr");
    if (ret != RT_EOK)
        goto _err;
    ret = i2c_check_error(periph, "final");
    if (ret != RT_EOK)
        goto _err;

    if (last_and_stop)
    {
        /* STPDET 即 STOP 完工，清旗收工；总线此刻已空闲 */
        I2C_STATC(periph) = I2C_STATC_STPDETC;
    }
    return RT_EOK;

_err:
    i2c_bus_recover(periph);    /* 统一逃生：按需 STOP + 状态清零 */
    return ret;
}

/* ---------------- RT-Thread I2C 框架接口 ---------------- */

static rt_ssize_t h7_i2c_xfer(struct rt_i2c_bus_device *bus,
                              struct rt_i2c_msg msgs[], rt_uint32_t num)
{
    struct h7_i2c_bus *h7bus = (struct h7_i2c_bus *)bus;
    rt_uint32_t periph = h7bus->cfg->periph;
    rt_uint32_t i;
    rt_err_t ret;

    for (i = 0; i < num; i++)
    {
        rt_bool_t is_read = (msgs[i].flags & RT_I2C_RD) ? RT_TRUE : RT_FALSE;
        rt_bool_t no_stop = (msgs[i].flags & RT_I2C_NO_STOP) ? RT_TRUE : RT_FALSE;
        rt_bool_t last_and_stop;

        if (msgs[i].flags & RT_I2C_ADDR_10BIT)
            return -RT_EINVAL;      /* 只支持 7 位地址（常见器件全是 7 位） */
        if (msgs[i].len == 0 || msgs[i].len > 255)
            return -RT_EINVAL;      /* BYTENUM 域 8 位；空消息无意义 */

        last_and_stop = (i == num - 1) && !no_stop;

        ret = i2c_xfer_one_msg(periph, msgs[i].buf, msgs[i].len,
                               msgs[i].addr, is_read, last_and_stop);
        if (ret != RT_EOK)
        {
            LOG_D("%s xfer msg%d fail: %d (addr 0x%02x len %d)",
                  h7bus->cfg->name, i, ret, msgs[i].addr, msgs[i].len);
            return ret;
        }
    }
    return (rt_ssize_t)num;
}

static const struct rt_i2c_bus_device_ops h7_i2c_ops =
{
    .master_xfer      = h7_i2c_xfer,
    .slave_xfer       = RT_NULL,
    .i2c_bus_control  = RT_NULL,
};

/* ---------------- 时序计算与初始化 ---------------- */

/*
 * 由 I2C 内核时钟实算 TIMING 寄存器。
 * tSCL = (PSC+1) * (SCLH+1 + SCLL+1) * tKER（忽略 SDADELY/SCLDELY 微调项）
 * 策略：PSC 从小到大试，SCLH/SCLL 五五分；取第一组合法分频。
 * 算的是偏保守值——实际 SCL 略低于目标，对 EEPROM 这类宽容器件最安全。
 */
static rt_err_t i2c_timing_init(uint32_t periph, rt_uint32_t scl_hz)
{
    rt_uint32_t f_ker = rcu_clock_freq_get(CK_APB1);  /* I2CxSEL 默认 APB1 */
    rt_uint32_t psc, total, scll, sclh;

    for (psc = 0; psc <= TIMING_PSC_MAX; psc++)
    {
        rt_uint32_t tick_hz = f_ker / (psc + 1);
        total = tick_hz / scl_hz;
        if (total < 8U || total > (TIMING_SCL_MAX + 1U) * 2U)
            continue;
        scll = total / 2U - 1U;
        sclh = total - (scll + 1U) - 1U;
        if (scll > TIMING_SCL_MAX || sclh > TIMING_SCL_MAX)
            continue;

        i2c_timing_config(periph, psc, 1U, 1U);   /* SCLDELY/SDADELY 各 1 tick */
        i2c_master_clock_config(periph, sclh, scll);

        LOG_I("timing: ker=%luHz psc=%lu sclh=%lu scll=%lu -> scl~%luHz",
              f_ker, psc, sclh, scll,
              tick_hz / ((scll + 1U) + (sclh + 1U)));
        return RT_EOK;
    }
    return -RT_EINVAL;
}

static void i2c_pin_init(const struct h7_i2c_pin *pin)
{
    rcu_periph_clock_enable(pin->rcu);
    gpio_af_set(pin->port, pin->af, pin->pin);
    gpio_mode_set(pin->port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, pin->pin);
    gpio_output_options_set(pin->port, GPIO_OTYPE_OD, GPIO_OSPEED_85MHZ,
                            pin->pin);
}

int rt_hw_i2c_init(void)
{
    rt_uint32_t i;

    for (i = 0; i < sizeof(i2c_config) / sizeof(i2c_config[0]); i++)
    {
        const struct h7_i2c_config *cfg = &i2c_config[i];
        struct h7_i2c_bus *bus = &i2c_buses[i];

        i2c_pin_init(&cfg->scl);
        i2c_pin_init(&cfg->sda);

        rcu_periph_clock_enable(cfg->rcu);
        i2c_deinit(cfg->periph);             /* RCU 外设复位清场（不动时钟使能） */
        i2c_disable(cfg->periph);
        /* 摄像头 SCCB 排障期：降到 100kHz（原厂 demo 同款）。
         * 定位完成后恢复 400k 或做成 Kconfig 可配 */
        if (i2c_timing_init(cfg->periph, 100000) != RT_EOK)
        {
            LOG_E("%s timing calc failed", cfg->name);
            continue;                        /* 单个实例失败不拖垮其它实例 */
        }
        i2c_enable(cfg->periph);

        bus->cfg = cfg;
        bus->parent.ops = &h7_i2c_ops;
        rt_i2c_bus_device_register(&bus->parent, cfg->name);
        LOG_I("%s registered", cfg->name);
    }
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_i2c_init);

#endif /* BSP_USING_I2Cx */