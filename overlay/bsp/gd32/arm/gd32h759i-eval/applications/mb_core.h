/*
 * mb_core.h — Modbus PDU 核心（传输无关、板级无关）
 */
#ifndef MB_CORE_H
#define MB_CORE_H

#include <rtthread.h>

/*
 * 支持的功能码 (FC):
 *   0x01 读线圈 | 0x02 读离散输入 | 0x03 读保持寄存器 | 0x04 读输入寄存器
 *   0x05 写单线圈 | 0x06 写单寄存器 | 0x0F 写多线圈 | 0x10 写多寄存器
 * 未注册回调 (NULL) 的功能码 -> 非法功能码异常
 *
 * 数据映射回调：应用层填表注册。返回 0 成功，
 *   -RT_ERROR 表示非法地址，-RT_EINVAL 表示非法值
 */
struct mb_map_ops {
    /* val 为出参；write 返回 0/-RT_ERROR/-RT_EINVAL(非法值) */
    int (*read_coil)(rt_uint16_t addr, rt_uint8_t *val);
    int (*write_coil)(rt_uint16_t addr, rt_uint8_t val);
    int (*read_din)(rt_uint16_t addr, rt_uint8_t *val);
    int (*read_ireg)(rt_uint16_t addr, rt_uint16_t *val);   /* FC 0x04 */
    int (*read_hreg)(rt_uint16_t addr, rt_uint16_t *val);
    int (*write_hreg)(rt_uint16_t addr, rt_uint16_t val);
};

/*
 * 处理一个请求 PDU，响应写回同一缓冲区。
 * @pdu  输入请求 PDU，也是输出响应缓冲区（至少 260 字节）
 * @len  请求 PDU 长度
 * @返回 响应 PDU 长度
 */
rt_size_t mb_core_handle(const struct mb_map_ops *ops,
                         rt_uint8_t *pdu, rt_size_t len);

#endif