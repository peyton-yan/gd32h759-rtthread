/*
 * app_can.c — CAN 服务实现
 *
 * 默认使用 **can2**：板上 PB13 由 JP65 在 CAN1_TX 与 SDIO0_D0 间二选一，
 * 为与 SD 卡（SDIO）共存，CAN 服务走 can2（TX=PF7 / RX=PF6，不占 PB13）。
 * 如需改用 CAN1，编译时定义 -DCAN_SVC_DEV=\"can1\"（此时 JP65 需跳到 CAN，SDIO 不可用）。
 */
#include "app_can.h"

#if defined(BSP_USING_CAN) && defined(RT_USING_CAN)

#include <rtdevice.h>

#ifndef CAN_SVC_DEV
#define CAN_SVC_DEV "can2"
#endif

static rt_device_t     can_dev;
static volatile rt_uint32_t rx_cnt;
static rt_uint32_t     last_id;
static rt_uint8_t      last_len;

static rt_err_t can_rx_ind(rt_device_t dev, rt_size_t size)
{
    struct rt_can_msg msg;

    (void)size;
    while (rt_device_read(dev, 0, &msg, sizeof(msg)) == sizeof(msg))
    {
        rx_cnt++;
        last_id  = msg.id;
        last_len = msg.len;
    }
    return RT_EOK;
}

rt_bool_t app_can_ready(void)
{
    return can_dev != RT_NULL;
}

rt_err_t app_can_init(void)
{
    if (can_dev != RT_NULL)
        return RT_EOK;

    can_dev = rt_device_find(CAN_SVC_DEV);
    if (can_dev == RT_NULL)
        return -RT_ENOSYS;

    rt_device_control(can_dev, RT_CAN_CMD_SET_BAUD, (void *)CAN1MBaud);
    rt_device_control(can_dev, RT_CAN_CMD_SET_MODE, (void *)RT_CAN_MODE_NORMAL);
    rt_device_open(can_dev, RT_DEVICE_OFLAG_RDWR |
                            RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX);
    rt_device_set_rx_indicate(can_dev, can_rx_ind);
    return RT_EOK;
}

rt_err_t app_can_send(rt_uint32_t id, rt_uint8_t len, const rt_uint8_t *data)
{
    struct rt_can_msg msg;

    if (can_dev == RT_NULL)
        return -RT_ERROR;
    if (len > 8)
        len = 8;

    rt_memset(&msg, 0, sizeof(msg));
    msg.id  = id;
    msg.ide = (id > 0x7FFu) ? RT_CAN_EXTID : RT_CAN_STDID;
    msg.rtr = RT_CAN_DTR;
    msg.len = len;
    rt_memcpy(msg.data, data, len);

    return (rt_device_write(can_dev, 0, &msg, sizeof(msg)) == sizeof(msg))
           ? RT_EOK : -RT_EIO;
}

rt_uint32_t app_can_rx_count(void)
{
    return rx_cnt;
}

void app_can_last(rt_uint32_t *id, rt_uint8_t *len)
{
    if (id)  *id  = last_id;
    if (len) *len = last_len;
}

#else

rt_bool_t   app_can_ready(void) { return RT_FALSE; }
rt_err_t    app_can_init(void)  { return -RT_ENOSYS; }
rt_err_t    app_can_send(rt_uint32_t id, rt_uint8_t len, const rt_uint8_t *d)
{
    (void)id; (void)len; (void)d; return -RT_ENOSYS;
}
rt_uint32_t app_can_rx_count(void) { return 0; }
void        app_can_last(rt_uint32_t *id, rt_uint8_t *len) { (void)id; (void)len; }

#endif /* BSP_USING_CAN && RT_USING_CAN */
