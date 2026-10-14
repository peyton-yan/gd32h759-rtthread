/*
 * app_at24.c — AT24C02 EEPROM 可复用实现（走 RT-Thread I2C 框架，I2C1）
 * 见 at24_test.c 的坑位说明：页大小 8 字节 + 写周期 ACK polling。
 */
#include "app_at24.h"
#include <rtdevice.h>

#if defined(BSP_USING_I2C1)

/* 随机读：msg0 写地址指针（RELOAD 保持总线）-> repeat-start -> msg1 读 */
rt_err_t at24_read_bytes(rt_uint8_t addr, rt_uint8_t *buf, rt_uint16_t len)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = AT24_ADDR;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &addr;
    msgs[0].len   = 1;

    msgs[1].addr  = AT24_ADDR;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = buf;
    msgs[1].len   = len;

    return (rt_i2c_transfer(rt_i2c_bus_device_find("i2c1"), msgs, 2) == 2)
           ? RT_EOK : -RT_EIO;
}

/* 等待内部写周期结束：期间器件对任何访问都 NACK */
static rt_err_t at24_wait_ready(void)
{
    struct rt_i2c_bus_device *bus = rt_i2c_bus_device_find("i2c1");
    rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(AT24_WR_POLL_MS);
    rt_uint8_t dummy;

    while (rt_tick_get() < deadline)
    {
        struct rt_i2c_msg msg = { AT24_ADDR, RT_I2C_RD, 1, &dummy };
        if (rt_i2c_transfer(bus, &msg, 1) == 1)
            return RT_EOK;
    }
    return -RT_ETIMEOUT;
}

/* 单页写：[memaddr, d0..dn] 一条消息发完（页内不跨界由调用者保证） */
static rt_err_t at24_write_page(rt_uint8_t addr, const rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t frame[1 + AT24_PAGE];
    struct rt_i2c_msg msg;

    frame[0] = addr;
    rt_memcpy(&frame[1], data, len);

    msg.addr  = AT24_ADDR;
    msg.flags = RT_I2C_WR;
    msg.buf   = frame;
    msg.len   = len + 1;

    if (rt_i2c_transfer(rt_i2c_bus_device_find("i2c1"), &msg, 1) != 1)
        return -RT_EIO;
    return at24_wait_ready();
}

rt_err_t at24_write_bytes(rt_uint8_t addr, const rt_uint8_t *data, rt_uint16_t len)
{
    while (len)
    {
        rt_uint16_t room = AT24_PAGE - (addr % AT24_PAGE);
        rt_uint16_t n = (len < room) ? len : room;
        rt_err_t ret = at24_write_page(addr, data, n);
        if (ret != RT_EOK)
            return ret;
        addr += n; data += n; len -= n;
    }
    return RT_EOK;
}

#else  /* !BSP_USING_I2C1 */

rt_err_t at24_read_bytes(rt_uint8_t addr, rt_uint8_t *buf, rt_uint16_t len)
{
    (void)addr; (void)buf; (void)len;
    return -RT_ENOSYS;
}

rt_err_t at24_write_bytes(rt_uint8_t addr, const rt_uint8_t *data, rt_uint16_t len)
{
    (void)addr; (void)data; (void)len;
    return -RT_ENOSYS;
}

#endif /* BSP_USING_I2C1 */
