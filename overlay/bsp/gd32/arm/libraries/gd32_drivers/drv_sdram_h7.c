/*
 * GD32H7 SDRAM 初始化 —— EXMC DEVICE0, MT48LC16M16A2 (32MB @ 0xC0000000)
 * RT-Thread 集成: INIT_BOARD_EXPORT 上电自动初始化
 *
 * 参数来源: 官方例程 16_EXMC_SDRAM (13行/9列/16bit/4bank/CL3/SDCLK=CK_EXMC/3)
 * 跳线: JP40 接 (1,2)  ((2,3) 是 I2S, 互斥)
 *
 * MPU: 0xC0000000 起 32MB 配为 Normal Non-Cacheable Shareable + XN。
 *   bring-up 阶段不用 cache, 杜绝一切一致性问题;
 *   后续 LCD 帧缓冲再按需改 Write-Back + cache 维护。
 *   MPU 区域号自动挑第一个未使能的, 不与 ENET 的 MPU 区冲突。
 */

#include <rtthread.h>
#include "gd32h7xx.h"

#define DBG_TAG "drv.sdram"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#if defined(BSP_USING_SDRAM) && defined(SOC_SERIES_GD32H7xx)

#define SDRAM_DEVICE0_ADDR   ((uint32_t)0xC0000000U)
#define SDRAM_SIZE           (32U * 1024U * 1024U)
#define SDRAM_TIMEOUT        ((uint32_t)0x0000FFFFU)

/* mode register 位定义（来自官方例程） */
#define SDRAM_MODEREG_BURST_LENGTH_1         ((uint16_t)0x0000U)
#define SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL  ((uint16_t)0x0000U)
#define SDRAM_MODEREG_CAS_LATENCY_3          ((uint16_t)0x0030U)
#define SDRAM_MODEREG_OPERATING_MODE_STANDARD ((uint16_t)0x0000U)
#define SDRAM_MODEREG_WRITEBURST_MODE_SINGLE ((uint16_t)0x0200U)

static void sdram_gpio_init(void)
{
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);
    rcu_periph_clock_enable(RCU_GPIOE);
    rcu_periph_clock_enable(RCU_GPIOF);
    rcu_periph_clock_enable(RCU_GPIOG);
    rcu_periph_clock_enable(RCU_GPIOH);

    /* SDNE0(PC2), SDCKE0(PC3) */
    gpio_af_set(GPIOC, GPIO_AF_12, GPIO_PIN_2 | GPIO_PIN_3);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_2 | GPIO_PIN_3);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_2 | GPIO_PIN_3);

    /* D12(PC0) */
    gpio_af_set(GPIOC, GPIO_AF_1, GPIO_PIN_0);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_0);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_0);

    /* D2(PD0),D3(PD1),D13(PD8),D14(PD9),D15(PD10),D0(PD14),D1(PD15) */
    gpio_af_set(GPIOD, GPIO_AF_12, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 |
                GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_mode_set(GPIOD, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 |
                GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_output_options_set(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 |
                            GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15);

    /* NBL0(PE0),NBL1(PE1),D4..D11(PE7..PE14) */
    gpio_af_set(GPIOE, GPIO_AF_12, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 |
                GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14);
    gpio_mode_set(GPIOE, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 |
                GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14);
    gpio_output_options_set(GPIOE, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_7 | GPIO_PIN_8 |
                            GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14);

    /* A0..A5(PF0..PF5), NRAS(PF11), A6..A9(PF12..PF15) */
    gpio_af_set(GPIOF, GPIO_AF_12, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 |
                GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_mode_set(GPIOF, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 |
                GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);
    gpio_output_options_set(GPIOF, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3 |
                            GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_11 | GPIO_PIN_12 |
                            GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15);

    /* A10(PG0),A11(PG1),A12(PG2),BA0(PG4),BA1(PG5),SDCLK(PG8),NCAS(PG15) */
    gpio_af_set(GPIOG, GPIO_AF_12, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
                GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_15);
    gpio_mode_set(GPIOG, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
                GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_15);
    gpio_output_options_set(GPIOG, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 |
                GPIO_PIN_5 | GPIO_PIN_8 | GPIO_PIN_15);

    /* SDNWE(PH5) */
    gpio_af_set(GPIOH, GPIO_AF_12, GPIO_PIN_5);
    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_5);
    gpio_output_options_set(GPIOH, GPIO_OTYPE_PP, GPIO_OSPEED_85MHZ, GPIO_PIN_5);
}

/* 在 MPU 里找第一个未使能的区域, 配为 Normal Non-Cacheable Shareable XN */
static rt_err_t sdram_mpu_config(void)
{
    rt_uint32_t rnr;
    rt_uint32_t rasr;

    for (rnr = 0; rnr < 16U; rnr++) {
        MPU->RNR = rnr;
        if ((MPU->RASR & MPU_RASR_ENABLE_Msk) == 0U) {
            break;
        }
    }
    if (rnr == 16U) {
        LOG_E("no free MPU region");
        return -RT_EFULL;
    }

    /* RASR: XN=1, AP=full(0b011), TEX=001(normal non-cacheable), S=1, C=0, B=0,
       SIZE=24 (32MB -> 2^(24+1)), ENABLE=1 */
    rasr = (1UL << MPU_RASR_XN_Pos)                    |
           (3UL << MPU_RASR_AP_Pos)                    |
           (1UL << MPU_RASR_TEX_Pos)                   |
           (1UL << MPU_RASR_S_Pos)                     |
           ((24UL << MPU_RASR_SIZE_Pos) & MPU_RASR_SIZE_Msk) |
           MPU_RASR_ENABLE_Msk;

    MPU->RNR  = rnr;
    MPU->RBAR = SDRAM_DEVICE0_ADDR;
    MPU->RASR = rasr;

    __DSB();
    __ISB();

    LOG_I("MPU region %lu: 0xC0000000 32MB normal non-cacheable", rnr);
    return RT_EOK;
}

static int h7_sdram_init(void)
{
    exmc_sdram_parameter_struct sdram_init_struct;
    exmc_sdram_timing_parameter_struct sdram_timing_init_struct;
    exmc_sdram_command_parameter_struct sdram_command_init_struct;
    uint32_t command_content, bank_select, timeout;
    volatile rt_uint32_t *probe;

    rcu_periph_clock_enable(RCU_EXMC);
    sdram_gpio_init();

    if (sdram_mpu_config() != RT_EOK) {
        return -RT_ERROR;
    }

    bank_select = EXMC_SDRAM_DEVICE0_SELECT;

    /* step 1: 时序寄存器 */
    sdram_timing_init_struct.load_mode_register_delay  = 2;
    sdram_timing_init_struct.exit_selfrefresh_delay    = 12;
    sdram_timing_init_struct.row_address_select_delay  = 8;
    sdram_timing_init_struct.auto_refresh_delay        = 11;
    sdram_timing_init_struct.write_recovery_delay      = 2;
    sdram_timing_init_struct.row_precharge_delay       = 4;
    sdram_timing_init_struct.row_to_column_delay       = 4;

    /* step 2: 控制寄存器 */
    exmc_sdram_struct_para_init(&sdram_init_struct);
    sdram_init_struct.sdram_device          = EXMC_SDRAM_DEVICE0;
    sdram_init_struct.column_address_width  = EXMC_SDRAM_COW_ADDRESS_9;
    sdram_init_struct.row_address_width     = EXMC_SDRAM_ROW_ADDRESS_13;
    sdram_init_struct.data_width            = EXMC_SDRAM_DATABUS_WIDTH_16B;
    sdram_init_struct.internal_bank_number  = EXMC_SDRAM_4_INTER_BANK;
    sdram_init_struct.cas_latency           = EXMC_CAS_LATENCY_3_SDCLK;
    sdram_init_struct.write_protection      = DISABLE;
    sdram_init_struct.sdclock_config        = EXMC_SDCLK_PERIODS_3_CK_EXMC;
    sdram_init_struct.burst_read_switch     = ENABLE;
    sdram_init_struct.pipeline_read_delay   = EXMC_PIPELINE_DELAY_1_CK_EXMC;
    sdram_init_struct.timing                = &sdram_timing_init_struct;
    exmc_sdram_init(&sdram_init_struct);

    /* step 3: CKE 拉高 */
    sdram_command_init_struct.command              = EXMC_SDRAM_CLOCK_ENABLE;
    sdram_command_init_struct.bank_select          = bank_select;
    sdram_command_init_struct.auto_refresh_number  = EXMC_SDRAM_AUTO_REFLESH_1_SDCLK;
    sdram_command_init_struct.mode_register_content = 0;
    timeout = SDRAM_TIMEOUT;
    while ((exmc_flag_get(EXMC_SDRAM_DEVICE0, EXMC_SDRAM_FLAG_NREADY) != RESET) && (timeout > 0)) {
        timeout--;
    }
    exmc_sdram_command_config(&sdram_command_init_struct);

    /* step 4: 等待 10ms（600MHz 下约 100000 循环/ms） */
    {
        volatile uint32_t i;
        for (i = 0; i < 6000000U; i++) {
        }
    }

    /* step 5: precharge all */
    sdram_command_init_struct.command = EXMC_SDRAM_PRECHARGE_ALL;
    timeout = SDRAM_TIMEOUT;
    while ((exmc_flag_get(EXMC_SDRAM_DEVICE0, EXMC_SDRAM_FLAG_NREADY) != RESET) && (timeout > 0)) {
        timeout--;
    }
    exmc_sdram_command_config(&sdram_command_init_struct);

    /* step 6: auto-refresh x8 */
    sdram_command_init_struct.command             = EXMC_SDRAM_AUTO_REFRESH;
    sdram_command_init_struct.auto_refresh_number = EXMC_SDRAM_AUTO_REFLESH_8_SDCLK;
    timeout = SDRAM_TIMEOUT;
    while ((exmc_flag_get(EXMC_SDRAM_DEVICE0, EXMC_SDRAM_FLAG_NREADY) != RESET) && (timeout > 0)) {
        timeout--;
    }
    exmc_sdram_command_config(&sdram_command_init_struct);

    /* step 7: load mode register (burst=1, sequential, CL3, single write) */
    command_content = (uint32_t)SDRAM_MODEREG_BURST_LENGTH_1        |
                      SDRAM_MODEREG_BURST_TYPE_SEQUENTIAL   |
                      SDRAM_MODEREG_CAS_LATENCY_3           |
                      SDRAM_MODEREG_OPERATING_MODE_STANDARD |
                      SDRAM_MODEREG_WRITEBURST_MODE_SINGLE;
    sdram_command_init_struct.command              = EXMC_SDRAM_LOAD_MODE_REGISTER;
    sdram_command_init_struct.auto_refresh_number  = EXMC_SDRAM_AUTO_REFLESH_1_SDCLK;
    sdram_command_init_struct.mode_register_content = command_content;
    timeout = SDRAM_TIMEOUT;
    while ((exmc_flag_get(EXMC_SDRAM_DEVICE0, EXMC_SDRAM_FLAG_NREADY) != RESET) && (timeout > 0)) {
        timeout--;
    }
    exmc_sdram_command_config(&sdram_command_init_struct);
  
    /* step 8: 刷新计数 1542 (64ms/8192, SDCLK=CK_EXMC/3) */
    exmc_sdram_refresh_count_set(1542);
    timeout = SDRAM_TIMEOUT;
    while ((exmc_flag_get(EXMC_SDRAM_DEVICE0, EXMC_SDRAM_FLAG_NREADY) != RESET) && (timeout > 0)) {
        timeout--;
    }

    /* 上电自检: 写一个值读回来 */
    probe = (volatile rt_uint32_t *)SDRAM_DEVICE0_ADDR;
    *probe = 0x5A5A1234UL;
    if (*probe == 0x5A5A1234UL) {
        LOG_I("SDRAM init ok, 32MB @ 0xC0000000");
        return RT_EOK;
    }
    LOG_E("SDRAM probe failed: wrote 0x5A5A1234, read 0x%08lx (check JP40=(1,2))", *probe);
    return -RT_ERROR;
}
INIT_BOARD_EXPORT(h7_sdram_init);

#endif /* BSP_USING_SDRAM && SOC_SERIES_GD32H7xx */