/*
 * app_at24.h — 板载 AT24C02 EEPROM 可复用读写接口（I2C1）
 * 从 at24_test.c 抽出，供点表等复用。
 */
#ifndef APP_AT24_H
#define APP_AT24_H

#include <rtthread.h>

#define AT24_ADDR       0x50    /* 7 位地址（A2A1A0 接地） */
#define AT24_SIZE       256     /* 字节数 */
#define AT24_PAGE       8       /* 页大小 */
#define AT24_WR_POLL_MS 50      /* ACK polling 总预算 */

/* 随机读 / 分页写（自动处理页边界与写周期 ACK polling） */
rt_err_t at24_read_bytes(rt_uint8_t addr, rt_uint8_t *buf, rt_uint16_t len);
rt_err_t at24_write_bytes(rt_uint8_t addr, const rt_uint8_t *data, rt_uint16_t len);

#endif /* APP_AT24_H */
