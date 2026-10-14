/*
 * drv_enet.c — RT-Thread Ethernet driver for GD32H759 (ENET0 + DP83848, RMII)
 *
 * Hardware notes (GD32H759I-EVAL):
 *   - PHY: DP83848CVV (U10), PHY_ADDRESS = 1, RMII interface
 *   - 50MHz REF_CLK: MCU CKOUT0 (PA8) = PLL0P/12 feeds PHY X1 — no external XO
 *   - RMII pins (AF11): PA1 REF_CLK / PA2 MDIO / PA7 CRS_DV / PC1 MDC /
 *     PC4 RXD0 / PC5 RXD1 / PG11 TX_EN / PB12 TXD0 / PG12 TXD1
 *   - DMA descriptors & buffers live at 0x30000000 (absolute sections in
 *     gd32h7xx_enet.c); link.ld must map them (see ENETRAM patch), and an
 *     MPU non-cacheable region must cover them because D-cache is enabled.
 *
 * Known limits (v1): interrupt-driven RX only (TX polled); cable must be
 * connected at boot (enet_init waits for PHY link/autoneg and returns ERROR
 * otherwise); no TX-complete reclaim interrupt.
 */

#include <rtthread.h>
#include <rtdevice.h>

#if defined(BSP_USING_ETH) && defined(RT_USING_LWIP)

#include <netif/ethernetif.h>
#include "gd32h7xx_enet.h"
#include "gd32h7xx_rcu.h"
#include "gd32h7xx_gpio.h"
#include "gd32h7xx_syscfg.h"
#include "gd32h7xx_misc.h"

#define DRV_DEBUG
#define DBG_TAG              "drv.enet"
#define DBG_LVL              DBG_INFO
#include <rtdbg.h>

#if defined(RT_USING_NETDEV) && defined(NETDEV_USING_LINK_STATUS_CALLBACK)
#include <netdev.h>

/* netdev 链接状态回调：lwIP 端口 (ethernetif.c) 以 extern 引用，需应用提供。
 * 本工程仅在链路变化时打日志；如需上层联动可在此扩展 */
void netdev_status_change(struct netdev *netdev, enum netdev_cb_type type)
{
    (void)netdev;
    if (type == NETDEV_CB_STATUS_LINK_UP)
        LOG_I("netdev link up");
    else if (type == NETDEV_CB_STATUS_LINK_DOWN)
        LOG_I("netdev link down");
}
#endif

/* keep in sync with netconf.c in the official demo */
static const rt_uint8_t h7_mac_addr[6] = {0x02, 0x0A, 0x0F, 0x0E, 0x0D, 0x06};
extern enet_descriptors_struct rxdesc_tab[ENET_RXBUF_NUM];
extern enet_descriptors_struct *dma_current_txdesc;
extern enet_descriptors_struct *dma_current_rxdesc;


struct h7_enet_device {
    struct eth_device parent;
};

static struct h7_enet_device h7_enet;

/* ------------------------------------------------------------------ */
/* MPU: make the descriptor/buffer region non-cacheable                */
/* ------------------------------------------------------------------ */
static void h7_enet_mpu_config(void)
{
    rt_uint32_t rnr;

    ARM_MPU_Disable();

    /* 不能硬编码 region 0：drv_sdram_h7 会先占用第一个空闲 region（通常是 0），
     * 这里改为查找空闲 region，避免把 SDRAM 的 non-cacheable 配置覆盖掉。 */
    for (rnr = 0; rnr < 16U; rnr++) {
        MPU->RNR = rnr;
        if ((MPU->RASR & MPU_RASR_ENABLE_Msk) == 0U) {
            break;
        }
    }
    if (rnr < 16U) {
        /* region: 0x30000000, 16KB, non-cacheable, bufferable, RW */
        ARM_MPU_SetRegion(ARM_MPU_RBAR(rnr, 0x30000000),
                          ARM_MPU_RASR(1, ARM_MPU_AP_FULL, 1, 0, 0, 1, 0,
                                       ARM_MPU_REGION_SIZE_16KB));
    } else {
        LOG_E("enet: no free MPU region");
    }

    ARM_MPU_Enable(MPU_MODE_PRIV_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* GPIO + clocks (from gd32h7xx_enet_eval.c)                           */
/* ------------------------------------------------------------------ */
static void h7_enet_gpio_config(void)
{
    /* CKOUT0 = PLL0P / 12 = 50MHz on PA8 -> PHY X1 */
    rcu_periph_clock_enable(RCU_GPIOA);
    gpio_af_set(GPIOA, GPIO_AF_0, GPIO_PIN_8);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_8);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_8);
    rcu_ckout0_config(RCU_CKOUT0SRC_PLL0P, RCU_CKOUT0_DIV12);

    /* RMII mode select */
    rcu_periph_clock_enable(RCU_SYSCFG);
    syscfg_enet_phy_interface_config(ENET0, SYSCFG_ENET_PHY_RMII);

    /* RMII pins, all AF11 */
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOG);

    /* PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV */
    gpio_af_set(GPIOA, GPIO_AF_11, GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_7);

    /* PC1 MDC, PC4 RXD0, PC5 RXD1 */
    gpio_af_set(GPIOC, GPIO_AF_11, GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_1 | GPIO_PIN_4 | GPIO_PIN_5);

    /* PG11 TX_EN, PG12 TXD1 */
    gpio_af_set(GPIOG, GPIO_AF_11, GPIO_PIN_11 | GPIO_PIN_12);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_NONE,
                  GPIO_PIN_11 | GPIO_PIN_12);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ,
                            GPIO_PIN_11 | GPIO_PIN_12);

    /* PB12 TXD0 (not listed in the user guide table 2-1, found in demo) */
    gpio_af_set(GPIOB, GPIO_AF_11, GPIO_PIN_12);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_12);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_12);
}

/* ------------------------------------------------------------------ */
/* rt_device ops                                                       */
/* ------------------------------------------------------------------ */
static rt_err_t h7_enet_dev_init(rt_device_t dev)
{
    h7_enet_gpio_config();
    h7_enet_mpu_config();

    rcu_periph_clock_enable(RCU_ENET0);
    rcu_periph_clock_enable(RCU_ENET0TX);
    rcu_periph_clock_enable(RCU_ENET0RX);

    enet_deinit(ENET0);
    enet_software_reset(ENET0);

    /* blocks on PHY link + autonegotiation: ERROR if cable unplugged */
    if (enet_init(ENET0, ENET_AUTO_NEGOTIATION, ENET_NO_AUTOCHECKSUM,
                  ENET_BROADCAST_FRAMES_PASS) == ERROR) {
        LOG_E("enet_init failed — is the cable connected?");
        return -RT_ERROR;
    }

    enet_mac_address_set(ENET0, ENET_MAC_ADDRESS0, (rt_uint8_t *)h7_mac_addr);

    enet_desc_select_normal_mode(ENET0);
    enet_descriptors_chain_init(ENET0, ENET_DMA_TX);
    enet_descriptors_chain_init(ENET0, ENET_DMA_RX);

    /* RX interrupt on every RX descriptor */
    for (int i = 0; i < ENET_RXBUF_NUM; i++) {
        enet_rx_desc_immediate_receive_complete_interrupt(&rxdesc_tab[i]);
    }

    enet_enable(ENET0);

    /* NIE + RIE */
        /* H7 库的 enet_int_enum 是 (寄存器<<6)|位号 编码，不可 OR，必须逐个使能 */
    enet_interrupt_enable(ENET0, ENET_DMA_INT_RIE);
    enet_interrupt_enable(ENET0, ENET_DMA_INT_NIE);

    nvic_irq_enable(ENET0_IRQn, 1, 0);

    eth_device_linkchange(&h7_enet.parent, RT_TRUE);

    LOG_I("ENET0 up, MAC %02X:%02X:%02X:%02X:%02X:%02X",
          h7_mac_addr[0], h7_mac_addr[1], h7_mac_addr[2],
          h7_mac_addr[3], h7_mac_addr[4], h7_mac_addr[5]);
    return RT_EOK;
}

static rt_err_t h7_enet_dev_open(rt_device_t dev, rt_uint16_t oflag)
{
    return RT_EOK;
}

static rt_err_t h7_enet_dev_close(rt_device_t dev)
{
    return RT_EOK;
}

static rt_ssize_t h7_enet_dev_read(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size)
{
    return 0;   /* packets come via eth_rx */
}

static rt_ssize_t h7_enet_dev_write(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size)
{
    return 0;   /* packets go via eth_tx */
}

static rt_err_t h7_enet_dev_control(rt_device_t dev, int cmd, void *args)
{
    switch (cmd) {
    case NIOCTL_GADDR:
        if (args) {
            rt_memcpy(args, h7_mac_addr, 6);
            return RT_EOK;
        }
        return -RT_ERROR;
    default:
        break;
    }
    return -RT_ERROR;
}

/* ------------------------------------------------------------------ */
/* eth_device ops: TX / RX                                             */
/* ------------------------------------------------------------------ */
static rt_err_t h7_enet_tx(rt_device_t dev, struct pbuf *p)
{
    struct pbuf *q;
    rt_uint8_t *buffer;
    rt_uint32_t framelength = 0;
    rt_uint32_t timeout = 0;
    enet_descriptors_struct *txdesc = dma_current_txdesc;

    /* wait for the DMA to release the current descriptor */
    while ((txdesc->status & ENET_TDES0_DAV) != 0U) {
        if (++timeout > 0x100000U) {
            return -RT_ETIMEOUT;
        }
    }
    

    buffer = (rt_uint8_t *)enet_desc_information_get(ENET0, txdesc, TXDESC_BUFFER_1_ADDR);

    for (q = p; q != NULL; q = q->next) {
        rt_memcpy(&buffer[framelength], q->payload, q->len);
        framelength += q->len;
    }

    ENET_NOCOPY_FRAME_TRANSMIT(ENET0, framelength);
    
    return RT_EOK;
}

static struct pbuf *h7_enet_rx(rt_device_t dev)
{
    struct pbuf *p = NULL;
    rt_uint32_t framelength = 0;
    rt_uint8_t *buffer;
    enet_descriptors_struct *rxdesc = dma_current_rxdesc;

    framelength = enet_desc_information_get(ENET0, rxdesc, RXDESC_FRAME_LENGTH);

    if (framelength == 0U) {
        return NULL;
    }

    p = pbuf_alloc(PBUF_LINK, framelength, PBUF_RAM);
    if (p != NULL) {
        buffer = (rt_uint8_t *)enet_desc_information_get(ENET0, rxdesc, RXDESC_BUFFER_1_ADDR);

        struct pbuf *q;
        rt_uint32_t offset = 0;
        for (q = p; q != NULL; q = q->next) {
            rt_memcpy(q->payload, &buffer[offset], q->len);
            offset += q->len;
        }
    } else {
        LOG_W("pbuf_alloc failed, frame dropped");
    }

    ENET_NOCOPY_FRAME_RECEIVE(ENET0);
    return p;
}

/* ------------------------------------------------------------------ */
/* ISR                                                                 */
/* ------------------------------------------------------------------ */
void ENET0_IRQHandler(void)
{
    rt_interrupt_enter();

    if (enet_interrupt_flag_get(ENET0, ENET_DMA_INT_FLAG_RS_CLR)) {
        enet_interrupt_flag_clear(ENET0, ENET_DMA_INT_FLAG_RS_CLR);
        enet_interrupt_flag_clear(ENET0, ENET_DMA_INT_FLAG_NI_CLR);
        eth_device_ready(&h7_enet.parent);
    }

    rt_interrupt_leave();
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */
static int rt_hw_enet_init(void)
{
    struct eth_device *eth = &h7_enet.parent;

    eth->parent.init      = h7_enet_dev_init;
    eth->parent.open      = h7_enet_dev_open;
    eth->parent.close     = h7_enet_dev_close;
    eth->parent.read      = h7_enet_dev_read;
    eth->parent.write     = h7_enet_dev_write;
    eth->parent.control   = h7_enet_dev_control;

    eth->eth_rx = h7_enet_rx;
    eth->eth_tx = h7_enet_tx;

    return eth_device_init(eth, "e0");
}
INIT_DEVICE_EXPORT(rt_hw_enet_init);

#endif /* BSP_USING_ETH && RT_USING_LWIP */