/*
 * drv_ospi_h7.c - GD32H7xx OSPI0 + OSPIM 驱动（板载 GD25X512ME 64MB 八线 Flash）
 *
 * 硬件事实（EVAL V2.1 原理图 OSPI 页实锤）：
 *   U7 = GD25X512MEBIRY（512Mbit 八线 NOR，JEDEC ID 0xC8481AFF）
 *   NCS=PB6(AF10)  CLK=PB2(AF9)
 *   IO0~IO7 = PD11/PD12(AF9) PA3(AF6) PD13(AF9) PD4/PD5/PD6/PD7(AF10)
 *   DQS 悬空（只能 no-DQS 模式）；RESET=NRST；WP/ECS 已上拉。
 *   跳线前提：JP61 拨 (2,3)=OSPI 档（PB6 与 DCI_D5 二选一）。
 *   内存映射基址：OSPI0 -> 0x90000000。
 *
 * 芯片命令层直接用官方 demo 的 gd25x512me.c（纯 SPL，无裸机依赖），
 * 本文件只做 RT-Thread 集成：初始化 + msh 测试命令。
 *
 * msh 命令：
 *   ospi_id     - 读 JEDEC ID（链路活没活的第一个判据）
 *   ospi_test1  - 单线间接模式 擦->写->读->校验 @0x000000
 *   ospi_test8  - 八线间接模式 擦->写->读->校验 @0x200000
 *   ospi_mmap   - 内存映射模式读校验 @0x400000（直接指针读 Flash！）
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_OSPI)

#include "gd25x512me.h"

#define DBG_TAG             "drv.ospi"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define GD25X512ME_ID       0xC8481AFF
#define OSPI_MMAP_BASE      0x90000000UL

ospi_parameter_struct ospi_struct;      /* drv_ospi_fal.c 也要用，不 static */

static int rt_hw_ospi_init(void)
{
    ospi_flash_init(OSPI0, &ospi_struct);

    /* 两种模式各复位一次（官方 demo 同款顺序）：
     * 上电后不知道 Flash 上次停在哪种模式，两边都敲门 */
    ospi_flash_reset_enable(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_reset_memory(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_reset_enable(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_reset_memory(OSPI0, &ospi_struct, OSPI_MODE);

    LOG_I("ospi0 ready: GD25X512ME 64MB, prescaler=%d", ospi_struct.prescaler);
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_ospi_init);

#ifdef RT_USING_FINSH
#include <finsh.h>

/* 里程碑 1：读 JEDEC ID */
static void ospi_id(int argc, char **argv)
{
    uint32_t id = ospi_flash_read_id(OSPI0, &ospi_struct, SPI_MODE);
    rt_kprintf("ospi: JEDEC ID = 0x%08lX %s (期望 0x%08lX)\n",
               id, id == GD25X512ME_ID ? "MATCH" : "MISMATCH!", GD25X512ME_ID);
}
MSH_CMD_EXPORT(ospi_id, read GD25X512ME JEDEC ID);

/* 通用：间接模式 擦->写->读->校验。mode: SPI_MODE / OSPI_MODE */
static int ospi_rw_verify(interface_mode mode, uint32_t addr, const char *tag)
{
    uint8_t tx[128], rx[128];
    uint32_t i;

    for (i = 0; i < sizeof(tx); i++) tx[i] = (uint8_t)(i ^ 0x5A);

    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, mode);
    ospi_flash_write_enbale(OSPI0, &ospi_struct, mode);
    ospi_flash_block_erase(OSPI0, &ospi_struct, mode,
                           GD25X512ME_3BYTES_SIZE, addr, GD25X512ME_ERASE_4K);
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, mode);

    ospi_flash_write_enbale(OSPI0, &ospi_struct, mode);
    ospi_flash_page_program(OSPI0, &ospi_struct, mode,
                            GD25X512ME_3BYTES_SIZE, tx, addr, sizeof(tx));
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, mode);

    ospi_flash_read(OSPI0, &ospi_struct, mode,
                    GD25X512ME_3BYTES_SIZE, rx, addr, sizeof(rx));

    for (i = 0; i < sizeof(tx); i++) {
        if (tx[i] != rx[i]) {
            rt_kprintf("ospi %s: FAIL @+%lu, wrote 0x%02X read 0x%02X\n",
                       tag, i, tx[i], rx[i]);
            return -RT_ERROR;
        }
    }
    rt_kprintf("ospi %s: erase->write->read->verify %u bytes @0x%06lX PASS\n",
               tag, sizeof(tx), addr);
    return RT_EOK;
}

/* 里程碑 2：单线（兼容模式）读写 */
static void ospi_test1(int argc, char **argv)
{
    ospi_rw_verify(SPI_MODE, 0x000000, "1-line");
}
MSH_CMD_EXPORT(ospi_test1, 1-line indirect erase/write/read verify);

/* 里程碑 3：八线间接模式（先配 dummy cycle 和 CFG0 进 Octal STR） */
static void ospi_test8(int argc, char **argv)
{
    /* 配置 Flash 侧：16 dummy cycles + 进入 8-8-8 STR 模式（官方 demo 顺序） */
    ospi_flash_write_enbale(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_write_volatilecfg_register(OSPI0, &ospi_struct, SPI_MODE,
            GD25X512ME_3BYTES_SIZE, GD25X512ME_CFG_REG1_ADDR,
            GD25X512ME_CFG_16_DUMMY_CYCLES);
    ospi_flash_write_enbale(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_write_volatilecfg_register(OSPI0, &ospi_struct, SPI_MODE,
            GD25X512ME_3BYTES_SIZE, GD25X512ME_CFG_REG0_ADDR,
            GD25X512ME_CFG_OCTAL_STR_WO);
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);

    ospi_rw_verify(OSPI_MODE, 0x200000, "8-line");
}
MSH_CMD_EXPORT(ospi_test8, 8-line indirect erase/write/read verify);

/* 里程碑 4：内存映射读——Flash 直接映射到 0x90000000，指针读 */
static void ospi_mmap(int argc, char **argv)
{
    uint8_t tx[64];
    const volatile uint8_t *mm = (const volatile uint8_t *)(OSPI_MMAP_BASE + 0x400000);
    uint32_t i, bad = 0;

    for (i = 0; i < sizeof(tx); i++) tx[i] = (uint8_t)(0xA5 + i);

    /* 写还是走间接模式（八线） */
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_write_enbale(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_block_erase(OSPI0, &ospi_struct, OSPI_MODE,
                           GD25X512ME_3BYTES_SIZE, 0x400000, GD25X512ME_ERASE_4K);
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_write_enbale(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_page_program(OSPI0, &ospi_struct, OSPI_MODE,
                            GD25X512ME_3BYTES_SIZE, tx, 0x400000, sizeof(tx));
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);

    /* 切内存映射模式，直接指针读 */
    ospi_flash_memory_map_mode_wrap_enable(OSPI0, &ospi_struct, OSPI_MODE,
                                           GD25X512ME_3BYTES_SIZE);
    for (i = 0; i < sizeof(tx); i++) {
        if (mm[i] != tx[i]) bad++;
    }
    rt_kprintf("ospi mmap: pointer-read @0x%08lX, %u/%u bytes match %s\n",
               OSPI_MMAP_BASE + 0x400000, sizeof(tx) - bad, sizeof(tx),
               bad == 0 ? "PASS" : "FAIL");

    /* 注意：memory-mapped 模式下 OSPI 控制器被占用，
     * 要回间接模式需重新 ospi_init——这里给出提示，生产代码按需切换 */
}
MSH_CMD_EXPORT(ospi_mmap, memory-mapped read verify @0x90000000);
#endif /* RT_USING_FINSH */

#endif /* BSP_USING_OSPI */