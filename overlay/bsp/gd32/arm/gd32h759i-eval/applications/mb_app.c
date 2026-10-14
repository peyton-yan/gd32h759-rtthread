/*
 * mb_app.c — 应用装配
 * 启动 Modbus TCP 从站：外设初始化 + 安装点表(mb_points.c) + 监听
 */
#include "mb_pointtable.h"
#include "mb_tcp.h"
#include "mb_points.h"

#include <stdlib.h>

#define DBG_TAG "mb.app"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static void modbus_start(int argc, char **argv)
{
    rt_uint16_t port = (argc > 1) ? (rt_uint16_t)atoi(argv[1]) : 502;
    rt_size_t   n;

    n = mb_points_register();
    if (mb_tcp_start(mb_pointtable_ops(), port) != RT_EOK) {
        rt_kprintf("modbus start failed (busy?), %u points ready\n", (unsigned)n);
        return;
    }
    rt_kprintf("modbus tcp server on port %u, %u points\n", port, (unsigned)n);
}
MSH_CMD_EXPORT(modbus_start, modbus tcp server: modbus_start [port]);

static void modbus_stop(int argc, char **argv)
{
    mb_tcp_stop();
}
MSH_CMD_EXPORT(modbus_stop, stop modbus tcp server);

static void modbus_points(int argc, char **argv)
{
    mb_points_register();
    rt_kprintf("point table reloaded\n");
}
MSH_CMD_EXPORT(modbus_points, reload modbus point table);

/* ------------------------------------------------------------------ */
/* 帧自检：本地构造请求 -> mb_core_handle -> 校验响应 PDU 格式           */
/* 标准读响应 = [fc][字节数][数据...]；异常 = [fc|0x80][异常码]          */
/* 不依赖 PC / 网络，直接验证设备侧协议实现                             */
/* ------------------------------------------------------------------ */
static int mb_probe(const char *name, const rt_uint8_t *req, rt_size_t req_len,
                    rt_size_t exp_len, rt_uint8_t exp_bc, rt_bool_t expect_exc)
{
    static rt_uint8_t pdu[260];
    rt_size_t n;
    rt_uint8_t bc;
    int ok;

    rt_memcpy(pdu, req, req_len);
    n = mb_core_handle(mb_pointtable_ops(), pdu, req_len);
    bc = (n > 1) ? pdu[1] : 0;

    if (expect_exc) {
        ok = (n == exp_len) && ((pdu[0] & 0x80) != 0) && ((pdu[0] & 0x7F) == req[0]);
    } else {
        ok = (n == exp_len) && (pdu[0] == req[0]) &&
             (exp_bc == 0xFF || bc == exp_bc);
    }

    rt_kprintf("[mb_selftest] %-24s %s  len=%u(exp %u) bc=%u(exp %u)\n",
               name, ok ? "PASS" : "FAIL",
               (unsigned)n, (unsigned)exp_len, (unsigned)bc, (unsigned)exp_bc);
    if (!ok) {
        rt_uint32_t i;
        rt_kprintf("    got:");
        for (i = 0; i < n; i++) rt_kprintf(" %02X", pdu[i]);
        rt_kprintf("   (exp len=%u bc=%u)\n", (unsigned)exp_len, (unsigned)exp_bc);
    }
    return ok;
}

static void mb_selftest(int argc, char **argv)
{
    rt_uint8_t r[8];
    int fails = 0;

    (void)argc; (void)argv;
    mb_points_register();   /* 确保点表与外设就绪 */

    rt_kprintf("[mb_selftest] ---- Modbus 响应帧自检 ----\n");

    /* FC03 读保持寄存器 0x0300 qty1  -> [03][02][hi][lo] len4 */
    r[0]=0x03; r[1]=0x03; r[2]=0x00; r[3]=0x00; r[4]=0x01;
    fails += !mb_probe("FC03 hreg 0x0300 x1", r, 5, 4, 2, RT_FALSE);

    /* FC04 读输入寄存器 0x0000 qty1 (SYS_MODEL=0x0759) -> len4 */
    r[0]=0x04; r[1]=0x00; r[2]=0x00; r[3]=0x00; r[4]=0x01;
    fails += !mb_probe("FC04 ireg 0x0000 x1", r, 5, 4, 2, RT_FALSE);

    /* FC04 读 RTC 0x0400 qty6 -> len 2+12=14 bc=12 */
    r[0]=0x04; r[1]=0x04; r[2]=0x00; r[3]=0x00; r[4]=0x06;
    fails += !mb_probe("FC04 ireg 0x0400 x6", r, 5, 14, 12, RT_FALSE);

    /* FC01 读线圈 0x0000 qty2 -> [01][01][bits] len3 bc1 */
    r[0]=0x01; r[1]=0x00; r[2]=0x00; r[3]=0x00; r[4]=0x02;
    fails += !mb_probe("FC01 coil 0x0000 x2", r, 5, 3, 1, RT_FALSE);

    /* FC02 读离散输入 0x0000 qty3 -> len3 bc1 */
    r[0]=0x02; r[1]=0x00; r[2]=0x00; r[3]=0x00; r[4]=0x03;
    fails += !mb_probe("FC02 din 0x0000 x3", r, 5, 3, 1, RT_FALSE);

    /* FC01 读线圈 0x0700 qty1 (LOG_ENABLE) -> len3 bc1 */
    r[0]=0x01; r[1]=0x07; r[2]=0x00; r[3]=0x00; r[4]=0x01;
    fails += !mb_probe("FC01 coil 0x0700 x1", r, 5, 3, 1, RT_FALSE);

    /* FC05 写线圈 0x0000=1 -> 回显 5 字节 */
    r[0]=0x05; r[1]=0x00; r[2]=0x00; r[3]=0xFF; r[4]=0x00;
    fails += !mb_probe("FC05 coil 0x0000=1", r, 5, 5, 0xFF, RT_FALSE);

    /* FC06 写保持寄存器 0x0300=2048 -> 回显 5 字节 */
    r[0]=0x06; r[1]=0x03; r[2]=0x00; r[3]=0x08; r[4]=0x00;
    fails += !mb_probe("FC06 hreg 0x0300=2048", r, 5, 5, 0xFF, RT_FALSE);

    /* FC03 非法地址 0x7FFF -> 异常 [83][02] len2 */
    r[0]=0x03; r[1]=0x7F; r[2]=0xFF; r[3]=0x00; r[4]=0x01;
    fails += !mb_probe("FC03 illegal addr", r, 5, 2, 0xFF, RT_TRUE);

    rt_kprintf("[mb_selftest] %s (%d fail)\n", fails ? "FAIL" : "ALL PASS", fails);
}
MSH_CMD_EXPORT(mb_selftest, Modbus response-frame self-test (no PC needed));
