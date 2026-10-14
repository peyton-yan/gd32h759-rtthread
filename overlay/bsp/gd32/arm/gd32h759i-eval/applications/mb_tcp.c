/*
 * mb_tcp.c — Modbus TCP 传输后端
 * MBAP: [tid:2][pid:2=0][len:2(含uid)][uid:1] + PDU
 * 收包：先收 7 字节头，再按 len 收体（TCP 粘包的最小正确姿势）
 */
#include "mb_tcp.h"
#include <string.h>
#include <sys/socket.h>
#include <lwip/inet.h>

#define DBG_TAG              "mb.tcp"
#define DBG_LVL              DBG_INFO
#include <rtdbg.h>

#define MB_TCP_BUF_SIZE   260

static int mb_listen_fd = -1;
static const struct mb_map_ops *mb_ops;

static rt_uint16_t get_be16(const rt_uint8_t *p)
{
    return ((rt_uint16_t)p[0] << 8) | p[1];
}

static void put_be16(rt_uint8_t *p, rt_uint16_t v)
{
    p[0] = (rt_uint8_t)(v >> 8);
    p[1] = (rt_uint8_t)(v & 0xFF);
}

static int recv_full(int fd, rt_uint8_t *buf, rt_size_t len)
{
    rt_size_t got = 0;
    while (got < len) {
        int r = recv(fd, buf + got, len - got, 0);
        if (r <= 0) {
            return -RT_ERROR;
        }
        got += r;
    }
    return RT_EOK;
}

static void mb_tcp_client(void *arg)
{
    int fd = (int)(rt_ubase_t)arg;
    rt_uint8_t *buf = rt_malloc(MB_TCP_BUF_SIZE);

    if (buf == RT_NULL) {
        goto __exit;
    }

    for (;;) {
        if (recv_full(fd, buf, 7) < 0) {
            break;
        }
        rt_uint16_t plen = get_be16(&buf[4]);    /* 含 uid */
        if (plen < 2 || 7 + plen - 1 > MB_TCP_BUF_SIZE) {
            break;
        }
        if (recv_full(fd, buf + 7, plen - 1) < 0) {
            break;
        }

        rt_size_t resp_len = mb_core_handle(mb_ops, &buf[7], plen - 1);
        put_be16(&buf[4], (rt_uint16_t)(resp_len + 1));

        rt_size_t total = 7 + resp_len, sent = 0;
        while (sent < total) {
            int r = send(fd, buf + sent, total - sent, 0);
            if (r <= 0) {
                goto __exit;
            }
            sent += r;
        }
    }

__exit:
    if (buf) {
        rt_free(buf);
    }
    closesocket(fd);
    LOG_I("client fd=%d closed", fd);
}

static void mb_tcp_server(void *arg)
{
    rt_uint16_t port = (rt_uint16_t)(rt_ubase_t)arg;
    struct sockaddr_in addr;

    mb_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mb_listen_fd < 0) {
        LOG_E("socket failed");
        return;
    }
    rt_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(mb_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(mb_listen_fd, 2) < 0) {
        LOG_E("bind/listen failed, port %u", port);
        goto __exit;
    }
    LOG_I("modbus tcp server on port %u", port);

    for (;;) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(mb_listen_fd, (struct sockaddr *)&client, &len);
        if (fd < 0) {
            break;
        }
        LOG_I("client %s:%u", inet_ntoa(client.sin_addr),
              ntohs(client.sin_port));
        rt_thread_t tid = rt_thread_create("mb_cli", mb_tcp_client,
                                           (void *)(rt_ubase_t)fd, 2048, 21, 10);
        if (tid != RT_NULL) {
            rt_thread_startup(tid);
        } else {
            closesocket(fd);
        }
    }

__exit:
    if (mb_listen_fd >= 0) {
        closesocket(mb_listen_fd);
        mb_listen_fd = -1;
    }
    LOG_I("modbus tcp server stopped");
}

rt_err_t mb_tcp_start(const struct mb_map_ops *ops, rt_uint16_t port)
{
    if (mb_listen_fd >= 0) {
        return -RT_EBUSY;
    }
    if (ops == RT_NULL) {
        return -RT_EINVAL;
    }
    mb_ops = ops;
    rt_thread_t tid = rt_thread_create("mb_srv", mb_tcp_server,
                                       (void *)(rt_ubase_t)port, 2048, 21, 10);
    if (tid == RT_NULL) {
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);
    return RT_EOK;
}

void mb_tcp_stop(void)
{
    if (mb_listen_fd >= 0) {
        closesocket(mb_listen_fd);
        mb_listen_fd = -1;
    }
}