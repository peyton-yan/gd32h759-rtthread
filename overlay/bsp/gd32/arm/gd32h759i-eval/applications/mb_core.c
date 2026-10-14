/*
 * mb_core.c — Modbus PDU 解析与分发
 * 支持 FC: 0x01/0x02/0x03/0x04/0x05/0x06/0x0F/0x10
 * 未注册的回调 → 非法功能码异常
 */
#include "mb_core.h"
#include <string.h>

#define MB_EX_ILLEGAL_FUNC    0x01
#define MB_EX_ILLEGAL_ADDR    0x02
#define MB_EX_ILLEGAL_VALUE   0x03

static rt_uint16_t get_be16(const rt_uint8_t *p)
{
    return ((rt_uint16_t)p[0] << 8) | p[1];
}

static void put_be16(rt_uint8_t *p, rt_uint16_t v)
{
    p[0] = (rt_uint8_t)(v >> 8);
    p[1] = (rt_uint8_t)(v & 0xFF);
}

static rt_size_t mb_exception(rt_uint8_t *pdu, rt_uint8_t fc, rt_uint8_t code)
{
    pdu[0] = fc | 0x80;
    pdu[1] = code;
    return 2;
}

static rt_size_t mb_read_bits(int (*fn)(rt_uint16_t, rt_uint8_t *),
                              rt_uint8_t *pdu, rt_uint8_t fc)
{
    rt_uint16_t addr = get_be16(&pdu[1]);
    rt_uint16_t qty  = get_be16(&pdu[3]);
    rt_uint8_t bytes, i;

    if (fn == RT_NULL) {
        return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
    }
    if (qty < 1 || qty > 2000) {
        return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
    }
    bytes = (rt_uint8_t)((qty + 7) / 8);
    rt_memset(&pdu[2], 0, bytes);
    for (i = 0; i < qty; i++) {
        rt_uint8_t v;
        if (fn(addr + i, &v) < 0) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_ADDR);
        }
        if (v) {
            pdu[2 + i / 8] |= (rt_uint8_t)(1U << (i % 8));
        }
    }
    pdu[1] = bytes;                 /* 字节数在 pdu[1]，数据从 pdu[2] 起（标准 Modbus） */
    return 2 + bytes;
}

static rt_size_t mb_read_regs(int (*fn)(rt_uint16_t, rt_uint16_t *),
                              rt_uint8_t *pdu, rt_uint8_t fc)
{
    rt_uint16_t addr = get_be16(&pdu[1]);
    rt_uint16_t qty  = get_be16(&pdu[3]);
    rt_uint16_t i;

    if (fn == RT_NULL) {
        return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
    }
    if (qty < 1 || qty > 125) {
        return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
    }
    for (i = 0; i < qty; i++) {
        rt_uint16_t v;
        if (fn(addr + i, &v) < 0) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_ADDR);
        }
        put_be16(&pdu[2 + i * 2], v);
    }
    pdu[1] = (rt_uint8_t)(qty * 2);   /* 字节数在 pdu[1]，数据从 pdu[2] 起 */
    return 2 + qty * 2;
}

rt_size_t mb_core_handle(const struct mb_map_ops *ops,
                         rt_uint8_t *pdu, rt_size_t len)
{
    rt_uint8_t fc = pdu[0];

    if (len < 5) {
        return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
    }

    switch (fc) {
    case 0x01:
        return mb_read_bits(ops->read_coil, pdu, fc);
    case 0x02:
        return mb_read_bits(ops->read_din, pdu, fc);
    case 0x03:
        return mb_read_regs(ops->read_hreg, pdu, fc);
    case 0x04:
        return mb_read_regs(ops->read_ireg, pdu, fc);
    case 0x05: {    /* write single coil: echo on success */
        rt_uint16_t addr = get_be16(&pdu[1]);
        rt_uint16_t v    = get_be16(&pdu[3]);
        int ret;
        if (ops->write_coil == RT_NULL) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
        }
        if (v != 0xFF00 && v != 0x0000) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
        }
        ret = ops->write_coil(addr, v == 0xFF00);
        if (ret == -RT_EINVAL) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
        }
        if (ret < 0) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_ADDR);
        }
        return len;
    }
    case 0x06: {    /* write single register: echo on success */
        rt_uint16_t addr = get_be16(&pdu[1]);
        rt_uint16_t v    = get_be16(&pdu[3]);
        int ret;
        if (ops->write_hreg == RT_NULL) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
        }
        ret = ops->write_hreg(addr, v);
        if (ret == -RT_EINVAL) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
        }
        if (ret < 0) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_ADDR);
        }
        return len;
    }
    case 0x0F: {    /* write multiple coils: echo addr+qty on success */
        rt_uint16_t addr = get_be16(&pdu[1]);
        rt_uint16_t qty  = get_be16(&pdu[3]);
        rt_uint16_t i;
        if (ops->write_coil == RT_NULL) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
        }
        if (len < 6 || qty < 1 || qty > 1968 ||
            pdu[4] != (rt_uint8_t)((qty + 7) / 8)) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
        }
        for (i = 0; i < qty; i++) {
            rt_uint8_t v = (pdu[5 + i / 8] >> (i % 8)) & 0x01;
            int ret = ops->write_coil(addr + i, v);
            if (ret == -RT_EINVAL) {
                return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
            }
            if (ret < 0) {
                return mb_exception(pdu, fc, MB_EX_ILLEGAL_ADDR);
            }
        }
        return 5;
    }
    case 0x10: {    /* write multiple registers: echo addr+qty on success */
        rt_uint16_t addr = get_be16(&pdu[1]);
        rt_uint16_t qty  = get_be16(&pdu[3]);
        rt_uint16_t i;
        if (ops->write_hreg == RT_NULL) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
        }
        if (len < 6 || qty < 1 || qty > 123 || pdu[4] != (rt_uint8_t)(qty * 2)) {
            return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
        }
        for (i = 0; i < qty; i++) {
            rt_uint16_t v = get_be16(&pdu[5 + i * 2]);
            int ret = ops->write_hreg(addr + i, v);
            if (ret == -RT_EINVAL) {
                return mb_exception(pdu, fc, MB_EX_ILLEGAL_VALUE);
            }
            if (ret < 0) {
                return mb_exception(pdu, fc, MB_EX_ILLEGAL_ADDR);
            }
        }
        return 5;
    }
    default:
        return mb_exception(pdu, fc, MB_EX_ILLEGAL_FUNC);
    }
}