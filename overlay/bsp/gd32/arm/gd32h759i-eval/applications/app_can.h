/*
 * app_can.h — CAN 从站服务（can1）：接收计数 + 发送
 * 点表用；与 can_test.c 的测试命令相互独立。
 */
#ifndef APP_CAN_H
#define APP_CAN_H

#include <rtthread.h>

rt_bool_t   app_can_ready(void);
rt_err_t    app_can_init(void);
rt_err_t    app_can_send(rt_uint32_t id, rt_uint8_t len, const rt_uint8_t *data);
rt_uint32_t app_can_rx_count(void);
void        app_can_last(rt_uint32_t *id, rt_uint8_t *len);

#endif /* APP_CAN_H */
