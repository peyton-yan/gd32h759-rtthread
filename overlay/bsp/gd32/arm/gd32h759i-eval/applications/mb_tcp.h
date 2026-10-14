#ifndef MB_TCP_H
#define MB_TCP_H

#include "mb_core.h"

/* 启动/停止 Modbus TCP 从站；ops 由应用层提供 */
rt_err_t mb_tcp_start(const struct mb_map_ops *ops, rt_uint16_t port);
void     mb_tcp_stop(void);

#endif