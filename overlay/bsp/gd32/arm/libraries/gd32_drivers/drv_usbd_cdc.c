/*
 * drv_usbd_cdc.c - GD32H759 USBHSx CDC ACM 虚拟串口（CherryUSB 板级胶水）
 *
 * 职责边界：
 *   CherryUSB 组件提供：dwc2 控制器驱动 + CDC ACM class +
 *                        cdc_acm_rttchardev 模板（描述符 + 初始化 +
 *                        rt_device 字符设备 "usb-acm0" +
 *                        cdc_acm_enter/exit 控制台切换命令）
 *   本文件提供：① 板级时钟/中断（照官方 27_USB_Device/CDC_ACM 的 FS 流程）
 *              ② DWC2 用户参数（GD 硬件参数寄存器读 0，手工写死）
 *              ③ 初始化入口（一行调模板）
 *   不修改第三方源码（两个硬件级补丁除外，见开发笔记附录 A 台账）。
 *
 * 控制器切换：
 *   只改下面 USB_CDC_USE_HS1 一处：0 = USBHS0/CN2（JP66 跳线），
 *   1 = USBHS1/CN3（JP68 跳线）。时钟、中断、寄存器基址全部联动。
 *   注意：busid 恒为 0——CherryUSB 设备栈只支持单总线
 *         （官方模板注释：bus num must be 1 except hpm ip），
 *         busid 传 1 会报 "bus overflow"。用哪个控制器由 reg_base 决定，
 *         与 busid 无关。CONFIG_USBDEV_MAX_BUS 保持 1 不要动。
 *
 * 硬件事实（评估板指南 §4.20 + datasheet）：
 *   - USBHS0 @ 0x40040000 / USBHS1 @ 0x40080000（datasheet 存储映射表），DP/DM 专用引脚
 *   - 时钟链：PMU 开 USB LDO → USB48M 选 IRC48M（PLL0R 在 600M 预设下是 300M，不可用！）
 *     → 置 USBHSxSWEN 使软件选择生效 → 开对应 USBHSx 外设钟
 *
 * 用法（msh）：
 *   vcom_echo       —— PC 串口助手发什么回什么，收发验证
 *   cdc_acm_enter   —— 把 msh 控制台切到 USB 虚拟串口（模板自带）
 *   cdc_acm_exit    —— 切回串口控制台（模板自带）
 *   usb_dbg         —— 寄存器快照（枚举排障取证工具）
 */

#include <rtthread.h>
#include <board.h>
#include "gd32h7xx.h"

#ifdef BSP_USING_USB_CDC

#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "../../../../../rt-thread/components/drivers/usb/cherryusb/port/dwc2/usb_dwc2_param.h"

/* ===================== 控制器选择开关（唯一需要改的地方） ===================== */
#define USB_CDC_USE_HS1     1   /* 0 = USBHS0/CN2, 1 = USBHS1/CN3 */

#define USB_BUS_ID          0   /* CherryUSB 设备栈单总线，恒为 0 */

#if USB_CDC_USE_HS1
    #define USBHS_REG_BASE      0x40080000UL  /* datasheet Rev2.3 §存储映射: USBHS1 @ 0x40080000-0x400BFFFF */
    #define USBHS_RCU           RCU_USBHS1
    #define USBHS_CLK_IDX       IDX_USBHS1
    #define USBHS_IRQN          USBHS1_IRQn
#else
    #define USBHS_REG_BASE      0x40040000UL
    #define USBHS_RCU           RCU_USBHS0
    #define USBHS_CLK_IDX       IDX_USBHS0
    #define USBHS_IRQN          USBHS0_IRQn
#endif

/* ===================== ① 板级时钟与中断 ===================== */

static volatile uint32_t g_usb_irq_count;
static volatile uint32_t g_gintsts_seen;

void usb_dc_low_level_init(uint8_t busid)
{
    (void)busid;

    /* USB 内部 LDO 上电并等 3.3V 就绪 */
    rcu_periph_clock_enable(RCU_PMU);
    pmu_usb_regulator_enable();
    pmu_usb_voltage_detector_enable();
    while (SET != pmu_flag_get(PMU_FLAG_USB33RF)) {
    }

    /* USB48M 时钟：选 IRC48M。
       注意！不能选 PLL0R——BSP 600M 时钟预设下 PLL0R = 600/2 = 300MHz
       （官方 demo 是为了凑 PLL0R=48M 才把 CPU 降到 480M 跑的，
       见官方 system_gd32h7xx.c 的 __SYSTEM_CLOCK_480M_PLL0_HXTAL: PLL0R=÷10）。
       IRC48M 是 GD 专为 USB 设计的振荡器（AN117：FS 精度够用）。
       且软件选择必须置 USBHSxSWEN 才生效（AN117 §2.1.1），
       GD SDK 的 rcu_usb48m_clock_config() 不置该位，需手动补。 */
    rcu_osci_on(RCU_IRC48M);
    while (SUCCESS != rcu_osci_stab_wait(RCU_IRC48M)) {
    }
    rcu_usb48m_clock_config(USBHS_CLK_IDX, RCU_USB48MSRC_IRC48M);
#if USB_CDC_USE_HS1
    RCU_USBCLKCTL |= RCU_USBCLKCTL_USBHS1SWEN;   /* bit12：软件选择生效 */
#else
    RCU_USBCLKCTL |= RCU_USBCLKCTL_USBHS0SWEN;   /* bit4 */
#endif

    /* USBHSx 外设时钟（内嵌 FS PHY，无 ULPI） */
    rcu_periph_clock_enable(USBHS_RCU);

    nvic_irq_enable(USBHS_IRQN, 5, 0);
}

void usb_dc_low_level_deinit(uint8_t busid)
{
    (void)busid;
    nvic_irq_disable(USBHS_IRQN);
    rcu_periph_clock_disable(USBHS_RCU);
}

#if USB_CDC_USE_HS1
void USBHS1_IRQHandler(void)
{
    extern void USBD_IRQHandler(uint8_t busid);
    g_usb_irq_count++;
    g_gintsts_seen |= *(volatile uint32_t *)(USBHS_REG_BASE + 0x14);  /* GINTSTS */
    USBD_IRQHandler(USB_BUS_ID);
}
#else
void USBHS0_IRQHandler(void)
{
    extern void USBD_IRQHandler(uint8_t busid);
    g_usb_irq_count++;
    g_gintsts_seen |= *(volatile uint32_t *)(USBHS_REG_BASE + 0x14);  /* GINTSTS */
    USBD_IRQHandler(USB_BUS_ID);
}
#endif

/* ===================== ② DWC2 用户参数（GD 硬件参数寄存器读 0，写死） =====================
 *
 * FIFO 预算：GD32H759 USBHS 共 1024 字（32bit）FIFO RAM
 * （官方 usb_conf.h：RX 512 + TX0 128 + TX1 384 = 1024）。
 * CDC ACM 用 EP0 + EP1(BULK IN) + EP2(BULK OUT) + EP3(INT)，分配：
 *   RX 512 / TX0 128 / TX1 192 / TX2~TX7 各 16
 *   合计 512+128+192+16*6 = 928 ≤ 1024
 */
void dwc2_get_user_params(uint32_t reg_base, struct dwc2_user_params *params)
{
    (void)reg_base;

    params->phy_type = 0;               /* DWC2_PHY_TYPE_PARAM_FS：内嵌全速 PHY */
    params->phy_utmi_width = 8;

    params->device_dma_enable = false;  /* GD 报架构 0 = slave only，不能用 DMA */
    params->device_dma_desc_enable = false;

    params->device_rx_fifo_size = 512;
    params->device_tx_fifo_size[0] = 128;
    params->device_tx_fifo_size[1] = 192;
    for (int i = 2; i < 8; i++) {
        params->device_tx_fifo_size[i] = 16;
    }

    /* GD32H7 官方 GCCFG 位定义（drv_usb_regs.h）：bit21=VDEN(VBUS检测使能)、
       bit20=SOFOEN、bit16=PWRON。它没有 STM32 的 VBUSACEN/VBUSBCEN/VBUSIG！
       官方 FS PHY 序列只置 PWRON；"忽略 VBUS" 在这颗芯片上靠 GOTGCTL 的
       BVALOEN/BVALOVAL 覆写位实现（官方 drv_usb_core.c：GOTGCS|=BVOV|BVOE），
       对应下面 b_session_valid_override = true。
       曾按 STM32 布局误写 (1<<21) 把 VDEN 打开，而 VBUS 引脚被 JP68/JP70 跳线
       断开，B-session 无效导致内核拒收 setup 包——枚举卡死的根因。 */
    params->device_gccfg = (1U << 16);              /* 仅 PWRON */

    params->b_session_valid_override = true;        /* = 官方 GOTGCS BVOV|BVOE */
    params->total_fifo_size = 1024;     /* 单位：32bit 字 */
}

void dwc2_get_user_fifo_config(uint32_t reg_base, struct usb_dwc2_user_fifo_config *config)
{
    (void)reg_base;
    config->device_rx_fifo_size = 512;
    config->device_tx_fifo_size[0] = 128;
    config->device_tx_fifo_size[1] = 192;
    for (int i = 2; i < 8; i++) {
        config->device_tx_fifo_size[i] = 16;
    }
}

/* ===================== ③ 初始化入口 ===================== */

static int usb_cdc_init(void)
{
    /* cdc_acm_chardev_init 由 CherryUSB 的 cdc_acm_rttchardev 模板提供：
       描述符注册 + CDC 接口/端点 + 字符设备 "usb-acm0" 一步全包 */
    extern void cdc_acm_chardev_init(uint8_t busid, uintptr_t reg_base);
    cdc_acm_chardev_init(USB_BUS_ID, USBHS_REG_BASE);

    rt_kprintf("[usb] CDC ACM on USBHS%d (FS), chardev: usb-acm0\n", USB_CDC_USE_HS1);
    return 0;
}
INIT_APP_EXPORT(usb_cdc_init);

/* ===================== ④ msh 测试命令 ===================== */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void vcom_echo(int argc, char **argv)
{
    rt_device_t vcom = rt_device_find("usb-acm0");
    if (vcom == RT_NULL) {
        rt_kprintf("usb-acm0 not found, check list_device\n");
        return;
    }
    if (rt_device_open(vcom, RT_DEVICE_FLAG_RDWR) != RT_EOK) {
        rt_kprintf("open failed: USB not configured? (plug the cable first)\n");
        return;
    }

    rt_kprintf("vcom echo running, send bytes from PC (auto exit after 30s idle)\n");

    rt_uint8_t buf[64];
    rt_tick_t deadline = rt_tick_get() + rt_tick_from_millisecond(30000);

    while (rt_tick_get() < deadline) {
        rt_ssize_t n = rt_device_read(vcom, 0, buf, sizeof(buf));
        if (n > 0) {
            rt_device_write(vcom, 0, buf, n);
            rt_kprintf("echo %d bytes\n", n);
            deadline = rt_tick_get() + rt_tick_from_millisecond(30000);
        }
        rt_thread_mdelay(10);
    }
    rt_device_close(vcom);
    rt_kprintf("vcom echo exit\n");
}
MSH_CMD_EXPORT(vcom_echo, USB CDC ACM loopback test);

/* USB 寄存器快照：枚举卡死时的取证工具。
   注意：base 是 uint32_t*，索引必须除以 4（base[0x818/4]），
   严禁 base + 0x818（指针运算按元素步进，会乘 4）！ */
static void usb_dbg(int argc, char **argv)
{
    volatile uint32_t *base = (volatile uint32_t *)USBHS_REG_BASE;
    uint32_t irqn = USBHS_IRQN;

    rt_kprintf("GOTGCTL(0x00) = %08lx\r\n", base[0x00 / 4]);
    rt_kprintf("GAHBCFG(0x08) = %08lx\r\n", base[0x08 / 4]);
    rt_kprintf("GUSBCFG(0x0C) = %08lx\r\n", base[0x0C / 4]);
    rt_kprintf("GINTSTS(0x14) = %08lx\r\n", base[0x14 / 4]);
    rt_kprintf("GINTMSK(0x18) = %08lx\r\n", base[0x18 / 4]);
    rt_kprintf("GCCFG  (0x38) = %08lx\r\n", base[0x38 / 4]);
    rt_kprintf("DCFG   (0x800)= %08lx\r\n", base[0x800 / 4]);
    rt_kprintf("DCTL   (0x804)= %08lx\r\n", base[0x804 / 4]);
    rt_kprintf("DSTS   (0x808)= %08lx\r\n", base[0x808 / 4]);

    rt_kprintf("irq_count   = %u\r\n", g_usb_irq_count);
    rt_kprintf("NVIC ISER%d  = %08lx (%s)\r\n", irqn >> 5, NVIC->ISER[irqn >> 5],
               (NVIC->ISER[irqn >> 5] >> (irqn & 31)) & 1 ? "ENABLED" : "disabled!");
    rt_kprintf("NVIC prio   = %lu\r\n", NVIC->IP[irqn]);
    rt_kprintf("DAINT(0x818)   = %08lx\r\n", base[0x818 / 4]);  /* 端点中断挂起 */
    rt_kprintf("DAINTMSK(0x81C)= %08lx\r\n", base[0x81C / 4]);  /* 端点中断掩码 */
    rt_kprintf("DIEPMSK(0x810) = %08lx\r\n", base[0x810 / 4]);
    rt_kprintf("DOEPMSK(0x814) = %08lx\r\n", base[0x814 / 4]);
    rt_kprintf("DIEPCTL0(0x900)= %08lx\r\n", base[0x900 / 4]);  /* EP0 IN */
    rt_kprintf("DIEPTSIZ0(0x910)= %08lx\r\n", base[0x910 / 4]);
    rt_kprintf("DOEPCTL0(0xB00)= %08lx\r\n", base[0xB00 / 4]);  /* EP0 OUT */
    rt_kprintf("DOEPTSIZ0(0xB10)= %08lx\r\n", base[0xB10 / 4]); /* 高2位=STUPCNT */
    rt_kprintf("GINTSTS_seen= %08lx\r\n", g_gintsts_seen);
    g_gintsts_seen = 0;
}
MSH_CMD_EXPORT(usb_dbg, dump USBHS registers);

#endif /* RT_USING_FINSH */

#endif /* BSP_USING_USB_CDC */