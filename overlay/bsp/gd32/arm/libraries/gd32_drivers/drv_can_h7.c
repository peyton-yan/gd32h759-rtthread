/*
 * File      : drv_can_h7.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2006 - 2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32H7xx 的 CAN 控制器是 FlexCAN 风格架构（邮箱 + 片内消息 RAM，原生支持
 * CAN-FD），与 GD32F1/F4/F5 的 bxCAN 完全不同，无法复用 drv_can.c，故独立实现。
 *
 * v1 范围：经典 CAN 8 字节帧；标准/扩展帧；中断收发；
 *          RX 固定邮箱 MB0，TX 固定邮箱 MB1；
 *          支持 NORMAL / LISTEN(监听) / LOOPBACK(回环) 模式。
 *
 * 已知限制（v2 计划）：
 *  - 未接 CANx_Error_IRQn/Busoff_IRQn，总线异常时 blocking 写会等不到 TX_DONE；
 *  - 过滤器为单邮箱粗粒度（id + public mask），未实现 RT_CAN_USING_HDR 多过滤器；
 *  - 未启用 CAN-FD。
 */

#include <board.h>
#include <rtdevice.h>

#if defined(BSP_USING_CAN) && defined(SOC_SERIES_GD32H7xx)

#define H7_CAN_RX_MB    0U
#define H7_CAN_TX_MB    1U

/* 硬件实例配置 */
struct h7_can_hw
{
    rt_uint32_t     can_x;
    const char     *name;
    rcu_periph_enum can_clk;
    can_idx_enum    rcu_idx;
    IRQn_Type       msg_irqn;
    rt_uint32_t     af;
    rt_uint32_t     tx_port;
    rt_uint32_t     tx_pin;
    rcu_periph_enum tx_clk;
    rt_uint32_t     rx_port;
    rt_uint32_t     rx_pin;
    rcu_periph_enum rx_clk;
};

struct h7_can_device
{
    struct rt_can_device          can_dev;
    const struct h7_can_hw       *hw;
    can_mailbox_descriptor_struct rx_desc;
    rt_uint8_t                    rx_buf[8];
    rt_uint32_t                   filter_id;    /* 0 + mask 0 = 接收全部 */
    rt_uint32_t                   filter_mask;  /* bit=1 表示该位参与匹配 */
};

/* 波特率表：CAN 时钟 = APB2 = 300MHz
 * TQ = 1 + prop + tseg1 + tseg2，baud = 300MHz / presc / TQ
 * 分频器 10bit（最大 1024），10kbps 在 300MHz 下不可达，故不提供 */
struct h7_can_baud
{
    rt_uint32_t baud;
    rt_uint8_t  sjw, prop, tseg1, tseg2;
    rt_uint16_t presc;
};
static const struct h7_can_baud h7_can_baud_tbl[] =
{   /*  baud            sjw prop tseg1 tseg2 presc   TQ=15 采样点80%，1M 为官方例程值 */
    {   CAN1MBaud,      1,  2,   5,    2,    30   },/* TQ=10，采样点 80% */
    {   CAN800kBaud,    1,  2,   9,    3,    25   },
    {   CAN500kBaud,    1,  2,   9,    3,    40   },
    {   CAN250kBaud,    1,  2,   9,    3,    80   },
    {   CAN125kBaud,    1,  2,   9,    3,    160  },
    {   CAN100kBaud,    1,  2,   9,    3,    200  },
    {   CAN50kBaud,     1,  2,   9,    3,    400  },
    {   CAN20kBaud,     1,  2,   9,    3,    1000 },
};

static const struct h7_can_hw h7_can_hw_tbl[] =
{
#ifdef BSP_USING_CAN1
    {
        .can_x = CAN1, .name = "can1", .can_clk = RCU_CAN1, .rcu_idx = IDX_CAN1,
        .msg_irqn = CAN1_Message_IRQn, .af = GPIO_AF_9,
#if defined(BSP_CAN1_TX_PB13)
        .tx_port = GPIOB, .tx_pin = GPIO_PIN_13, .tx_clk = RCU_GPIOB,
#else
    #error "Select CAN1 tx pin"
#endif
#if defined(BSP_CAN1_RX_PB5)
        .rx_port = GPIOB, .rx_pin = GPIO_PIN_5, .rx_clk = RCU_GPIOB,
#else
    #error "Select CAN1 rx pin"
#endif
    },
#endif
#ifdef BSP_USING_CAN2
    {
        .can_x = CAN2, .name = "can2", .can_clk = RCU_CAN2, .rcu_idx = IDX_CAN2,
        .msg_irqn = CAN2_Message_IRQn, .af = GPIO_AF_2,
#if defined(BSP_CAN2_TX_PF7)
        .tx_port = GPIOF, .tx_pin = GPIO_PIN_7, .tx_clk = RCU_GPIOF,
#else
    #error "Select CAN2 tx pin"
#endif
#if defined(BSP_CAN2_RX_PF6)
        .rx_port = GPIOF, .rx_pin = GPIO_PIN_6, .rx_clk = RCU_GPIOF,
#else
    #error "Select CAN2 rx pin"
#endif
    },
#endif
};

#ifdef BSP_USING_CAN1
static struct h7_can_device h7_can1;
#endif
#ifdef BSP_USING_CAN2
static struct h7_can_device h7_can2;
#endif

static void h7_can_gpio_init(const struct h7_can_hw *hw)
{
    /* CAN 时钟源选 APB2（300MHz），官方例程做法 */
    rcu_can_clock_config(hw->rcu_idx, RCU_CANSRC_APB2);
    rcu_periph_clock_enable(hw->can_clk);
    rcu_periph_clock_enable(hw->tx_clk);
    rcu_periph_clock_enable(hw->rx_clk);

    gpio_output_options_set(hw->tx_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, hw->tx_pin);
    gpio_mode_set(hw->tx_port, GPIO_MODE_AF, GPIO_PUPD_PULLUP, hw->tx_pin);
    gpio_af_set(hw->tx_port, hw->af, hw->tx_pin);

    gpio_output_options_set(hw->rx_port, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, hw->rx_pin);
    gpio_mode_set(hw->rx_port, GPIO_MODE_AF, GPIO_PUPD_NONE, hw->rx_pin);
    gpio_af_set(hw->rx_port, hw->af, hw->rx_pin);
}

static rt_err_t h7_can_config(struct rt_can_device *can, struct can_configure *cfg)
{
    struct h7_can_device *dev = (struct h7_can_device *)can->parent.user_data;
    rt_uint32_t can_x = dev->hw->can_x;
    can_parameter_struct param;
    can_operation_modes_enum mode = CAN_NORMAL_MODE;
    const struct h7_can_baud *bt = &h7_can_baud_tbl[0];
    ErrStatus st;

    rt_kprintf("[drv] cfg enter: mode=%lu baud=%lu\n", cfg->mode, cfg->baud_rate);

    for (rt_uint32_t i = 0; i < sizeof(h7_can_baud_tbl) / sizeof(h7_can_baud_tbl[0]); i++)
    {
        if (h7_can_baud_tbl[i].baud == cfg->baud_rate) { bt = &h7_can_baud_tbl[i]; break; }
    }

    /* 参照官方 19_CAN_Network demo：CAN1/CAN2 同属一块 FlexCAN 逻辑，
     * 必须一起 deinit（顺序：CAN1 后 CAN2），只处理 CAN2 会进不了 NORMAL 模式 */
#ifdef BSP_USING_CAN1
    can_deinit(CAN1);
#endif
#ifdef BSP_USING_CAN2
    can_deinit(CAN2);
#endif
    can_struct_para_init(CAN_INIT_STRUCT, &param);
    param.internal_counter_source        = CAN_TIMER_SOURCE_BIT_CLOCK;
    param.mb_tx_order                    = CAN_TX_HIGH_PRIORITY_MB_FIRST;
    param.mb_tx_abort_enable             = ENABLE;
    param.local_priority_enable          = DISABLE;
    param.mb_rx_ide_rtr_type             = CAN_IDE_RTR_FILTERED;
    param.mb_remote_frame                = CAN_STORE_REMOTE_REQUEST_FRAME;
    param.rx_private_filter_queue_enable = DISABLE;
    param.edge_filter_enable             = DISABLE;
    param.protocol_exception_enable      = DISABLE;
    param.rx_filter_order                = CAN_RX_FILTER_ORDER_MAILBOX_FIRST;
    param.memory_size                    = CAN_MEMSIZE_32_UNIT;
    param.mb_public_filter               = dev->filter_mask;
    param.self_reception = (cfg->mode == RT_CAN_MODE_LOOPBACK ||
                            cfg->mode == RT_CAN_MODE_LOOPBACKANLISTEN) ? ENABLE : DISABLE;
    param.resync_jump_width = bt->sjw;
    param.prop_time_segment = bt->prop;
    param.time_segment_1    = bt->tseg1;
    param.time_segment_2    = bt->tseg2;
    param.prescaler         = bt->presc;

    /* 顺序很关键：先 CAN1 再 CAN2（与官方 demo 一致），否则 CAN2 起不来 */
#ifdef BSP_USING_CAN1
    st = can_init(CAN1, &param);
    rt_kprintf("[drv] can_init(CAN1) -> %d\n", st);
    if (st != SUCCESS) return -RT_ERROR;
#endif
#ifdef BSP_USING_CAN2
    st = can_init(CAN2, &param);
    rt_kprintf("[drv] can_init(CAN2) -> %d\n", st);
    if (st != SUCCESS) return -RT_ERROR;
#endif

    can_struct_para_init(CAN_MDSC_STRUCT, &dev->rx_desc);
    dev->rx_desc.code = CAN_MB_RX_STATUS_EMPTY;
    dev->rx_desc.id   = dev->filter_id;
    dev->rx_desc.data = (rt_uint32_t *)dev->rx_buf;
    can_mailbox_config(can_x, H7_CAN_RX_MB, &dev->rx_desc);
    rt_kprintf("[drv] rx mailbox ok\n");

    switch (cfg->mode)
    {
    case RT_CAN_MODE_NORMAL:           mode = CAN_NORMAL_MODE;          break;
    case RT_CAN_MODE_LISTEN:           mode = CAN_MONITOR_MODE;         break;
    case RT_CAN_MODE_LOOPBACK:
    case RT_CAN_MODE_LOOPBACKANLISTEN: mode = CAN_LOOPBACK_SILENT_MODE; break;
    default: rt_kprintf("[drv] bad mode\n"); return -RT_ERROR;
    }
    st = can_operation_mode_enter(can_x, mode);
    rt_kprintf("[drv] mode_enter(%d) -> %d\n", mode, st);
    if (st != SUCCESS)
    {
        rt_kprintf("[drv] mode_enter fail: CTL0=0x%08x STAT=0x%08x CTL1=0x%08x\n",
                   (unsigned)CAN_CTL0(can_x), (unsigned)CAN_STAT(can_x),
                   (unsigned)CAN_CTL1(can_x));
        rt_kprintf("[drv] hint: check CAN bus/jumpers (JP67->CAN, JP14 120R) or use loopback mode\n");
        return -RT_ERROR;
    }

    if (can->parent.open_flag & RT_DEVICE_FLAG_INT_RX)
    {
        can_interrupt_enable(can_x, CAN_INT_MB0);
        nvic_irq_enable(dev->hw->msg_irqn, 1, 0);
    }
    if (can->parent.open_flag & RT_DEVICE_FLAG_INT_TX)
    {
        can_interrupt_enable(can_x, CAN_INT_MB1);
        nvic_irq_enable(dev->hw->msg_irqn, 1, 0);
    }
    rt_kprintf("[drv] cfg done\n");
    return RT_EOK;
}

static rt_err_t h7_can_control(struct rt_can_device *can, int cmd, void *arg)
{
    struct h7_can_device *dev = (struct h7_can_device *)can->parent.user_data;
    rt_uint32_t can_x = dev->hw->can_x;
    rt_uint32_t argval;

    switch (cmd)
    {
    case RT_DEVICE_CTRL_SET_INT:
        argval = (rt_uint32_t)(rt_ubase_t)arg;
        if (argval == RT_DEVICE_FLAG_INT_RX)
        {
            can_interrupt_enable(can_x, CAN_INT_MB0);
            nvic_irq_enable(dev->hw->msg_irqn, 1, 0);
        }
        else if (argval == RT_DEVICE_FLAG_INT_TX)
        {
            can_interrupt_enable(can_x, CAN_INT_MB1);
            nvic_irq_enable(dev->hw->msg_irqn, 1, 0);
        }
        /* RT_DEVICE_CAN_INT_ERR：v1 未接错误中断，直接忽略 */
        break;

    case RT_DEVICE_CTRL_CLR_INT:
        argval = (rt_uint32_t)(rt_ubase_t)arg;
        if (argval == RT_DEVICE_FLAG_INT_RX)
        {
            can_interrupt_disable(can_x, CAN_INT_MB0);
            nvic_irq_disable(dev->hw->msg_irqn);
        }
        else if (argval == RT_DEVICE_FLAG_INT_TX)
        {
            can_interrupt_disable(can_x, CAN_INT_MB1);
            nvic_irq_disable(dev->hw->msg_irqn);
        }
        break;

    case RT_CAN_CMD_SET_MODE:
        argval = (rt_uint32_t)(rt_ubase_t)arg;
        if (argval != RT_CAN_MODE_NORMAL && argval != RT_CAN_MODE_LISTEN &&
            argval != RT_CAN_MODE_LOOPBACK && argval != RT_CAN_MODE_LOOPBACKANLISTEN)
        {
            return -RT_ERROR;
        }
        if (argval != can->config.mode)
        {
            can->config.mode = argval;
            return h7_can_config(can, &can->config);
        }
        break;

    case RT_CAN_CMD_SET_BAUD:
        argval = (rt_uint32_t)(rt_ubase_t)arg;
        if (argval != CAN1MBaud && argval != CAN800kBaud &&
            argval != CAN500kBaud && argval != CAN250kBaud &&
            argval != CAN125kBaud && argval != CAN100kBaud &&
            argval != CAN50kBaud  && argval != CAN20kBaud)
        {
            return -RT_ERROR;   /* 10k 在 300MHz 时钟下不可达 */
        }
        if (argval != can->config.baud_rate)
        {
            can->config.baud_rate = argval;
            return h7_can_config(can, &can->config);
        }
        break;

    case RT_CAN_CMD_SET_PRIV:
        argval = (rt_uint32_t)(rt_ubase_t)arg;
        if (argval != RT_CAN_MODE_PRIV && argval != RT_CAN_MODE_NOPRIV)
        {
            return -RT_ERROR;
        }
        can->config.privmode = argval;   /* 由框架层处理，无需重配硬件 */
        break;

    case RT_CAN_CMD_SET_FILTER:
        if (arg == RT_NULL)
        {
            dev->filter_id = 0;
            dev->filter_mask = 0;
        }
        else
        {
            struct rt_can_filter_config *fcfg = (struct rt_can_filter_config *)arg;
            if (fcfg->count > 0)
            {
                dev->filter_id   = fcfg->items[0].id;
                dev->filter_mask = fcfg->items[0].mask;
            }
        }
        return h7_can_config(can, &can->config);

    case RT_CAN_CMD_GET_STATUS:
    {
        can_error_counter_struct errcnt;
        can_error_counter_get(can_x, &errcnt);
        can->status.rcverrcnt = errcnt.rx_errcnt;
        can->status.snderrcnt = errcnt.tx_errcnt;
        can->status.errcode   = (rt_uint32_t)can_error_state_get(can_x);
        rt_memcpy(arg, &can->status, sizeof(can->status));
        break;
    }

    default:
        break;
    }

    return RT_EOK;
}

static rt_ssize_t h7_can_sendmsg(struct rt_can_device *can, const void *buf, rt_uint32_t box_num)
{
    struct h7_can_device *dev = (struct h7_can_device *)can->parent.user_data;
    struct rt_can_msg *pmsg = (struct rt_can_msg *)buf;
    rt_uint32_t can_x = dev->hw->can_x;
    can_mailbox_descriptor_struct tx_msg;

    /* 上一帧还没发出去 */
    if (can_mailbox_code_get(can_x, H7_CAN_TX_MB) == CAN_MB_TX_STATUS_DATA)
    {
        return -RT_EBUSY;
    }

    can_struct_para_init(CAN_MDSC_STRUCT, &tx_msg);
    tx_msg.code       = CAN_MB_TX_STATUS_DATA;
    tx_msg.ide        = (pmsg->ide == RT_CAN_EXTID) ? 1U : 0U;
    tx_msg.rtr        = (pmsg->rtr == RT_CAN_RTR) ? 1U : 0U;
    tx_msg.id         = pmsg->id;
    tx_msg.data_bytes = (pmsg->len > 8U) ? 8U : pmsg->len;
    tx_msg.data       = (rt_uint32_t *)pmsg->data;
    can_mailbox_config(can_x, H7_CAN_TX_MB, &tx_msg);

    return RT_EOK;
}

static rt_ssize_t h7_can_recvmsg(struct rt_can_device *can, void *buf, rt_uint32_t fifo)
{
    struct h7_can_device *dev = (struct h7_can_device *)can->parent.user_data;
    struct rt_can_msg *pmsg = (struct rt_can_msg *)buf;

    if (can_mailbox_receive_data_read(dev->hw->can_x, H7_CAN_RX_MB, &dev->rx_desc) != SUCCESS)
    {
        return -RT_ERROR;
    }

    pmsg->ide = dev->rx_desc.ide ? RT_CAN_EXTID : RT_CAN_STDID;
    pmsg->id  = dev->rx_desc.id;
    pmsg->rtr = dev->rx_desc.rtr ? RT_CAN_RTR : RT_CAN_DTR;
    pmsg->len = (dev->rx_desc.data_bytes > 8U) ? 8U : (rt_uint8_t)dev->rx_desc.data_bytes;
    rt_memcpy(pmsg->data, dev->rx_buf, pmsg->len);
    pmsg->hdr_index = 0;

    return RT_EOK;
}

static rt_ssize_t h7_can_sendmsg_nonblocking(struct rt_can_device *can, const void *buf)
{
    return h7_can_sendmsg(can, buf, 0);
}

static const struct rt_can_ops h7_can_ops =
{
    h7_can_config,
    h7_can_control,
    h7_can_sendmsg,
    h7_can_recvmsg,
    h7_can_sendmsg_nonblocking,
};

static void h7_can_msg_isr(struct h7_can_device *dev)
{
    rt_uint32_t can_x = dev->hw->can_x;

    if (can_interrupt_flag_get(can_x, CAN_INT_FLAG_MB0))
    {
        can_interrupt_flag_clear(can_x, CAN_INT_FLAG_MB0);
        rt_hw_can_isr(&dev->can_dev, RT_CAN_EVENT_RX_IND | (0 << 8));
    }
    if (can_interrupt_flag_get(can_x, CAN_INT_FLAG_MB1))
    {
        can_interrupt_flag_clear(can_x, CAN_INT_FLAG_MB1);
        rt_hw_can_isr(&dev->can_dev, RT_CAN_EVENT_TX_DONE | (0 << 8));
    }
}

#ifdef BSP_USING_CAN1
void CAN1_Message_IRQHandler(void)
{
    rt_interrupt_enter();
    h7_can_msg_isr(&h7_can1);
    rt_interrupt_leave();
}
#endif

#ifdef BSP_USING_CAN2
void CAN2_Message_IRQHandler(void)
{
    rt_interrupt_enter();
    h7_can_msg_isr(&h7_can2);
    rt_interrupt_leave();
}
#endif

int rt_hw_can_init(void)
{
    struct can_configure config = CANDEFAULTCONFIG;
    rt_uint32_t i = 0;

    config.privmode     = RT_CAN_MODE_NOPRIV;
    config.ticks        = 50;
    config.sndboxnumber = 1;    /* 单 TX 邮箱，框架只会分配 box 0 */

#ifdef BSP_USING_CAN1
    h7_can1.hw = &h7_can_hw_tbl[i++];
    h7_can1.can_dev.config = config;
    h7_can_gpio_init(h7_can1.hw);
    rt_hw_can_register(&h7_can1.can_dev, h7_can1.hw->name, &h7_can_ops, &h7_can1);
#endif
#ifdef BSP_USING_CAN2
    h7_can2.hw = &h7_can_hw_tbl[i++];
    h7_can2.can_dev.config = config;
    h7_can_gpio_init(h7_can2.hw);
    rt_hw_can_register(&h7_can2.can_dev, h7_can2.hw->name, &h7_can_ops, &h7_can2);
#endif

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_can_init);

#endif /* BSP_USING_CAN && SOC_SERIES_GD32H7xx */