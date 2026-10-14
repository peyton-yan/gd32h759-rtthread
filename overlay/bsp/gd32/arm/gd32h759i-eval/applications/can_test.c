/*
 * can_test.c — RT-Thread 标准 CAN 设备 API 验证
 * can_test loop : can1 回环自测（免接线）
 * can_test net  : can2 发 -> can1 收（需 JP12<->JP14 H-H L-L，跳线见笔记）
 */
#include <rtthread.h>
#include <rtdevice.h>

static rt_err_t can_rx_ind(rt_device_t dev, rt_size_t size)
{
    struct rt_can_msg msg;
    while (rt_device_read(dev, 0, &msg, sizeof(msg)) == sizeof(msg))
    {
        rt_kprintf("[CAN] rx id=0x%lX len=%d data=", msg.id, msg.len);
        for (int i = 0; i < msg.len; i++) rt_kprintf("%02X ", msg.data[i]);
        rt_kprintf("\n");
    }
    return RT_EOK;
}

static void can_test(int argc, char **argv)
{
    rt_bool_t loop = (argc > 1 && !rt_strcmp(argv[1], "loop"));
    const char *txname = loop ? "can1" : "can2";
    rt_device_t rxdev = rt_device_find("can1");
    rt_device_t txdev = rt_device_find(txname);

    if (!rxdev || !txdev)
    {
        rt_kprintf("can device not found, check menuconfig\n");
        return;
    }

    rt_device_control(rxdev, RT_CAN_CMD_SET_BAUD, (void *)CAN1MBaud);
    rt_device_control(txdev, RT_CAN_CMD_SET_BAUD, (void *)CAN1MBaud);

    if (loop) {
        rt_device_control(rxdev, RT_CAN_CMD_SET_MODE, (void *)RT_CAN_MODE_LOOPBACK);
    } else {
        rt_device_control(rxdev, RT_CAN_CMD_SET_MODE, (void *)RT_CAN_MODE_NORMAL);
        rt_device_control(txdev, RT_CAN_CMD_SET_MODE, (void *)RT_CAN_MODE_NORMAL);
    }

    rt_device_open(rxdev, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX);
    if (txdev != rxdev)
        rt_device_open(txdev, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_INT_TX);
    rt_device_set_rx_indicate(rxdev, can_rx_ind);

    struct rt_can_msg msg = {0};
    msg.id = 0x55; msg.ide = RT_CAN_STDID; msg.rtr = RT_CAN_DTR; msg.len = 8;
    for (int i = 0; i < 8; i++) msg.data[i] = 0xA1 + i;

    rt_kprintf("[CAN] %s tx id=0x55, A1..A8\n", txname);
    if (rt_device_write(txdev, 0, &msg, sizeof(msg)) != sizeof(msg))
        rt_kprintf("[CAN] tx failed\n");
}
MSH_CMD_EXPORT(can_test, can test: can_test loop | net);