/*
 * drv_spi_h7.c - GD32H7xx SPI 总线驱动（RT-Thread SPI 框架）
 *
 * H7 SPI 是"会话式"传输引擎：MSTART 开跑后逐字节"等TP->写TDATA->等TC"
 * （官方 14_SPI_Quad_LCD demo 的用法：不声明 TSIZE、不等 ET、不收队，
 * CS 拉起即结束）。曾经尝试了按数据手册自研 TSIZE/ET 完整会话管理，但未成功。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_SPI0) || defined(BSP_USING_SPI1) || defined(BSP_USING_SPI2) || \
    defined(BSP_USING_SPI3) || defined(BSP_USING_SPI4) || defined(BSP_USING_SPI5)

#define DBG_TAG             "drv.spi"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define SPI_XFER_TIMEOUT_MS 1000

/* ---------- 配置表 ---------- */
struct h7_spi_pin
{
    rcu_periph_enum rcu;
    uint32_t        port;
    uint32_t        pin;
    uint32_t        af;
};

struct h7_spi_config
{
    const char      *name;
    uint32_t        periph;
    rcu_periph_enum rcu;
    struct h7_spi_pin sck;
    struct h7_spi_pin miso;
    struct h7_spi_pin mosi;
};

static const struct h7_spi_config spi_config[] =
{
#ifdef BSP_USING_SPI4
    {
        "spi4", SPI4, RCU_SPI4,
        { RCU_GPIOH, GPIOH, GPIO_PIN_6, GPIO_AF_5 },   /* SCK  */
        { RCU_GPIOH, GPIOH, GPIO_PIN_7, GPIO_AF_5 },   /* MISO */
        { RCU_GPIOF, GPIOF, GPIO_PIN_9, GPIO_AF_5 },   /* MOSI */
    },
#endif
};

struct h7_spi
{
    struct rt_spi_bus           bus;
    uint32_t                    periph;
    const struct h7_spi_config *cfg;
};

/* 引脚 AF 配置：rt_hw_spi_init 调一次，spi_configure 每次调。 */
static void h7_spi_gpio_init(const struct h7_spi_config *cfg)
{
    rcu_periph_clock_enable(cfg->sck.rcu);
    rcu_periph_clock_enable(cfg->miso.rcu);
    rcu_periph_clock_enable(cfg->mosi.rcu);
    rcu_periph_clock_enable(cfg->rcu);

    gpio_af_set(cfg->sck.port,  cfg->sck.af,  cfg->sck.pin);
    gpio_af_set(cfg->miso.port, cfg->miso.af, cfg->miso.pin);
    gpio_af_set(cfg->mosi.port, cfg->mosi.af, cfg->mosi.pin);

    gpio_mode_set(cfg->sck.port,  GPIO_MODE_AF, GPIO_PUPD_NONE, cfg->sck.pin);
    gpio_mode_set(cfg->miso.port, GPIO_MODE_AF, GPIO_PUPD_NONE, cfg->miso.pin);
    gpio_mode_set(cfg->mosi.port, GPIO_MODE_AF, GPIO_PUPD_NONE, cfg->mosi.pin);
    gpio_output_options_set(cfg->sck.port,  GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, cfg->sck.pin);
    gpio_output_options_set(cfg->miso.port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, cfg->miso.pin);
    gpio_output_options_set(cfg->mosi.port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, cfg->mosi.pin);
}

/* ---------- 时钟：SPI4 内核时钟默认源 APB2（RCU_CFG5.SPI4SEL 复位值 0） ---------- */
static uint32_t spi_kernel_clock(void)
{
    return rcu_clock_freq_get(CK_APB2);
}

/* 由目标波特率选分频：找第一个 <= max_hz 的挡位 */
static uint32_t spi_psc_from_hz(uint32_t clk, uint32_t max_hz)
{
    static const uint32_t psc_tab[] = {
        SPI_PSC_2,   SPI_PSC_4,   SPI_PSC_8,   SPI_PSC_16,
        SPI_PSC_32,  SPI_PSC_64,  SPI_PSC_128, SPI_PSC_256
    };
    int i;
    for (i = 0; i < 8; i++)
    {
        if (clk / (2U << i) <= max_hz)
            return psc_tab[i];
    }
    return SPI_PSC_256;
}

/* ---------- RT-Thread SPI bus ops ---------- */
static rt_err_t spi_configure(struct rt_spi_device *device, struct rt_spi_configuration *cfg)
{
    struct h7_spi *spi = (struct h7_spi *)device->bus->parent.user_data;
    spi_parameter_struct para;
    uint32_t mode;

    RT_ASSERT(cfg->data_width == 8);        /* 屏驱用 8bit，其余宽度以后再加 */

    spi_struct_para_init(&para);
    para.device_mode  = SPI_MASTER;
    para.trans_mode   = SPI_TRANSMODE_FULLDUPLEX;
    para.data_size    = SPI_DATASIZE_8BIT;
    para.nss          = SPI_NSS_SOFT;       /* CS 交给框架 GPIO */
    para.endian       = (cfg->mode & RT_SPI_LSB) ? SPI_ENDIAN_LSB : SPI_ENDIAN_MSB;

    if (cfg->mode & RT_SPI_MODE_3)      mode = SPI_CK_PL_HIGH_PH_2EDGE;
    else if (cfg->mode & RT_SPI_MODE_2) mode = SPI_CK_PL_HIGH_PH_1EDGE;
    else if (cfg->mode & RT_SPI_MODE_1) mode = SPI_CK_PL_LOW_PH_2EDGE;
    else                                mode = SPI_CK_PL_LOW_PH_1EDGE;
    para.clock_polarity_phase = mode;

    para.prescale = spi_psc_from_hz(spi_kernel_clock(), cfg->max_hz);
    h7_spi_gpio_init(spi->cfg);
    SPI_CTL0(spi->periph) &= ~(1U << 9);        /* MSTART=0 */
    spi_disable(spi->periph);
    SPI_CFG0(spi->periph) &= ~SPI_CFG0_PSC;
    spi_init(spi->periph, &para);
    spi_byte_access_enable(spi->periph);
    spi_fifo_threshold_level_set(spi->periph, SPI_FIFO_TH_01DATA);
    spi_nss_internal_high(spi->periph); 
    spi_nss_output_enable(spi->periph); 
    spi_enable(spi->periph);
    return RT_EOK;
}

/* 超时残局清理：停会话、排 RX、清所有可清标志*/
static void spi_recover(uint32_t periph)
{
    spi_master_transfer_start(periph, SPI_TRANS_IDLE);
    while (SPI_STAT(periph) & (SPI_STAT_RP | SPI_STAT_RWNE))
        (void)SPI_RDATA(periph);
    SPI_STATC(periph) = SPI_STATC_ETC | SPI_STATC_TXFC | SPI_STATC_TXURERRC |
                        SPI_STATC_RXORERRC | SPI_STATC_CRCERRC | SPI_STATC_FERRC |
                        SPI_STATC_CONFERRC | SPI_STATC_TXSERFC | SPI_STATC_SPDC;
}

/* 等一个标志位置位，带 deadline 超时；超时返回 -RT_ETIMEOUT */
static rt_err_t spi_wait_flag(uint32_t periph, rt_uint32_t flag, rt_tick_t deadline)
{
    while (!(SPI_STAT(periph) & flag))
    {
        if (rt_tick_get() - deadline < RT_TICK_MAX / 2)
        {
            rt_kprintf("[spi] TIMEOUT wait flag=0x%04x STAT=0x%04x\n",
                       flag, SPI_STAT(periph));
            return -RT_ETIMEOUT;
        }
    }
    return RT_EOK;
}

/* 传输：照搬官方 demo 14_SPI_Quad_LCD */
static rt_ssize_t spi_xfer(struct rt_spi_device *device, struct rt_spi_message *message)
{
    struct h7_spi *spi = (struct h7_spi *)device->bus->parent.user_data;
    const rt_uint8_t *tx = message->send_buf;
    rt_uint8_t *rx = message->recv_buf;
    rt_uint32_t len = message->length;
    rt_uint32_t i;
    rt_err_t ret;

    if (len == 0)
        return 0;

    rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(SPI_XFER_TIMEOUT_MS);

    spi_master_transfer_start(spi->periph, SPI_TRANS_START);

    if (rx == RT_NULL)
    {
        for (i = 0; i < len; i++)
        {
            ret = spi_wait_flag(spi->periph, SPI_STAT_TP, deadline);
            if (ret != RT_EOK) goto _timeout;
            *(volatile rt_uint8_t *)&SPI_TDATA(spi->periph) = tx ? tx[i] : 0xFF;
        }
        ret = spi_wait_flag(spi->periph, SPI_STAT_TC, deadline);
        if (ret != RT_EOK) goto _timeout;
    }
    else
    {
        for (i = 0; i < len; i++)
        {
            ret = spi_wait_flag(spi->periph, SPI_STAT_TP, deadline);
            if (ret != RT_EOK) goto _timeout;

            *(volatile rt_uint8_t *)&SPI_TDATA(spi->periph) = tx ? tx[i] : 0xFF;

            ret = spi_wait_flag(spi->periph, SPI_STAT_TC, deadline);
            if (ret != RT_EOK) goto _timeout;

            ret = spi_wait_flag(spi->periph, SPI_STAT_RP, deadline);
            if (ret != RT_EOK) goto _timeout;
            rx[i] = *(volatile rt_uint8_t *)&SPI_RDATA(spi->periph);
        }
    }

    return (rt_ssize_t)len;

_timeout:
    spi_recover(spi->periph);
    return -RT_ETIMEOUT;
}

static const struct rt_spi_ops h7_spi_ops =
{
    .configure = spi_configure,
    .xfer      = spi_xfer,
};

/* ---------- 注册 ---------- */
int rt_hw_spi_init(void)
{
    rt_uint32_t i;
    int result = RT_EOK;

    for (i = 0; i < sizeof(spi_config) / sizeof(spi_config[0]); i++)
    {
        const struct h7_spi_config *cfg = &spi_config[i];
        struct h7_spi *spi;

        spi = rt_malloc(sizeof(struct h7_spi));
        if (spi == RT_NULL)
        {
            LOG_E("malloc %s failed", cfg->name);
            return -RT_ENOMEM;
        }
        spi->periph = cfg->periph;
        spi->cfg    = cfg;
        h7_spi_gpio_init(cfg);
        spi_i2s_deinit(cfg->periph);
        if (cfg->periph == SPI4)
            rcu_spi_clock_config(IDX_SPI4, RCU_SPISRC_APB2);

        result = rt_spi_bus_register(&spi->bus, cfg->name, &h7_spi_ops);
        if (result != RT_EOK)
        {
            LOG_E("register %s failed: %d", cfg->name, result);
            rt_free(spi);
            return result;
        }
        spi->bus.parent.user_data = spi;
        LOG_I("%s registered", cfg->name);
    }
    return result;
}
INIT_BOARD_EXPORT(rt_hw_spi_init);

#endif /* BSP_USING_SPIx */