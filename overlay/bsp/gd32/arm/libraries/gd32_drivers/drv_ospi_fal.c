/*
 * drv_ospi_fal.c - GD25X512ME 的 FAL（Flash Abstraction Layer）适配
 *
 * 把 gd25x512me.c 的裸 API 包装成 fal_flash_dev，供分区表/littlefs 使用。
 * 统一用八线模式（OSPI_MODE）：init 时复位后直接进入 8-8-8 STR，
 * 此后所有操作走八线，和 msh 的 ospi_test1（单线）互斥——
 * 开了 FAL 就别再跑 ospi_test1（会把协议状态打乱，反正它已经验证过了）。
 *
 * 地址范围：3 字节地址模式，只用低 16MB（0x000000~0xFFFFFF）。
 *           全容量 64MB 需要 4 字节地址模式（命令 0xB7 切换），留作后续。
 *
 * 分区表在 board/fal_cfg.h（fal 框架的强制约定）。
 */

#include <rtthread.h>
#include <board.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_OSPI) && defined(RT_USING_FAL)

#include <fal.h>
#include "gd25x512me.h"

#define DBG_TAG             "ospi.fal"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

extern ospi_parameter_struct ospi_struct;   /* drv_ospi_h7.c 里的全局配置 */

static int ospi_fal_init(void)
{
     rt_kprintf("[fal] init: reset (octal+spi)\n");
    /* 双模式复位：芯片此刻在哪种模式不确定，两种都发 */
    ospi_flash_reset_enable(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_reset_memory(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_reset_enable(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_reset_memory(OSPI0, &ospi_struct, SPI_MODE);

    rt_kprintf("[fal] init: cfg 16 dummy cycles\n");
    /* 在单线模式下写 volatile cfg，切到八线 STR */
    ospi_flash_write_enbale(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_write_volatilecfg_register(OSPI0, &ospi_struct, SPI_MODE,
            GD25X512ME_3BYTES_SIZE, GD25X512ME_CFG_REG1_ADDR,
            GD25X512ME_CFG_16_DUMMY_CYCLES);

    rt_kprintf("[fal] init: enter octal STR\n");
    ospi_flash_write_enbale(OSPI0, &ospi_struct, SPI_MODE);
    ospi_flash_write_volatilecfg_register(OSPI0, &ospi_struct, SPI_MODE,
            GD25X512ME_3BYTES_SIZE, GD25X512ME_CFG_REG0_ADDR,
            GD25X512ME_CFG_OCTAL_STR_WO);

    rt_kprintf("[fal] init: autopolling\n");
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);

    rt_kprintf("[fal] init: done\n");
    return 0;
}

static int ospi_fal_read(long offset, rt_uint8_t *buf, rt_size_t size)
{
    ospi_flash_read(OSPI0, &ospi_struct, OSPI_MODE,
                    GD25X512ME_3BYTES_SIZE, buf, offset, size);
    return size;
}

static int ospi_fal_write(long offset, const rt_uint8_t *buf, rt_size_t size)
{
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_write_enbale(OSPI0, &ospi_struct, OSPI_MODE);
    ospi_flash_page_program(OSPI0, &ospi_struct, OSPI_MODE,
                            GD25X512ME_3BYTES_SIZE, (uint8_t *)buf, offset, size);
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);
    return size;
}

static int ospi_fal_erase(long offset, rt_size_t size)
{
    rt_size_t erased = size;            /* fal 约定：返回擦除的字节数 */

    /* fal 保证按 blk_size(4K) 对齐调用，逐扇区擦 */
    while (size > 0) {
        ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);
        ospi_flash_write_enbale(OSPI0, &ospi_struct, OSPI_MODE);
        ospi_flash_block_erase(OSPI0, &ospi_struct, OSPI_MODE,
                               GD25X512ME_3BYTES_SIZE, offset, GD25X512ME_ERASE_4K);
        offset += GD25X512ME_SECTOR_4K;
        size -= GD25X512ME_SECTOR_4K;
    }
    ospi_flash_autopolling_mem_ready(OSPI0, &ospi_struct, OSPI_MODE);
    return erased;
}

const struct fal_flash_dev gd25x512_flash = {
    .name       = "gd25x512",
    .addr       = 0,
    .len        = 16 * 1024 * 1024,     /* 3 字节地址模式只用低 16MB */
    .blk_size   = GD25X512ME_SECTOR_4K,
    .ops        = { ospi_fal_init, ospi_fal_read, ospi_fal_write, ospi_fal_erase },
    .write_gran = 1,                   /* NOR: 按位编程 */
};

#endif /* BSP_USING_OSPI && RT_USING_FAL */