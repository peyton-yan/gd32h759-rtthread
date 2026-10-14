/*
 * tcp_echo.c — TCP echo server (M3 milestone)
 *
 * Blog notes:
 *  - pure BSD socket API via RT-Thread lwIP (sal abstraction)
 *  - one server thread + one handler thread per client (2 clients max here)
 *  - finsh: tcp_echo_start [port]  (default 5000)
 *           tcp_echo_stop
 */

#include <rtthread.h>
#include <string.h>
#include <sys/socket.h>
#include <lwip/inet.h>

#define DBG_TAG              "app.tcpecho"
#define DBG_LVL              DBG_INFO
#include <rtdbg.h>

#define TCP_ECHO_DEFAULT_PORT  5000
#define TCP_ECHO_BUF_SIZE      1024
#define TCP_ECHO_MAX_CLIENTS   2

static int listen_fd = -1;

static void tcp_echo_client(void *arg)
{
    int fd = (int)(rt_ubase_t)arg;
    rt_uint8_t *buf = rt_malloc(TCP_ECHO_BUF_SIZE);

    if (buf == RT_NULL) {
        LOG_E("no mem for client buffer");
        goto __exit;
    }

    for (;;) {
        int len = recv(fd, buf, TCP_ECHO_BUF_SIZE, 0);
        if (len <= 0) {                 /* 0 = peer closed, <0 = error */
            break;
        }
        int sent = 0;
        while (sent < len) {            /* send() may return partial */
            int ret = send(fd, buf + sent, len - sent, 0);
            if (ret <= 0) {
                goto __exit;
            }
            sent += ret;
        }
    }

__exit:
    if (buf) {
        rt_free(buf);
    }
    closesocket(fd);
    LOG_I("client fd=%d closed", fd);
}

static void tcp_echo_server(void *arg)
{
    int port = (int)(rt_ubase_t)arg;
    struct sockaddr_in addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        LOG_E("socket failed");
        return;
    }

    rt_memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_E("bind failed");
        goto __exit;
    }
    if (listen(listen_fd, TCP_ECHO_MAX_CLIENTS) < 0) {
        LOG_E("listen failed");
        goto __exit;
    }
    LOG_I("tcp echo server on port %d", port);

    for (;;) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(listen_fd, (struct sockaddr *)&client, &len);
        if (fd < 0) {
            break;                      /* listen_fd closed by stop cmd */
        }
        LOG_I("client %s:%d connected, fd=%d",
              inet_ntoa(client.sin_addr), ntohs(client.sin_port), fd);

        rt_thread_t tid = rt_thread_create("tcp_cli", tcp_echo_client,
                                           (void *)(rt_ubase_t)fd,
                                           2048, 20, 10);
        if (tid != RT_NULL) {
            rt_thread_startup(tid);
        } else {
            LOG_E("no thread for client, drop fd=%d", fd);
            closesocket(fd);
        }
    }

__exit:
    if (listen_fd >= 0) {
        closesocket(listen_fd);
        listen_fd = -1;
    }
    LOG_I("tcp echo server stopped");
}

static void tcp_echo_start(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : TCP_ECHO_DEFAULT_PORT;

    if (listen_fd >= 0) {
        rt_kprintf("server already running\n");
        return;
    }
    rt_thread_t tid = rt_thread_create("tcp_srv", tcp_echo_server,
                                       (void *)(rt_ubase_t)port,
                                       2048, 20, 10);
    if (tid != RT_NULL) {
        rt_thread_startup(tid);
    }
}
MSH_CMD_EXPORT(tcp_echo_start, tcp echo server: tcp_echo_start [port]);

static void tcp_echo_stop(int argc, char **argv)
{
    if (listen_fd >= 0) {
        closesocket(listen_fd);         /* 让 accept() 退出 */
        listen_fd = -1;
    }
}
MSH_CMD_EXPORT(tcp_echo_stop, stop tcp echo server);