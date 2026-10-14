/*
 * drv_dci_h7.c - GD32H7xx DCI + OV2640 摄像头驱动（RT-Thread）
 *
 * 硬件链路（官方 25_DCI_OV2640 demo 锁定）：
 *   SCCB 配置口 = 硬件 I2C1（SCL=PH4/SDA=PB11，AF4，复用 drv_i2c_h7 的 "i2c1" 总线）
 *   XCLK       = CKOUT0(PA8, AF0) = HXTAL(25MHz)/3 ≈ 8.33MHz
 *   DCI(AF13)  = PIXCLK=PE3, VSYNC=PB7, HSYNC=PA4,
 *                D0-D7 = PC6/PH10/PC8/PC9/PE4/PB6/PE5/PE6
 *   8bit 连续模式，上升沿采样，HSYNC/VSYNC 低有效
 *   DMA1 CH7（DMA_REQUEST_DCI）循环模式，32bit 宽度，
 *   38400 字 = 320*240*2 字节 -> SDRAM 0xC0000000
 *
 * 无屏验证法（屏还没到时照样能确认摄像头）：
 *   cam_test [n] —— 收 n 帧后统计帧缓冲：全0/全FF=没数据，
 *   有方差有色块 = 真采到图像；cam_dump 可看局部像素。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "gd32h7xx.h"

#ifdef BSP_USING_DCI_OV2640

#include "ov2640_init_table.h"

#define DBG_TAG             "drv.dci"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define CAM_I2C_BUS         "i2c1"
#define OV2640_ADDR_7BIT    0x30            /* 8位写地址0x60的7位形式 */

#define CAM_WIDTH           320
#define CAM_HEIGHT          240
#define CAM_FB_ADDR         0xC0000000      /* SDRAM 帧缓冲（demo 同款地址） */
#define CAM_FB_WORDS        38400           /* 320*240*2/4 */
#define CAM_FB_BYTES        (CAM_WIDTH * CAM_HEIGHT * 2)

#define DCI_DATA_ADDR       ((uint32_t)&DCI_DATA)   /* 0x4C050028 */

static volatile rt_uint32_t cam_frame_count = 0;    /* FTF 中断计数 = 已收完整帧数 */
static struct rt_semaphore  cam_frame_sem;          /* 帧同步信号量 */
static rt_bool_t            cam_inited = RT_FALSE;
static struct rt_device    *cam_i2c = RT_NULL;

/* ---------------- SCCB：走 RT-Thread I2C 框架 ---------------- */
static rt_err_t ov2640_write_reg(rt_uint8_t reg, rt_uint8_t val)
{
    rt_uint8_t buf[2] = { reg, val };
    return rt_i2c_master_send((struct rt_i2c_bus_device *)cam_i2c,
                              OV2640_ADDR_7BIT, 0, buf, 2) == 2 ? RT_EOK : -RT_EIO;
}

static rt_err_t ov2640_read_reg(rt_uint8_t reg, rt_uint8_t *val)
{
    struct rt_i2c_msg msgs[2];

    msgs[0].addr  = OV2640_ADDR_7BIT;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;
    msgs[1].addr  = OV2640_ADDR_7BIT;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].buf   = val;
    msgs[1].len   = 1;

    return rt_i2c_transfer((struct rt_i2c_bus_device *)cam_i2c, msgs, 2) == 2
           ? RT_EOK : -RT_EIO;
}

/* OV2640 ID：MIDH=0x7F MIDL=0xA2 PID=0x26 VER=0x42 为正品特征 */
static rt_err_t ov2640_id_check(void)
{
    rt_uint8_t midh = 0, midl = 0, pid = 0, ver = 0;

    ov2640_write_reg(0xFF, 0x01);           /* 切到 bank 1 */
    ov2640_read_reg(0x1C, &midh);
    ov2640_read_reg(0x1D, &midl);
    ov2640_read_reg(0x0A, &pid);
    ov2640_read_reg(0x0B, &ver);
    rt_kprintf("[cam] ID: MIDH=0x%02x MIDL=0x%02x PID=0x%02x VER=0x%02x\n",
               midh, midl, pid, ver);
    if (midh == 0x7F && midl == 0xA2 && pid == 0x26)
        return RT_EOK;
    return -RT_ERROR;
}

static rt_err_t ov2640_outsize_set(rt_uint16_t w, rt_uint16_t h)
{
    rt_uint16_t outw = w / 4, outh = h / 4;
    rt_uint8_t temp;

    if ((w % 4) || (h % 4)) return -RT_EINVAL;
    ov2640_write_reg(0xFF, 0x00);
    ov2640_write_reg(0xE0, 0x04);
    ov2640_write_reg(0x5A, outw & 0xFF);
    ov2640_write_reg(0x5B, outh & 0xFF);
    temp = ((outw >> 8) & 0x03) | ((outh >> 6) & 0x04);
    ov2640_write_reg(0x5C, temp);
    ov2640_write_reg(0xE0, 0x00);
    return RT_EOK;
}

/* ---------------- 引脚/外设（逐行对照 demo dci_config） ---------------- */
static void dci_gpio_config(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_GPIOH);
    rcu_periph_clock_enable(RCU_DCI);

    /* PIXCLK=PE3, VSYNC=PB7, HSYNC=PA4 */
    gpio_af_set(GPIOE, GPIO_AF_13, GPIO_PIN_3);
    gpio_af_set(GPIOB, GPIO_AF_13, GPIO_PIN_7);
    gpio_af_set(GPIOA, GPIO_AF_13, GPIO_PIN_4);
    gpio_mode_set(GPIOE, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_3);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_7);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_4);
    gpio_output_options_set(GPIOE, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_3);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_7);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_4);

    /* D0-D7 = PC6, PH10, PC8, PC9, PE4, PB6, PE5, PE6 */
    gpio_af_set(GPIOC, GPIO_AF_13, GPIO_PIN_6 | GPIO_PIN_8 | GPIO_PIN_9);
    gpio_af_set(GPIOH, GPIO_AF_13, GPIO_PIN_10);
    gpio_af_set(GPIOB, GPIO_AF_13, GPIO_PIN_6);
    gpio_af_set(GPIOE, GPIO_AF_13, GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_6 | GPIO_PIN_8 | GPIO_PIN_9);
    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_10);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_6);
    gpio_mode_set(GPIOE, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_6 | GPIO_PIN_8 | GPIO_PIN_9);
    gpio_output_options_set(GPIOH, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_10);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_6);
    gpio_output_options_set(GPIOE, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6);
}

static void dci_periph_config(void)
{
    dci_parameter_struct dci_struct;
    dma_single_data_parameter_struct dma_struct;

    /* DCI：8bit 连续，上升沿，HSYNC/VSYNC 低有效 */
    dci_struct.capture_mode     = DCI_CAPTURE_MODE_CONTINUOUS;
    dci_struct.clock_polarity   = DCI_CK_POLARITY_RISING;
    dci_struct.hsync_polarity   = DCI_HSYNC_POLARITY_LOW;
    dci_struct.vsync_polarity   = DCI_VSYNC_POLARITY_LOW;
    dci_struct.frame_rate       = DCI_FRAME_RATE_ALL;
    dci_struct.interface_format = DCI_INTERFACE_FORMAT_8BITS;
    dci_init(&dci_struct);

    /* DMA1 CH7 循环搬运：DCI_DATA -> SDRAM 帧缓冲 */
    rcu_periph_clock_enable(RCU_DMA1);
    rcu_periph_clock_enable(RCU_DMAMUX);

    dma_single_data_para_struct_init(&dma_struct);
    dma_deinit(DMA1, DMA_CH7);
    dma_struct.request             = DMA_REQUEST_DCI;
    dma_struct.periph_addr         = DCI_DATA_ADDR;
    dma_struct.memory0_addr        = CAM_FB_ADDR;
    dma_struct.direction           = DMA_PERIPH_TO_MEMORY;
    dma_struct.number              = CAM_FB_WORDS;
    dma_struct.periph_inc          = DMA_PERIPH_INCREASE_DISABLE;
    dma_struct.memory_inc          = DMA_MEMORY_INCREASE_ENABLE;
    dma_struct.periph_memory_width = DMA_PERIPH_WIDTH_32BIT;
    dma_struct.circular_mode       = DMA_CIRCULAR_MODE_ENABLE;
    dma_struct.priority            = DMA_PRIORITY_HIGH;
    dma_single_data_mode_init(DMA1, DMA_CH7, &dma_struct);
}

/* XCLK = CKOUT0(PA8) = HXTAL/3 ≈ 8.33MHz，OV2640 内部 PLL 再倍频 */
static void ckout0_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOA);
    gpio_af_set(GPIOA, GPIO_AF_0, GPIO_PIN_8);
    gpio_mode_set(GPIOA, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_8);
    gpio_output_options_set(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_8);
    rcu_ckout0_config(RCU_CKOUT0SRC_HXTAL, RCU_CKOUT0_DIV3);
}

/* ---------------- DMA 满帧中断：一帧落地 ---------------- */
void DMA1_Channel7_IRQHandler(void)
{
    rt_interrupt_enter();
    if (dma_interrupt_flag_get(DMA1, DMA_CH7, DMA_INT_FLAG_FTF)) {
        dma_interrupt_flag_clear(DMA1, DMA_CH7, DMA_INT_FLAG_FTF);
        cam_frame_count++;
        rt_sem_release(&cam_frame_sem);
    }
    rt_interrupt_leave();
}

/* ---------------- 对外 API ---------------- */

/* 初始化：I2C 总线须在（drv_i2c_h7 注册的 "i2c1"），SDRAM 须在 */
rt_err_t dci_camera_init(void)
{
    rt_uint32_t i;

    if (cam_inited) return RT_EOK;

    cam_i2c = rt_device_find(CAM_I2C_BUS);
    if (cam_i2c == RT_NULL)
    {
        LOG_E("i2c bus \"%s\" not found (BSP_USING_I2C1 enabled?)", CAM_I2C_BUS);
        return -RT_ENOSYS;
    }

    dci_gpio_config();
    dci_periph_config();
    ckout0_init();
    rt_thread_mdelay(100);

    /* OV2640 复位 */
    if (ov2640_write_reg(0xFF, 0x01) != RT_EOK ||
        ov2640_write_reg(0x12, 0x80) != RT_EOK)
    {
        LOG_E("SCCB no ACK —— 摄像头没接上或 I2C1 不通");
        return -RT_EIO;
    }
    rt_thread_mdelay(10);

    if (ov2640_id_check() != RT_EOK)
    {
        LOG_E("OV2640 ID mismatch");
        return -RT_ERROR;
    }

    /* SVGA 基础表 -> RGB565 表 -> 输出尺寸 320x240 */
    for (i = 0; i < sizeof(ov2640_svga_init_reg_tbl) / 2; i++)
        ov2640_write_reg(ov2640_svga_init_reg_tbl[i][0], ov2640_svga_init_reg_tbl[i][1]);
    rt_thread_mdelay(100);
    for (i = 0; i < sizeof(ov2640_rgb565_reg_tbl) / 2; i++)
        ov2640_write_reg(ov2640_rgb565_reg_tbl[i][0], ov2640_rgb565_reg_tbl[i][1]);
    rt_thread_mdelay(100);
    ov2640_outsize_set(CAM_WIDTH, CAM_HEIGHT);

    /* 开 DMA 中断 + 通道 + DCI 采集 */
    rt_sem_init(&cam_frame_sem, "camfrm", 0, RT_IPC_FLAG_PRIO);
    nvic_irq_enable(DMA1_Channel7_IRQn, 0, 1);
    dma_interrupt_enable(DMA1, DMA_CH7, DMA_INT_FTF);
    dma_channel_enable(DMA1, DMA_CH7);
    dci_enable();
    dci_capture_enable();

    cam_inited = RT_TRUE;
    LOG_I("OV2640 ready: %dx%d RGB565 -> 0x%08x", CAM_WIDTH, CAM_HEIGHT, CAM_FB_ADDR);
    return RT_EOK;
}

rt_uint32_t dci_camera_frame_count(void) { return cam_frame_count; }
rt_uint32_t dci_camera_fb_addr(void)     { return CAM_FB_ADDR; }

/* ---------------- 无屏验证命令 ---------------- */
static int cam_init(int argc, char **argv)
{
    return dci_camera_init() == RT_EOK ? 0 : -1;
}
MSH_CMD_EXPORT(cam_init, init OV2640 camera via DCI);

/* SCCB 链路诊断：对常见摄像头 7 位地址逐个试 ACK。
 * 任何一个地址有 ACK = I2C1 电气链路通，剩下只是地址/型号问题；
 * 全部 NACK = 线没接好/跳线不对/模组没供电/无上拉。 */
static int cam_scan(int argc, char **argv)
{
    /* 0x50 = 板载 AT24C02（对照组）：它 ACK = I2C1 引擎/引脚无罪，
     * 问题纯在摄像头侧；它都 NACK = 总线本身坏了 */
    static const rt_uint8_t addrs[] = { 0x50, 0x30, 0x21, 0x3C, 0x20, 0x42, 0x36 };
    rt_uint32_t i;
    rt_uint8_t dummy = 0xFF;
    rt_device_t bus;

    bus = rt_device_find(CAM_I2C_BUS);
    if (bus == RT_NULL)
    {
        rt_kprintf("cam_scan: i2c bus \"%s\" not found\n", CAM_I2C_BUS);
        return -1;
    }
    for (i = 0; i < sizeof(addrs); i++)
    {
        rt_ssize_t r = rt_i2c_master_send((struct rt_i2c_bus_device *)bus,
                                          addrs[i], 0, &dummy, 1);
        rt_kprintf("cam_scan: addr 0x%02x -> %s\n", addrs[i],
                   r == 1 ? "ACK!" : "nack");
    }
    rt_kprintf("cam_scan: done (OV2640=0x30, OV7670/GC0308=0x21, OV5640=0x3c)\n");
    return 0;
}
MSH_CMD_EXPORT(cam_scan, probe common camera SCCB addresses);

/* cam_test [n]：等 n 帧（默认5），然后统计帧缓冲判定是否真有图像 */
static int cam_test(int argc, char **argv)
{
    rt_uint32_t want = (argc > 1) ? atoi(argv[1]) : 5;
    rt_uint32_t start, i;
    volatile rt_uint16_t *fb = (volatile rt_uint16_t *)CAM_FB_ADDR;
    rt_uint32_t zero = 0, full = 0, other = 0;
    rt_uint32_t sum = 0, min = 0xFFFF, max = 0;
    rt_uint32_t t0;

    if (dci_camera_init() != RT_EOK) return -1;

    start = cam_frame_count;
    t0 = rt_tick_get();
    while (cam_frame_count - start < want)
    {
        if (rt_tick_get() - t0 > rt_tick_from_millisecond(5000))
        {
            rt_kprintf("cam_test: TIMEOUT, frames=%u (DCI/DMA 没有帧落地，查接线/跳线)\n",
                       cam_frame_count - start);
            return -1;
        }
        rt_thread_mdelay(10);
    }
    rt_kprintf("cam_test: %u frames received (total=%u)\n", want, cam_frame_count);

    /* CPU 读 DMA 写的 SDRAM 前先无效化 D-Cache */
    SCB_InvalidateDCache_by_Addr((void *)CAM_FB_ADDR, CAM_FB_BYTES);

    /* 抽 1/16 像素统计：全 0x0000/全 0xFFFF = 没采到 */
    for (i = 0; i < CAM_FB_BYTES / 2; i += 16)
    {
        rt_uint16_t p = fb[i];
        if (p == 0x0000)      zero++;
        else if (p == 0xFFFF) full++;
        else                  other++;
        sum += p;
        if (p < min) min = p;
        if (p > max) max = p;
    }
    rt_kprintf("cam_test: sampled=%u zero=%u full=%u other=%u min=0x%04x max=0x%04x mean=%u\n",
               CAM_FB_BYTES / 2 / 16, zero, full, other, min, max,
               sum / (CAM_FB_BYTES / 2 / 16));
    if (other > (CAM_FB_BYTES / 2 / 16) / 2)
        rt_kprintf("cam_test: PASS —— 像素有方差，摄像头真采到图像了\n");
    else
        rt_kprintf("cam_test: 像素千篇一律，数据通路有问题\n");
    return 0;
}
MSH_CMD_EXPORT(cam_test, camera frame stats test [frames]);

/* cam_dump [x y]：dump 以 (x,y) 为左上角的 8x8 像素块 */
static int cam_dump(int argc, char **argv)
{
    rt_uint32_t x = (argc > 1) ? atoi(argv[1]) : 156;
    rt_uint32_t y = (argc > 2) ? atoi(argv[2]) : 116;
    volatile rt_uint16_t *fb = (volatile rt_uint16_t *)CAM_FB_ADDR;
    rt_uint32_t r, c;

    if (!cam_inited) { rt_kprintf("cam_dump: run cam_init first\n"); return -1; }
    SCB_InvalidateDCache_by_Addr((void *)CAM_FB_ADDR, CAM_FB_BYTES);
    rt_kprintf("cam_dump: 8x8 block at (%u,%u):\n", x, y);
    for (r = 0; r < 8; r++)
    {
        for (c = 0; c < 8; c++)
            rt_kprintf("%04x ", fb[(y + r) * CAM_WIDTH + (x + c)]);
        rt_kprintf("\n");
    }
    return 0;
}
MSH_CMD_EXPORT(cam_dump, dump 8x8 pixels at [x y]);

#endif /* BSP_USING_DCI_OV2640 */

/* ================= cam_bb：GPIO 模拟 SCCB 对照组 =================
 * 判决逻辑（对照组思想，和黑屏案 lcd_bb 同款）：
 *   硬件 I2C1 已被 AT24C02(0x50) 作证无罪，剩下两个嫌疑：
 *     A. 400kHz 对摄像头太快（官方 demo 用 100kHz，内部弱上拉）
 *     B. XCLK 没真正送到摄像头（OV2640 没时钟时 SCCB 不应答）
 *   位 bang 速度仅 ~50kHz 且完全绕开 I2C 外设：
 *     bang 出 ACK  -> 硬件 I2C 时序/速率问题，降速即可
 *     bang 也 NACK -> 摄像头没醒：XCLK/供电/跳帽问题
 * 顺手 dump PA8 与 RCU_CFG0 的 CKOUT0 域，无仪器核对 XCLK 配置。
 * 引脚临时从 AF4 抢回 GPIO，测试结束恢复原状（AF4 OD 上拉）。 */

#define BB_SCL_PORT   GPIOH
#define BB_SCL_PIN    GPIO_PIN_4
#define BB_SDA_PORT   GPIOB
#define BB_SDA_PIN    GPIO_PIN_11

/* 600MHz 下约 10us 半周期 -> SCCB ~50kHz，慢到不可能有时序冤案 */
static void bb_dly(void)
{
    volatile int i;
    for (i = 0; i < 6000; i++) { __NOP(); }
}

/* 开漏语义：写 1 = 释放（靠上拉拉高），写 0 = 主动拉低。
 * SCL 释放后等它真的变高：容忍从机时钟拉伸（clock stretching），
 * 从机若按住 SCL 不放，死等到超时为止 */
static void bb_scl(int v)
{
    gpio_bit_write(BB_SCL_PORT, BB_SCL_PIN, v ? SET : RESET);
    bb_dly();
    if (v) {
        volatile int t = 100000;
        while (gpio_input_bit_get(BB_SCL_PORT, BB_SCL_PIN) == RESET && --t > 0) { __NOP(); }
    }
}
static void bb_sda(int v) { gpio_bit_write(BB_SDA_PORT, BB_SDA_PIN, v ? SET : RESET); bb_dly(); }
static int  bb_sda_rd(void) { return gpio_input_bit_get(BB_SDA_PORT, BB_SDA_PIN) == SET; }

static void bb_gpio_take(void)
{
    /* 抢回成 GPIO 开漏输出，初态双释放 */
    gpio_mode_set(BB_SCL_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, BB_SCL_PIN);
    gpio_output_options_set(BB_SCL_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ, BB_SCL_PIN);
    gpio_mode_set(BB_SDA_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, BB_SDA_PIN);
    gpio_output_options_set(BB_SDA_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ, BB_SDA_PIN);
    gpio_bit_set(BB_SCL_PORT, BB_SCL_PIN);
    gpio_bit_set(BB_SDA_PORT, BB_SDA_PIN);
    bb_dly();
}

static void bb_gpio_release(void)
{
    /* 归还 AF4 开漏上拉，与 drv_i2c_h7 初始化一致 */
    gpio_af_set(BB_SCL_PORT, GPIO_AF_4, BB_SCL_PIN);
    gpio_af_set(BB_SDA_PORT, GPIO_AF_4, BB_SDA_PIN);
    gpio_mode_set(BB_SCL_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP, BB_SCL_PIN);
    gpio_output_options_set(BB_SCL_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ, BB_SCL_PIN);
    gpio_mode_set(BB_SDA_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP, BB_SDA_PIN);
    gpio_output_options_set(BB_SDA_PORT, GPIO_OTYPE_OD, GPIO_OSPEED_12MHZ, BB_SDA_PIN);
}

static void bb_start(void)
{
    bb_sda(1); bb_scl(1);
    bb_sda(0);          /* SDA 下降沿（SCL 高）= START */
    bb_scl(0);
}

static void bb_stop(void)
{
    bb_sda(0); bb_scl(1);
    bb_sda(1);          /* SDA 上升沿（SCL 高）= STOP */
    bb_dly();
}

/* 发一个字节，返回 1 = 收到 ACK */
static int bb_write(rt_uint8_t v)
{
    int i, ack;
    for (i = 7; i >= 0; i--) {
        bb_sda((v >> i) & 1);
        bb_scl(1); bb_scl(0);
    }
    bb_sda(1);          /* 第 9 拍：释放 SDA 让从机拉 */
    bb_scl(1);
    ack = (bb_sda_rd() == 0);
    bb_scl(0);
    return ack;
}

/* 全地址扫描：0x08~0x77 每个 7 位地址敲一次门，只报告应答者 */
static void bb_scan_all(const char *tag)
{
    int addr, found = 0;
    rt_kprintf("--- %s ---\n", tag);
    for (addr = 0x08; addr < 0x78; addr++) {
        bb_start();
        int ack = bb_write((rt_uint8_t)(addr << 1));
        bb_stop();
        if (ack) {
            rt_kprintf("cam_bb: *** addr 0x%02x -> ACK! ***\n", addr);
            found++;
        }
    }
    rt_kprintf("cam_bb: 扫描完成，%d 个设备应答（0x50=AT24C02 应始终在列）\n", found);
}

static int cam_bb(int argc, char **argv)
{
    rt_uint8_t midh = 0, midl = 0, pid = 0;
    rt_uint32_t i;

    bb_gpio_take();

    /* 第一档：HXTAL/3 ≈ 8.33MHz（OV2640 官方 demo 配置） */
    ckout0_init();
    rt_thread_mdelay(100);
    rt_kprintf("cam_bb: 档位1 RCU_CFG2=0x%08lx (低7位=0x23 即 HXTAL/3)\n", RCU_CFG2);
    bb_scan_all("XCLK=8.33MHz 扫描");

    /* 第二档：HXTAL/1 = 25MHz（OV5640 需要的量级；OV2640 上限 27MHz 也安全） */
    rcu_ckout0_config(RCU_CKOUT0SRC_HXTAL, RCU_CKOUT0_DIV1);
    rt_thread_mdelay(100);
    rt_kprintf("cam_bb: 档位2 RCU_CFG2=0x%08lx (低7位=0x21 即 HXTAL/1)\n", RCU_CFG2);
    bb_scan_all("XCLK=25MHz 扫描");

    /* 裸探测可能是盲区（某些 OV 传感器不应答"地址+STOP"）。
     * 复刻原厂完整写交易：地址+寄存器+数据，逐字节看 ACK */
    {
        int a0, a1, a2;
        bb_start();
        a0 = bb_write(0x30 << 1);       /* 0x60 写地址 */
        a1 = bb_write(0xFF);            /* bank sel */
        a2 = bb_write(0x01);
        bb_stop();
        rt_kprintf("cam_bb: 完整写[60 FF 01] ACK = %d/%d/%d (地址/寄存器/数据)\n",
                   a0, a1, a2);
    }

    /* 读 ID 三兄弟：0x1C=MIDH(7F) 0x1D=MIDL(A2) 0x0A=PID(26)，无条件尝试。
     * SCCB 读不保证地址自增，每个寄存器单独走"写地址->重发->读"完整流程 */
    {
        static const rt_uint8_t id_regs[3] = { 0x1C, 0x1D, 0x0A };
        rt_uint8_t *dst[3] = { &midh, &midl, &pid };
        for (i = 0; i < 3; i++) {
            int b; rt_uint8_t v = 0;
            bb_start();
            bb_write(0x30 << 1);
            bb_write(id_regs[i]);           /* 指定寄存器地址 */
            bb_stop();
            bb_start();
            bb_write((0x30 << 1) | 1);      /* 读方向 */
            for (b = 7; b >= 0; b--) {
                bb_scl(1);
                if (bb_sda_rd()) v |= (1 << b);
                bb_scl(0);
            }
            *dst[i] = v;
            bb_sda(1);                      /* 单字节读完：NACK */
            bb_scl(1); bb_scl(0);
            bb_sda(1);
            bb_stop();
        }
        rt_kprintf("cam_bb: MIDH=0x%02x MIDL=0x%02x PID=0x%02x (期望 7F A2 26；全00/全FF=无应答)\n",
                   midh, midl, pid);
    }

    ckout0_init();      /* XCLK 还原 8.33MHz，不给后面的流程留烂摊子 */
    bb_gpio_release();
    return 0;
}
MSH_CMD_EXPORT(cam_bb, bitbang SCCB control group on PH4/PB11);

/* ================= cam_xclk：无仪器测量 XCLK 频率 =================
 * PA8=CKOUT0 处于 AF 模式时，GPIOA_ISTAT 仍实时反映引脚电平。
 * 600MHz 下紧凑采样循环约 20ns/次，对 8.33MHz 信号是 40 倍过采样，
 * 数 100ms 内的上升沿即可算出频率：
 *   期望 ~833000 边沿 -> XCLK 活着，摄像头没醒另有原因
 *   0 或极小           -> XCLK 没出来，SCCB 不应答天经地义 */
static int cam_xclk(int argc, char **argv)
{
    rt_tick_t start;
    rt_uint32_t prev, edges = 0, samples = 0;

    ckout0_init();  /* 幂等重配，排除"忘了跑 cam_init"的干扰 */
    rt_thread_mdelay(10);

    /* CKOUT0 配置在 RCU_CFG2：bit0:3=分频(应=3)，bit4:6=源(应=2 即 HXTAL)，
     * 即低 7 位应读出 0x23 */
    rt_kprintf("cam_xclk: RCU_CFG2=0x%08lx (低7位应=0x23 即 HXTAL/3)\n", RCU_CFG2);

    start = rt_tick_get();
    prev = GPIO_ISTAT(GPIOA) & GPIO_PIN_8;
    while (rt_tick_get() - start < 100) {          /* 100ms @ RT_TICK=1000 */
        rt_uint32_t n;
        /* 512 次采样才查一次 tick：循环体缩到几条指令，采样率冲 30MHz+ */
        for (n = 0; n < 512; n++) {
            rt_uint32_t cur = GPIO_ISTAT(GPIOA) & GPIO_PIN_8;
            if (cur && !prev) edges++;
            prev = cur;
            samples++;
        }
    }

    rt_kprintf("cam_xclk: edges=%lu in 100ms -> XCLK ~ %lu Hz (期望 8330000)\n",
               edges, edges * 10);
    rt_kprintf("cam_xclk: samples=%lu (采样率 %lu Hz，须远大于 16.6MHz 才可信)\n",
               samples, samples * 10);
    if (edges == 0 && samples > 0) {
        rt_kprintf("cam_xclk: PA8 全程无跳变 —— XCLK 死了！\n");
    } else if (samples * 10 < 20000000) {
        rt_kprintf("cam_xclk: 采样率不足 20MHz，测量欠采样，结果仅供参考\n");
    }
    return 0;
}
MSH_CMD_EXPORT(cam_xclk, measure XCLK frequency on PA8 without instruments);

/* ================= cam_free：释放 LCD 霸占的 PB4/PB5 =================
 * 怀疑：摄像头插座的 PWDN/RESET 路由到 PB4/PB5，被 SPI LCD 驱动
 * （RES=PB4/DC=PB5）按死在非工作态。本命令把两脚释放成浮空输入，
 * 恢复板上电阻决定的默认电平，然后由用户接着跑 cam_bb 验证。
 * 注意：执行后 LCD 的 RES/DC 失控，测完摄像头请重启再用 LCD。 */
static int cam_free(int argc, char **argv)
{
    gpio_mode_set(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO_PIN_4);
    gpio_mode_set(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO_PIN_5);
    rt_kprintf("cam_free: PB4/PB5 已释放为浮空输入 (GPIOB_CTL=0x%08lx)\n",
               GPIO_CTL(GPIOB));
    rt_kprintf("cam_free: 现在跑 cam_bb 看摄像头应不应答\n");
    return 0;
}
MSH_CMD_EXPORT(cam_free, release PB4/PB5 from LCD then retry camera);