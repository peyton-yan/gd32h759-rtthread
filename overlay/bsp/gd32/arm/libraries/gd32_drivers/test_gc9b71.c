/*
 * test_gc9b71.c —— 官方 14_SPI_Quad_LCD demo 的 RT-Thread 移植壳
 *
 * 目的：用板载 GC9B71 屏（插 JP13）做 SPI4 链路的终极校准。
 * 下面的 rcu/gpio/spi 配置与官方 demo main.c 逐行一致，
 * 屏驱动本体（spi_quad_lcd_driver.c）原封不动来自 demo 包。
 *
 * 接线/跳线（指南 5.14）：
 *   GC9B71 屏插 JP13；JP41/JP45/JP46 跳 (2,3)（SPI_LCD 侧）。
 *
 * 用法：msh 里 gc9b71_test —— 屏依次刷 红/绿/蓝/白 各 1 秒，循环 3 轮。
 *   屏有颜色 = 板子/SPI4 引脚/时序全链路无罪，鬼在自家驱动侧；
 *   屏黑 = 板级/跳线/屏问题，与代码无关。
 */

#include <rtthread.h>
#include "gd32h7xx.h"
#include "spi_quad_lcd_driver.h"

/* ---- 以下三个函数与官方 demo main.c 逐行一致，仅去掉无关部分 ---- */

static void demo_rcu_config(void)
{
    /* enable the peripherals clock */
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOG);
    rcu_periph_clock_enable(RCU_GPIOH);
    rcu_periph_clock_enable(RCU_SPI4);
    rcu_spi_clock_config(IDX_SPI4, RCU_SPISRC_APB2);
}

static void demo_gpio_config(void)
{
    /* connect port to SPI4_NSS  -> PF6
                       SPI4_SCK  -> PH6
                       SPI4_MISO -> PH7
                       SPI4_MOSI -> PF9
                       SPI4_IO2  -> PH8
                       SPI4_IO3  -> PH9 */
    gpio_af_set(GPIOF, GPIO_AF_5, GPIO_PIN_9);
    gpio_af_set(GPIOH, GPIO_AF_5, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9);

    gpio_mode_set(GPIOF, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_9);
    gpio_output_options_set(GPIOF, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_9);

    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9);
    gpio_output_options_set(GPIOH, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_6 | GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9);

    gpio_mode_set(LCD_LEDK_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LCD_LEDK_PIN);
    gpio_output_options_set(LCD_LEDK_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, LCD_LEDK_PIN);
    gpio_bit_reset(LCD_LEDK_PORT, LCD_LEDK_PIN);

    gpio_mode_set(LCD_CS_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LCD_CS_PIN);
    gpio_output_options_set(LCD_CS_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, LCD_CS_PIN);
    gpio_bit_set(LCD_CS_PORT, LCD_CS_PIN);
}

static void demo_spi_config(void)
{
    spi_parameter_struct spi_init_struct;
    /* deinitilize SPI and the parameters */
    spi_i2s_deinit(LCD_SPI);
    spi_struct_para_init(&spi_init_struct);

    /* SPI parameter config */
    spi_init_struct.device_mode = SPI_MASTER;
    spi_init_struct.trans_mode = SPI_TRANSMODE_FULLDUPLEX;
    spi_init_struct.data_size = SPI_DATASIZE_8BIT;
    spi_init_struct.nss = SPI_NSS_SOFT;
    spi_init_struct.endian = SPI_ENDIAN_MSB;
    spi_init_struct.clock_polarity_phase = SPI_CK_PL_LOW_PH_1EDGE;
    spi_init_struct.prescale = SPI_PSC_8;
    spi_init(LCD_SPI, &spi_init_struct);

    /* enable SPI byte access */
    spi_byte_access_enable(LCD_SPI);

    /* enable SPI NSS output */
    spi_nss_output_enable(LCD_SPI);

    /* enable quad wire SPI_IO2 and SPI_IO3 pin output */
    spi_quad_io23_output_enable(LCD_SPI);

    spi_enable(LCD_SPI);
}

/* ---- msh 命令：跑官方序列 ---- */
static int gc9b71_test(int argc, char **argv)
{
    static const rt_uint16_t colors[] = {
        LCD_COLOR_RED, LCD_COLOR_GREEN, LCD_COLOR_BLUE, LCD_COLOR_WHITE
    };
    static const char *names[] = { "RED", "GREEN", "BLUE", "WHITE" };
    rt_uint32_t round, c;

    rt_kprintf("gc9b71_test: official demo sequence start\n");

    demo_rcu_config();
    demo_gpio_config();
    demo_spi_config();

    rt_thread_mdelay(120);

    spi_quad_lcd_init();
    rt_kprintf("gc9b71_test: init done, filling colors...\n");

    for (round = 0; round < 3; round++)
    {
        for (c = 0; c < 4; c++)
        {
            rt_kprintf("gc9b71_test: fill %s\n", names[c]);
            spi_quad_lcd_clean(colors[c]);
            rt_thread_mdelay(1000);
        }
    }
    rt_kprintf("gc9b71_test: done. 屏上看到了颜色 = SPI4 链路无罪\n");
    return 0;
}
MSH_CMD_EXPORT(gc9b71_test, official GC9B71 SPI LCD demo on RT-Thread);