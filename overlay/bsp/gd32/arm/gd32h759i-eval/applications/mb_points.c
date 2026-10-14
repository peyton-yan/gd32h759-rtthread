/*
 * mb_points.c — 本机 Modbus 点表（单一事实来源）
 *
 * 地址分块：高字节表示子系统
 *   0x00xx 系统   | 0x01xx LED/按键 | 0x02xx ADC    | 0x03xx DAC
 *   0x04xx RTC    | 0x05xx EEPROM   | 0x06xx Flash  | 0x07xx SD/记录
 *   0x08xx CAN    | 0x09xx 触摸     | 0x0Axx 音频   | 0x0Bxx USB
 *   0x0Cxx ENET   | 0x0Dxx 闭环控制 | 0x0Exx 电机(预留)
 *
 * 本文件同时是 tools/gen_points_json.py 的输入：MB_PT(...) 必须保持单行。
 */
#include "mb_points.h"
#include "mb_pointtable.h"

#include <rtthread.h>
#include <rtdevice.h>
#include <drv_gpio.h>
#include <time.h>

#include "app_at24.h"
#include "app_can.h"
#include "app_log.h"
#include "app_loop.h"

#if defined(RT_USING_FAL)
#include <fal.h>
#endif
#if defined(RT_USING_LWIP)
#include <lwip/netif.h>
#include <lwip/dhcp.h>
#include <netif/ethernetif.h>

/* 取 lwIP netif：用 eth_device->netif，比 netif_find("e0") 可靠
 * （netif_find 依赖 netif 名字/编号，实测对 "e0" 常返回 NULL） */
static struct netif *h7_netif(void)
{
    struct eth_device *dev = (struct eth_device *)rt_device_find("e0");
    return (dev != RT_NULL) ? (struct netif *)dev->netif : RT_NULL;
}
#endif

#define DBG_TAG "mb.points"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ================================================================== */
/* 0x00xx 系统                                                          */
/* ================================================================== */
static rt_uint16_t sys_model = 0x0759;   /* GD32H759 */
static rt_uint16_t sys_fw    = 0x0100;   /* v1.0 */

static int hook_uptime(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_uint32_t s;

    (void)area;
    if (write) {
        return -RT_ERROR;   /* 只读 */
    }
    s = rt_tick_get() / RT_TICK_PER_SECOND;
    *(rt_uint16_t *)val = (addr == 0x0002) ? (rt_uint16_t)(s >> 16)
                                           : (rt_uint16_t)(s & 0xFFFFu);
    return 0;
}

/* ================================================================== */
/* 0x01xx LED / 按键                                                    */
/* ================================================================== */
#define LED1_PIN       GET_PIN(F, 10)
#define LED2_PIN       GET_PIN(A, 6)
#define KEY_WAKEUP_PIN GET_PIN(A, 0)
#define KEY_TAMPER_PIN GET_PIN(C, 13)
#define KEY_USER_PIN   GET_PIN(F, 8)   /* TODO: 核对硬件实际引脚（README 写 PE4） */

/* LED 状态软件镜像：部分 MCU 上 rt_pin_read() 回读输出引脚不可靠（读到 0），
 * 会使上位机"翻转"永远写 1。用镜像保证读回的就是当前状态（与旧 mb_app.c 同思路）。 */
static rt_uint8_t led_state[2];

static int hook_led(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    int idx = (addr == 0x0000) ? 0 : 1;
    rt_base_t pin = idx ? LED2_PIN : LED1_PIN;

    (void)area;
    if (write) {
        led_state[idx] = (*(rt_uint8_t *)val) ? 1 : 0;
        rt_pin_write(pin, led_state[idx] ? PIN_HIGH : PIN_LOW);
    } else {
        *(rt_uint8_t *)val = led_state[idx];
    }
    return 0;
}

static int hook_key(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_base_t pin;

    (void)area;
    if (write) {
        return -RT_ERROR;   /* 只读 */
    }
    pin = (addr == 0x0000) ? KEY_WAKEUP_PIN
        : (addr == 0x0001) ? KEY_TAMPER_PIN : KEY_USER_PIN;
    *(rt_uint8_t *)val = rt_pin_read(pin) ? 1 : 0;
    return 0;
}

/* ================================================================== */
/* 0x02xx ADC                                                           */
/* ================================================================== */
#if defined(BSP_USING_ADC) && defined(RT_USING_ADC)
static rt_adc_device_t adc_dev;

static int hook_adc(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    (void)area;
    if (write || adc_dev == RT_NULL) {
        return -RT_ERROR;
    }
    *(rt_uint16_t *)val = (rt_uint16_t)rt_adc_read(adc_dev, addr & 0xFFu);
    return 0;
}
#endif

/* ================================================================== */
/* 0x03xx DAC                                                           */
/* ================================================================== */
#if defined(BSP_USING_DAC) && defined(RT_USING_DAC)
static rt_dac_device_t dac_dev;
static rt_uint16_t     dac_shadow;
#define DAC_CH 1

static int hook_dac(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    (void)area;
    if (dac_dev == RT_NULL) {
        return -RT_ERROR;
    }
    if (write) {
        dac_shadow = *(rt_uint16_t *)val;
        rt_dac_write(dac_dev, DAC_CH, dac_shadow);
    } else {
        *(rt_uint16_t *)val = dac_shadow;
    }
    return 0;
}
#endif

/* ================================================================== */
/* 0x04xx RTC                                                           */
/* ================================================================== */
#if defined(BSP_USING_RTC) && defined(RT_USING_RTC)
static rt_device_t rtc_dev;
static struct tm   rtc_shadow;

static int hook_rtc(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_uint16_t field = addr & 0xFFu;
    struct tm  *ptm;
    time_t      t;

    (void)area;
    if (rtc_dev == RT_NULL || field > 5) {
        return -RT_ERROR;
    }
    if (rt_device_control(rtc_dev, RT_DEVICE_CTRL_RTC_GET_TIME, &t) != RT_EOK) {
        return -RT_ERROR;
    }
    ptm = localtime(&t);
    if (ptm == RT_NULL) {
        return -RT_ERROR;
    }
    rtc_shadow = *ptm;
    rtc_shadow.tm_isdst = -1;

    if (!write) {
        switch (field) {
        case 0: *(rt_uint16_t *)val = (rt_uint16_t)(rtc_shadow.tm_year + 1900); break;
        case 1: *(rt_uint16_t *)val = (rt_uint16_t)(rtc_shadow.tm_mon + 1);     break;
        case 2: *(rt_uint16_t *)val = (rt_uint16_t)rtc_shadow.tm_mday;          break;
        case 3: *(rt_uint16_t *)val = (rt_uint16_t)rtc_shadow.tm_hour;          break;
        case 4: *(rt_uint16_t *)val = (rt_uint16_t)rtc_shadow.tm_min;           break;
        default:*(rt_uint16_t *)val = (rt_uint16_t)rtc_shadow.tm_sec;           break;
        }
        return 0;
    }

    switch (field) {
    case 0: rtc_shadow.tm_year = (int)(*(rt_uint16_t *)val) - 1900; break;
    case 1: rtc_shadow.tm_mon  = (int)(*(rt_uint16_t *)val) - 1;    break;
    case 2: rtc_shadow.tm_mday = (int)(*(rt_uint16_t *)val);        break;
    case 3: rtc_shadow.tm_hour = (int)(*(rt_uint16_t *)val);        break;
    case 4: rtc_shadow.tm_min  = (int)(*(rt_uint16_t *)val);        break;
    default:rtc_shadow.tm_sec  = (int)(*(rt_uint16_t *)val);        break;
    }

    t = mktime(&rtc_shadow);
    if (t == (time_t)-1) {
        return -RT_EINVAL;
    }
    if (rt_device_control(rtc_dev, RT_DEVICE_CTRL_RTC_SET_TIME, &t) != RT_EOK) {
        return -RT_ERROR;
    }
    return 0;
}
#endif

/* ================================================================== */
/* 0x05xx EEPROM (AT24C02)                                              */
/* ================================================================== */
#if defined(BSP_USING_I2C1)
static rt_uint16_t eep_addr;

static int hook_eep(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_uint8_t b;

    if (area == MB_AREA_HREG) {
        if (addr == 0x0500) {           /* EEP_ADDR (RW) */
            if (write) {
                eep_addr = (rt_uint16_t)(*(rt_uint16_t *)val) % AT24_SIZE;
            } else {
                *(rt_uint16_t *)val = eep_addr;
            }
            return 0;
        }
        if (addr == 0x0501) {           /* EEP_WDATA (WO) */
            if (!write) {
                return -RT_ERROR;
            }
            b = (rt_uint8_t)(*(rt_uint16_t *)val);
            return (at24_write_bytes((rt_uint8_t)eep_addr, &b, 1) == RT_EOK)
                   ? 0 : -RT_ERROR;
        }
        return -RT_ERROR;
    }
    if (area == MB_AREA_IREG && addr == 0x0500) {   /* EEP_RDATA (RO) */
        if (at24_read_bytes((rt_uint8_t)eep_addr, &b, 1) != RT_EOK) {
            return -RT_ERROR;
        }
        *(rt_uint16_t *)val = b;
        return 0;
    }
    return -RT_ERROR;
}
#endif

/* ================================================================== */
/* 0x06xx OSPI Flash (fal 分区 "param")                                 */
/* ================================================================== */
#if defined(RT_USING_FAL)
#define FLASH_PARAM_MAGIC 0x50415241u   /* 'PARA' */
#define FLASH_PART_NAME   "param"

static int hook_flash(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    const struct fal_partition *part = fal_partition_find(FLASH_PART_NAME);
    rt_uint32_t v;
    rt_uint32_t magic = FLASH_PARAM_MAGIC;
    rt_uint8_t  buf[4];

    if (part == RT_NULL) {
        return -RT_ERROR;
    }

    if (area == MB_AREA_IREG && addr == 0x0600) {   /* MAGIC 有效标志 (RO) */
        if (fal_partition_read(part, 4, buf, 4) < 0) {
            return -RT_ERROR;
        }
        rt_memcpy(&v, buf, 4);
        *(rt_uint16_t *)val = (v == FLASH_PARAM_MAGIC) ? 1 : 0;
        return 0;
    }

    if (area == MB_AREA_HREG) {                     /* PARAM u32 @ 0x0600/0x0601 */
        if (fal_partition_read(part, 0, buf, 4) < 0) {
            return -RT_ERROR;
        }
        rt_memcpy(&v, buf, 4);
        if (!write) {
            *(rt_uint16_t *)val = (addr == 0x0600)
                ? (rt_uint16_t)(v >> 16) : (rt_uint16_t)(v & 0xFFFFu);
            return 0;
        }
        if (addr == 0x0600) {
            v = (v & 0x0000FFFFu) | ((rt_uint32_t)(*(rt_uint16_t *)val) << 16);
        } else {
            v = (v & 0xFFFF0000u) | (rt_uint32_t)(*(rt_uint16_t *)val);
        }
        fal_partition_erase(part, 0, 4);
        rt_memcpy(buf, &v, 4);
        if (fal_partition_write(part, 0, buf, 4) < 0) {
            return -RT_ERROR;
        }
        fal_partition_write(part, 4, (const rt_uint8_t *)&magic, 4);
        return 0;
    }
    return -RT_ERROR;
}
#endif

/* ================================================================== */
/* 0x07xx SD 卡 / 数据记录                                              */
/* ================================================================== */
#if defined(BSP_USING_SDIO)
static int hook_sd(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    (void)area; (void)addr;
    if (write) {
        return -RT_ERROR;
    }
    *(rt_uint8_t *)val = (rt_device_find("sd0") != RT_NULL) ? 1 : 0;
    return 0;
}
#endif

#if defined(RT_USING_DFS)
static int hook_log(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    if (area == MB_AREA_COIL && addr == 0x0700) {           /* LOG_ENABLE */
        if (write) {
            return (app_log_set(*(rt_uint8_t *)val ? RT_TRUE : RT_FALSE) == RT_EOK)
                   ? 0 : -RT_ERROR;
        }
        *(rt_uint8_t *)val = app_log_enabled() ? 1 : 0;
        return 0;
    }
    if (area == MB_AREA_HREG && addr == 0x0700) {           /* LOG_PERIOD_MS */
        if (write) {
            app_log_set_period(*(rt_uint16_t *)val);
        } else {
            *(rt_uint16_t *)val = (rt_uint16_t)app_log_period();
        }
        return 0;
    }
    if (area == MB_AREA_IREG && (addr == 0x0700 || addr == 0x0701)) {  /* LOG_LINES u32 */
        rt_uint32_t n;
        if (write) {
            return -RT_ERROR;
        }
        n = app_log_lines();
        *(rt_uint16_t *)val = (addr == 0x0700)
            ? (rt_uint16_t)(n >> 16) : (rt_uint16_t)(n & 0xFFFFu);
        return 0;
    }
    return -RT_ERROR;
}
#endif

/* ================================================================== */
/* 0x08xx CAN                                                           */
/* ================================================================== */
#if defined(BSP_USING_CAN) && defined(RT_USING_CAN)
static rt_uint16_t can_tx_id;
static rt_uint16_t can_tx_dlc;
static rt_uint16_t can_tx_data[4];

static int hook_can(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    if (area == MB_AREA_IREG) {
        if (addr == 0x0800 || addr == 0x0801) {             /* RX_CNT u32 */
            rt_uint32_t n;
            if (write) {
                return -RT_ERROR;
            }
            n = app_can_rx_count();
            *(rt_uint16_t *)val = (addr == 0x0800)
                ? (rt_uint16_t)(n >> 16) : (rt_uint16_t)(n & 0xFFFFu);
            return 0;
        }
        if (addr == 0x0802 || addr == 0x0803) {             /* LAST_ID / LAST_DLC */
            rt_uint32_t id; rt_uint8_t dl;
            app_can_last(&id, &dl);
            *(rt_uint16_t *)val = (addr == 0x0802) ? (rt_uint16_t)id : dl;
            return 0;
        }
        return -RT_ERROR;
    }
    if (area == MB_AREA_HREG) {
        switch (addr) {
        case 0x0800:                                        /* TX_ID */
            if (write) can_tx_id  = *(rt_uint16_t *)val; else *(rt_uint16_t *)val = can_tx_id;
            return 0;
        case 0x0801:                                        /* TX_DLC */
            if (write) can_tx_dlc = *(rt_uint16_t *)val; else *(rt_uint16_t *)val = can_tx_dlc;
            return 0;
        case 0x0802: case 0x0803: case 0x0804: case 0x0805: /* TX_DATA0..3 */
            {
                int i = addr - 0x0802;
                if (write) can_tx_data[i] = *(rt_uint16_t *)val;
                else       *(rt_uint16_t *)val = can_tx_data[i];
            }
            return 0;
        case 0x0806:                                        /* TX_TRIG (WO) */
            if (write) {
                rt_uint8_t d[8];
                rt_uint8_t len = (can_tx_dlc > 8) ? 8 : (rt_uint8_t)can_tx_dlc;
                int i;
                for (i = 0; i < 4; i++) {
                    d[i * 2]     = (rt_uint8_t)(can_tx_data[i] >> 8);
                    d[i * 2 + 1] = (rt_uint8_t)(can_tx_data[i] & 0xFFu);
                }
                return (app_can_send(can_tx_id, len, d) == RT_EOK) ? 0 : -RT_ERROR;
            }
            return -RT_ERROR;
        default:
            return -RT_ERROR;
        }
    }
    return -RT_ERROR;
}
#endif

/* ================================================================== */
/* 0x09xx 触摸 (GT911)                                                  */
/* ================================================================== */
#if defined(BSP_USING_TOUCH)
extern rt_err_t gt911_read_point(rt_uint16_t *x, rt_uint16_t *y);

static int hook_touch(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_uint16_t x = 0, y = 0;
    rt_err_t    r;

    (void)addr;
    if (write) {
        return -RT_ERROR;
    }
    r = gt911_read_point(&x, &y);
    if (area == MB_AREA_DIN) {
        *(rt_uint8_t *)val = (r == RT_EOK) ? 1 : 0;
        return 0;
    }
    if (area == MB_AREA_IREG) {
        *(rt_uint16_t *)val = (addr == 0x0900) ? x : y;
        return 0;
    }
    return -RT_ERROR;
}
#endif

/* ================================================================== */
/* 0x0Axx 音频 (HPDF 麦)                                                */
/* ================================================================== */
#if defined(BSP_USING_HPDF)
extern void hpdf_get_level(rt_uint16_t *left, rt_uint16_t *right);

static int hook_audio(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_uint16_t l = 0, r = 0;

    (void)area;
    if (write) {
        return -RT_ERROR;
    }
    hpdf_get_level(&l, &r);
    *(rt_uint16_t *)val = (addr == 0x0A00) ? l : r;
    return 0;
}
#endif

/* ================================================================== */
/* 0x0Bxx USB CDC                                                       */
/* ================================================================== */
#if defined(BSP_USING_USB_CDC)
static int hook_usb(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    (void)area; (void)addr;
    if (write) {
        return -RT_ERROR;
    }
    *(rt_uint8_t *)val = (rt_device_find("usb-acm0") != RT_NULL) ? 1 : 0;
    return 0;
}
#endif

/* ================================================================== */
/* 0x0Cxx ENET                                                          */
/* ================================================================== */
#if defined(RT_USING_LWIP)
static int hook_enet(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    struct netif *nif;

    if (write) {
        return -RT_ERROR;
    }
    nif = h7_netif();
    if (nif == RT_NULL) {
        return -RT_ERROR;
    }
    if (area == MB_AREA_DIN) {                          /* LINK_UP */
        *(rt_uint8_t *)val = netif_is_link_up(nif) ? 1 : 0;
        return 0;
    }
    if (area == MB_AREA_IREG) {                         /* IP u32 @ 0x0C00/0x0C01 */
        rt_uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(nif));
        *(rt_uint16_t *)val = (addr == 0x0C00)
            ? (rt_uint16_t)(ip >> 16) : (rt_uint16_t)(ip & 0xFFFFu);
        return 0;
    }
    return -RT_ERROR;
}

/* 未启用 netdev（无 ifconfig）时的替代：显示 e0 的 IP/掩码/网关/链路 */
static void net_info(int argc, char **argv)
{
    struct netif *nif = h7_netif();
    char ip[16], gw[16], mask[16];

    (void)argc; (void)argv;
    if (nif == RT_NULL) {
        rt_kprintf("e0 netif not ready (ENET not up?)\n");
        return;
    }
    /* 注意：ip4addr_ntoa() 用同一静态缓冲，不能一条 printf 里调多次 */
    ip4addr_ntoa_r(netif_ip4_addr(nif), ip, sizeof(ip));
    ip4addr_ntoa_r(netif_ip4_gw(nif), gw, sizeof(gw));
    ip4addr_ntoa_r(netif_ip4_netmask(nif), mask, sizeof(mask));

    rt_kprintf("e0: ip=%s gw=%s mask=%s link=%s %s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               ip, gw, mask,
               netif_is_link_up(nif) ? "up" : "down",
               netif_is_up(nif) ? "if-up" : "if-down",
               nif->hwaddr[0], nif->hwaddr[1], nif->hwaddr[2],
               nif->hwaddr[3], nif->hwaddr[4], nif->hwaddr[5]);
    if (ip4_addr_isany(netif_ip4_addr(nif))) {
        rt_kprintf("  IP=0.0.0.0: DHCP 未拿到租约，可用 net_set 或 ifconfig 配静态 IP\n");
    }
}
MSH_CMD_EXPORT(net_info, print e0 ip/gw/mask/link);

static void net_set(int argc, char **argv)
{
    struct netif *nif = h7_netif();
    ip4_addr_t ip, mask, gw;
    char s_ip[16], s_mask[16], s_gw[16];

    if (nif == RT_NULL) {
        rt_kprintf("e0 netif not ready\n");
        return;
    }
    if (argc != 4) {
        rt_kprintf("usage: net_set <ip> <mask> <gw>\n");
        return;
    }
    if (!ip4addr_aton(argv[1], &ip) || !ip4addr_aton(argv[2], &mask) ||
        !ip4addr_aton(argv[3], &gw)) {
        rt_kprintf("bad ip/mask/gw\n");
        return;
    }
#if LWIP_DHCP
    dhcp_stop(nif);
#endif
    netif_set_addr(nif, &ip, &mask, &gw);
    netif_set_up(nif);

    ip4addr_ntoa_r(netif_ip4_addr(nif), s_ip, sizeof(s_ip));
    ip4addr_ntoa_r(netif_ip4_netmask(nif), s_mask, sizeof(s_mask));
    ip4addr_ntoa_r(netif_ip4_gw(nif), s_gw, sizeof(s_gw));
    rt_kprintf("e0 set: ip=%s mask=%s gw=%s\n", s_ip, s_mask, s_gw);
}
MSH_CMD_EXPORT(net_set, net_set <ip> <mask> <gw> - static IP (stops DHCP));
#endif

/* ================================================================== */
/* 0x0Dxx 闭环控制回路（DAC<->ADC PID）                                 */
/* ================================================================== */
#if defined(BSP_USING_ADC) && defined(RT_USING_ADC) && \
    defined(BSP_USING_DAC) && defined(RT_USING_DAC)
#define LOOP_OUT_MAX 4095

static int hook_loop(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
{
    rt_uint16_t v;

    if (area == MB_AREA_COIL && addr == 0x0D00) {          /* LOOP_ENABLE */
        if (write) {
            g_loop.enabled = (*(rt_uint8_t *)val) ? RT_TRUE : RT_FALSE;
            app_loop_reset();
        } else {
            *(rt_uint8_t *)val = g_loop.enabled ? 1 : 0;
        }
        return 0;
    }

    if (area == MB_AREA_IREG) {
        if (write) {
            return -RT_ERROR;
        }
        switch (addr) {
        case 0x0D00: v = (rt_uint16_t)(g_loop.enabled ? 1 : 0); break;
        case 0x0D01: v = g_loop.setpoint;                       break;
        case 0x0D02: v = g_loop.feedback;                       break;
        case 0x0D03: v = g_loop.output;                         break;
        case 0x0D04: v = (rt_uint16_t)g_loop.error;             break;
        case 0x0D05: v = g_loop.alarm;                          break;
        case 0x0D06: v = (rt_uint16_t)g_loop.scan_us;           break;
        case 0x0D07: v = (rt_uint16_t)g_loop.jitter_us;         break;
        case 0x0D08: v = (rt_uint16_t)(g_loop.iters >> 16);     break;
        case 0x0D09: v = (rt_uint16_t)(g_loop.iters & 0xFFFFu); break;
        default: return -RT_ERROR;
        }
        *(rt_uint16_t *)val = v;
        return 0;
    }

    if (area == MB_AREA_HREG) {
        v = *(rt_uint16_t *)val;
        if (!write) {
            switch (addr) {
            case 0x0D00: v = g_loop.setpoint;          break;
            case 0x0D01: v = (rt_uint16_t)g_loop.kp;   break;
            case 0x0D02: v = (rt_uint16_t)g_loop.ki;   break;
            case 0x0D03: v = (rt_uint16_t)g_loop.kd;   break;
            case 0x0D04: v = g_loop.period_ms;         break;
            case 0x0D05: v = g_loop.safe_output;       break;
            case 0x0D06: v = g_loop.fb_lo;             break;
            case 0x0D07: v = g_loop.fb_hi;             break;
            case 0x0D08: v = 0;                        break;   /* RESET 只写 */
            default: return -RT_ERROR;
            }
            *(rt_uint16_t *)val = v;
            return 0;
        }
        switch (addr) {
        case 0x0D00: g_loop.setpoint    = (v <= LOOP_OUT_MAX) ? v : LOOP_OUT_MAX; break;
        case 0x0D01: g_loop.kp          = (rt_int16_t)v; break;
        case 0x0D02: g_loop.ki          = (rt_int16_t)v; break;
        case 0x0D03: g_loop.kd          = (rt_int16_t)v; break;
        case 0x0D04: g_loop.period_ms   = v ? v : 1;     break;
        case 0x0D05: g_loop.safe_output = (v <= LOOP_OUT_MAX) ? v : LOOP_OUT_MAX; break;
        case 0x0D06: g_loop.fb_lo       = v; break;
        case 0x0D07: g_loop.fb_hi       = v; break;
        case 0x0D08: app_loop_reset();   break;
        default: return -RT_ERROR;
        }
        return 0;
    }
    return -RT_ERROR;
}
#endif

/* ================================================================== */
/* 点表                                                                 */
/* ================================================================== */
static const mb_point_t mb_points[] = {
    /* ---- 0x00xx 系统 (3x 只读) ---- */
    MB_PT(MB_AREA_IREG, 0x0000, MB_T_U16, MB_A_RO, &sys_model, RT_NULL, "SYS_MODEL"),
    MB_PT(MB_AREA_IREG, 0x0001, MB_T_U16, MB_A_RO, &sys_fw,    RT_NULL, "SYS_FW_VER"),
    MB_PT(MB_AREA_IREG, 0x0002, MB_T_U32, MB_A_RO, RT_NULL, hook_uptime, "SYS_UPTIME_S"),

    /* ---- 0x01xx LED / 按键 ---- */
    MB_PT(MB_AREA_COIL, 0x0000, MB_T_BIT, MB_A_RW, RT_NULL, hook_led, "LED1"),
    MB_PT(MB_AREA_COIL, 0x0001, MB_T_BIT, MB_A_RW, RT_NULL, hook_led, "LED2"),
    MB_PT(MB_AREA_DIN,  0x0000, MB_T_BIT, MB_A_RO, RT_NULL, hook_key, "KEY_WAKEUP"),
    MB_PT(MB_AREA_DIN,  0x0001, MB_T_BIT, MB_A_RO, RT_NULL, hook_key, "KEY_TAMPER"),
    MB_PT(MB_AREA_DIN,  0x0002, MB_T_BIT, MB_A_RO, RT_NULL, hook_key, "KEY_USER"),

#if defined(BSP_USING_ADC) && defined(RT_USING_ADC)
    /* ---- 0x02xx ADC 采样 (3x 只读) ---- */
    MB_PT(MB_AREA_IREG, 0x0200, MB_T_U16, MB_A_RO, RT_NULL, hook_adc, "ADC_CH0"),
    MB_PT(MB_AREA_IREG, 0x0201, MB_T_U16, MB_A_RO, RT_NULL, hook_adc, "ADC_CH1"),
    MB_PT(MB_AREA_IREG, 0x0202, MB_T_U16, MB_A_RO, RT_NULL, hook_adc, "ADC_CH2"),
    MB_PT(MB_AREA_IREG, 0x0203, MB_T_U16, MB_A_RO, RT_NULL, hook_adc, "ADC_CH3"),
#endif

#if defined(BSP_USING_DAC) && defined(RT_USING_DAC)
    /* ---- 0x03xx DAC ---- */
    MB_PT(MB_AREA_HREG, 0x0300, MB_T_U16, MB_A_RW, RT_NULL, hook_dac, "DAC_CH1_SET"),
    MB_PT(MB_AREA_IREG, 0x0300, MB_T_U16, MB_A_RO, RT_NULL, hook_dac, "DAC_CH1_VAL"),
#endif

#if defined(BSP_USING_RTC) && defined(RT_USING_RTC)
    /* ---- 0x04xx RTC (3x 读 / 4x 写) ---- */
    MB_PT(MB_AREA_IREG, 0x0400, MB_T_U16, MB_A_RO, RT_NULL, hook_rtc, "RTC_YEAR"),
    MB_PT(MB_AREA_IREG, 0x0401, MB_T_U16, MB_A_RO, RT_NULL, hook_rtc, "RTC_MON"),
    MB_PT(MB_AREA_IREG, 0x0402, MB_T_U16, MB_A_RO, RT_NULL, hook_rtc, "RTC_DAY"),
    MB_PT(MB_AREA_IREG, 0x0403, MB_T_U16, MB_A_RO, RT_NULL, hook_rtc, "RTC_HOUR"),
    MB_PT(MB_AREA_IREG, 0x0404, MB_T_U16, MB_A_RO, RT_NULL, hook_rtc, "RTC_MIN"),
    MB_PT(MB_AREA_IREG, 0x0405, MB_T_U16, MB_A_RO, RT_NULL, hook_rtc, "RTC_SEC"),
    MB_PT(MB_AREA_HREG, 0x0400, MB_T_U16, MB_A_RW, RT_NULL, hook_rtc, "RTC_YEAR_SET"),
    MB_PT(MB_AREA_HREG, 0x0401, MB_T_U16, MB_A_RW, RT_NULL, hook_rtc, "RTC_MON_SET"),
    MB_PT(MB_AREA_HREG, 0x0402, MB_T_U16, MB_A_RW, RT_NULL, hook_rtc, "RTC_DAY_SET"),
    MB_PT(MB_AREA_HREG, 0x0403, MB_T_U16, MB_A_RW, RT_NULL, hook_rtc, "RTC_HOUR_SET"),
    MB_PT(MB_AREA_HREG, 0x0404, MB_T_U16, MB_A_RW, RT_NULL, hook_rtc, "RTC_MIN_SET"),
    MB_PT(MB_AREA_HREG, 0x0405, MB_T_U16, MB_A_RW, RT_NULL, hook_rtc, "RTC_SEC_SET"),
#endif

#if defined(BSP_USING_I2C1)
    /* ---- 0x05xx EEPROM ---- */
    MB_PT(MB_AREA_HREG, 0x0500, MB_T_U16, MB_A_RW, RT_NULL, hook_eep, "EEP_ADDR"),
    MB_PT(MB_AREA_HREG, 0x0501, MB_T_U16, MB_A_WO, RT_NULL, hook_eep, "EEP_WDATA"),
    MB_PT(MB_AREA_IREG, 0x0500, MB_T_U16, MB_A_RO, RT_NULL, hook_eep, "EEP_RDATA"),
#endif

#if defined(RT_USING_FAL)
    /* ---- 0x06xx OSPI Flash param 分区 ---- */
    MB_PT(MB_AREA_HREG, 0x0600, MB_T_U32, MB_A_RW, RT_NULL, hook_flash, "FLASH_PARAM"),
    MB_PT(MB_AREA_IREG, 0x0600, MB_T_U16, MB_A_RO, RT_NULL, hook_flash, "FLASH_MAGIC_OK"),
#endif

#if defined(BSP_USING_SDIO)
    /* ---- 0x07xx SD 卡 ---- */
    MB_PT(MB_AREA_DIN,  0x0700, MB_T_BIT, MB_A_RO, RT_NULL, hook_sd, "SD_MOUNTED"),
#endif

#if defined(RT_USING_DFS)
    /* ---- 0x07xx 数据记录 ---- */
    MB_PT(MB_AREA_COIL, 0x0700, MB_T_BIT, MB_A_RW, RT_NULL, hook_log, "LOG_ENABLE"),
    MB_PT(MB_AREA_HREG, 0x0700, MB_T_U16, MB_A_RW, RT_NULL, hook_log, "LOG_PERIOD_MS"),
    MB_PT(MB_AREA_IREG, 0x0700, MB_T_U32, MB_A_RO, RT_NULL, hook_log, "LOG_LINES"),
#endif

#if defined(BSP_USING_CAN) && defined(RT_USING_CAN)
    /* ---- 0x08xx CAN ---- */
    MB_PT(MB_AREA_IREG, 0x0800, MB_T_U32, MB_A_RO, RT_NULL, hook_can, "CAN_RX_CNT"),
    MB_PT(MB_AREA_IREG, 0x0802, MB_T_U16, MB_A_RO, RT_NULL, hook_can, "CAN_LAST_ID"),
    MB_PT(MB_AREA_IREG, 0x0803, MB_T_U16, MB_A_RO, RT_NULL, hook_can, "CAN_LAST_DLC"),
    MB_PT(MB_AREA_HREG, 0x0800, MB_T_U16, MB_A_RW, RT_NULL, hook_can, "CAN_TX_ID"),
    MB_PT(MB_AREA_HREG, 0x0801, MB_T_U16, MB_A_RW, RT_NULL, hook_can, "CAN_TX_DLC"),
    MB_PT(MB_AREA_HREG, 0x0802, MB_T_U16, MB_A_RW, RT_NULL, hook_can, "CAN_TX_DATA0"),
    MB_PT(MB_AREA_HREG, 0x0803, MB_T_U16, MB_A_RW, RT_NULL, hook_can, "CAN_TX_DATA1"),
    MB_PT(MB_AREA_HREG, 0x0804, MB_T_U16, MB_A_RW, RT_NULL, hook_can, "CAN_TX_DATA2"),
    MB_PT(MB_AREA_HREG, 0x0805, MB_T_U16, MB_A_RW, RT_NULL, hook_can, "CAN_TX_DATA3"),
    MB_PT(MB_AREA_HREG, 0x0806, MB_T_U16, MB_A_WO, RT_NULL, hook_can, "CAN_TX_TRIG"),
#endif

#if defined(BSP_USING_TOUCH)
    /* ---- 0x09xx 触摸 ---- */
    MB_PT(MB_AREA_DIN,  0x0900, MB_T_BIT, MB_A_RO, RT_NULL, hook_touch, "TOUCH_PRESSED"),
    MB_PT(MB_AREA_IREG, 0x0900, MB_T_U16, MB_A_RO, RT_NULL, hook_touch, "TOUCH_X"),
    MB_PT(MB_AREA_IREG, 0x0901, MB_T_U16, MB_A_RO, RT_NULL, hook_touch, "TOUCH_Y"),
#endif

#if defined(BSP_USING_HPDF)
    /* ---- 0x0Axx 音频 ---- */
    MB_PT(MB_AREA_IREG, 0x0A00, MB_T_U16, MB_A_RO, RT_NULL, hook_audio, "MIC_LEFT"),
    MB_PT(MB_AREA_IREG, 0x0A01, MB_T_U16, MB_A_RO, RT_NULL, hook_audio, "MIC_RIGHT"),
#endif

#if defined(BSP_USING_USB_CDC)
    /* ---- 0x0Bxx USB ---- */
    MB_PT(MB_AREA_DIN,  0x0B00, MB_T_BIT, MB_A_RO, RT_NULL, hook_usb, "USB_CDC_READY"),
#endif

#if defined(RT_USING_LWIP)
    /* ---- 0x0Cxx ENET ---- */
    MB_PT(MB_AREA_DIN,  0x0C00, MB_T_BIT, MB_A_RO, RT_NULL, hook_enet, "ENET_LINK_UP"),
    MB_PT(MB_AREA_IREG, 0x0C00, MB_T_U32, MB_A_RO, RT_NULL, hook_enet, "ENET_IP"),
#endif

#if defined(BSP_USING_ADC) && defined(RT_USING_ADC) && \
    defined(BSP_USING_DAC) && defined(RT_USING_DAC)
    /* ---- 0x0Dxx 闭环控制回路（DAC<->ADC PID） ---- */
    MB_PT(MB_AREA_COIL, 0x0D00, MB_T_BIT, MB_A_RW, RT_NULL, hook_loop, "LOOP_ENABLE"),
    MB_PT(MB_AREA_HREG, 0x0D00, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_SETPOINT"),
    MB_PT(MB_AREA_HREG, 0x0D01, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_KP"),
    MB_PT(MB_AREA_HREG, 0x0D02, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_KI"),
    MB_PT(MB_AREA_HREG, 0x0D03, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_KD"),
    MB_PT(MB_AREA_HREG, 0x0D04, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_PERIOD_MS"),
    MB_PT(MB_AREA_HREG, 0x0D05, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_SAFE_OUT"),
    MB_PT(MB_AREA_HREG, 0x0D06, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_FB_LO"),
    MB_PT(MB_AREA_HREG, 0x0D07, MB_T_U16, MB_A_RW, RT_NULL, hook_loop, "LOOP_FB_HI"),
    MB_PT(MB_AREA_HREG, 0x0D08, MB_T_U16, MB_A_WO, RT_NULL, hook_loop, "LOOP_RESET"),
    MB_PT(MB_AREA_IREG, 0x0D00, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_EN"),
    MB_PT(MB_AREA_IREG, 0x0D01, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_SP"),
    MB_PT(MB_AREA_IREG, 0x0D02, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_FEEDBACK"),
    MB_PT(MB_AREA_IREG, 0x0D03, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_OUTPUT"),
    MB_PT(MB_AREA_IREG, 0x0D04, MB_T_S16, MB_A_RO, RT_NULL, hook_loop, "LOOP_ERROR"),
    MB_PT(MB_AREA_IREG, 0x0D05, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_ALARM"),
    MB_PT(MB_AREA_IREG, 0x0D06, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_SCAN_US"),
    MB_PT(MB_AREA_IREG, 0x0D07, MB_T_U16, MB_A_RO, RT_NULL, hook_loop, "LOOP_JITTER_US"),
    MB_PT(MB_AREA_IREG, 0x0D08, MB_T_U32, MB_A_RO, RT_NULL, hook_loop, "LOOP_ITERS"),
#endif
};

rt_size_t mb_points_register(void)
{
    /* GPIO 方向初始化 */
    rt_pin_mode(LED1_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(LED2_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(KEY_WAKEUP_PIN, PIN_MODE_INPUT);
    rt_pin_mode(KEY_TAMPER_PIN, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(KEY_USER_PIN, PIN_MODE_INPUT_PULLUP);

#if defined(BSP_USING_ADC) && defined(RT_USING_ADC)
    adc_dev = (rt_adc_device_t)rt_device_find("adc2");
    if (adc_dev != RT_NULL) {
        int ch;
        for (ch = 0; ch < 4; ch++) {
            rt_adc_enable(adc_dev, ch);
        }
    } else {
        LOG_W("adc2 not found");
    }
#endif

#if defined(BSP_USING_DAC) && defined(RT_USING_DAC)
    dac_dev = (rt_dac_device_t)rt_device_find("dac");
    if (dac_dev != RT_NULL) {
        rt_dac_enable(dac_dev, DAC_CH);
    } else {
        LOG_W("dac not found");
    }
#endif

#if defined(BSP_USING_RTC) && defined(RT_USING_RTC)
    rtc_dev = rt_device_find("rtc");
    if (rtc_dev == RT_NULL) {
        LOG_W("rtc not found");
    }
#endif

#if defined(BSP_USING_CAN) && defined(RT_USING_CAN)
    if (app_can_init() != RT_EOK) {
        LOG_W("can service init failed");
    }
#endif

#if defined(BSP_USING_ADC) && defined(RT_USING_ADC) && \
    defined(BSP_USING_DAC) && defined(RT_USING_DAC)
    if (app_loop_init() != RT_EOK) {
        LOG_W("control loop init failed");
    }
#endif

    mb_pointtable_init(mb_points, sizeof(mb_points) / sizeof(mb_points[0]));
    LOG_I("point table: %u points", (unsigned)(sizeof(mb_points) / sizeof(mb_points[0])));
    return sizeof(mb_points) / sizeof(mb_points[0]);
}
