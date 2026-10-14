/*
 * usb_config.h - CherryUSB 完整配置（GD32H759I-EVAL / USBHS0 / FS / CDC ACM）
 *
 * 基于 CherryUSB v1.5.2 官方 cherryusb_config_template.h 裁剪，
 * 放 board/ 目录（须在头文件搜索路径内）。
 */

#ifndef USB_CONFIG_H
#define USB_CONFIG_H

#include "rtthread.h"

/* ================= 调试 ================= */
#define CONFIG_USB_PRINTF(...) rt_kprintf(__VA_ARGS__)

#ifndef CONFIG_USB_DBG_LEVEL
#define CONFIG_USB_DBG_LEVEL USB_INFO_LOG
#endif


/* Enable assert with default printf */
// #define CONFIG_USB_ASSERT_ENABLE

/* ================= 内存属性 ================= */
/* M7 带 cache；dwc2 在 GD 上只能 slave 模式（无 DMA），不强制 nocache 段，
   定义为空宏即可（将来开 HS/DMA 时再挂 .noncacheable） */
#define USB_NOCACHE_RAM_SECTION

#ifndef CONFIG_USB_ALIGN_SIZE
#define CONFIG_USB_ALIGN_SIZE 4
#endif

/* attribute data into no cache ram */
#define USB_NOCACHE_RAM_SECTION_IMPL

/* ================= OSAL（RT-Thread） ================= */
#ifndef CONFIG_USB_OSAL_PRIO
#define CONFIG_USB_OSAL_PRIO 5
#endif

#ifndef CONFIG_USB_OSAL_STACKSIZE
#define CONFIG_USB_OSAL_STACKSIZE 2048
#endif

/* ================= Device 模式 ================= */
#define CONFIG_USBDEV_MAX_BUS 1

/* FS 内嵌 PHY（将来用内嵌 HS PHY 再开 CONFIG_USB_HS） */
/* #define CONFIG_USB_HS */

/* GD32H759 USBHS 设备端点数 = 6（EP0~EP5）；
   usb_glue_gd.c 只接受 4 或 6，HS 口就是 6 */
#define CONFIG_USBDEV_EP_NUM 6

#define CONFIG_USBDEV_EP0_MAXPACKETSIZE 64

#ifndef CONFIG_USBDEV_REQUEST_BUFFER_LEN
#define CONFIG_USBDEV_REQUEST_BUFFER_LEN 512
#endif

/* 自检框架需要 */
#define CONFIG_USBDEV_ADVANCE_DESC

#ifndef CONFIG_USBDEV_DESC_CHECK
#define CONFIG_USBDEV_DESC_CHECK
#endif

#ifndef CONFIG_USBDEV_TEST_MODE
// #define CONFIG_USBDEV_TEST_MODE
#endif

/* dwc2 FS 设备 FIFO 访问宽度 */
#define CONFIG_USBDEV_FSDEV_PBUF_ACCESS 2

/* ---- CDC ACM ---- */
/* （rt-thread 内置版由 Kconfig 控制 class 编译，此处无需宏） */

/* ---- MSC（本篇不用，字段给默认值防止编译期检查报错） ---- */
#ifndef CONFIG_USBDEV_MSC_MAX_LUN
#define CONFIG_USBDEV_MSC_MAX_LUN 1
#endif

#ifndef CONFIG_USBDEV_MSC_MAX_BUFSIZE
#define CONFIG_USBDEV_MSC_MAX_BUFSIZE 512
#endif

#ifndef CONFIG_USBDEV_MSC_MANUFACTURER_STRING
#define CONFIG_USBDEV_MSC_MANUFACTURER_STRING ""
#endif

#ifndef CONFIG_USBDEV_MSC_PRODUCT_STRING
#define CONFIG_USBDEV_MSC_PRODUCT_STRING ""
#endif

#ifndef CONFIG_USBDEV_MSC_VERSION_STRING
#define CONFIG_USBDEV_MSC_VERSION_STRING "0.01"
#endif

// #define CONFIG_USBDEV_MSC_POLLING
// #define CONFIG_USBDEV_MSC_THREAD

#ifndef CONFIG_USBDEV_MSC_PRIO
#define CONFIG_USBDEV_MSC_PRIO 4
#endif

#ifndef CONFIG_USBDEV_MSC_STACKSIZE
#define CONFIG_USBDEV_MSC_STACKSIZE 2048
#endif

/* ---- 其他 class 占位（均不用，防编译期检查） ---- */
#ifndef CONFIG_USBDEV_RNDIS_RESP_BUFFER_SIZE
#define CONFIG_USBDEV_RNDIS_RESP_BUFFER_SIZE 156
#endif

#ifndef CONFIG_USBDEV_RNDIS_ETH_MAX_FRAME_SIZE
#define CONFIG_USBDEV_RNDIS_ETH_MAX_FRAME_SIZE 1580
#endif

#ifndef CONFIG_USBDEV_RNDIS_VENDOR_ID
#define CONFIG_USBDEV_RNDIS_VENDOR_ID 0x0000ffff
#endif

#ifndef CONFIG_USBDEV_RNDIS_VENDOR_DESC
#define CONFIG_USBDEV_RNDIS_VENDOR_DESC "CherryUSB"
#endif

/* ================= Host 占位（usbd_serial.c 误引用了 host 字段） ================= */
#ifndef CONFIG_USBHOST_DEV_NAMELEN
#define CONFIG_USBHOST_DEV_NAMELEN 16
#endif

#define USB_FLAG_TEST_AAA 0x18

#endif /* USB_CONFIG_H */