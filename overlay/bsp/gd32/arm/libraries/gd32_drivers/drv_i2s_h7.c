/*
 * drv_i2s_h7.c - GD32H7xx I2S 播放驱动（CS4344 DAC -> J2 耳机座）
 *
 * 链路：I2S1(SPI1 的 I2S 功能) -> CS4344(U13, 纯数据 DAC，无控制接口) -> J2。
 * 引脚：MCK=PC6 / CK=PD3 / WS=PB9 / SD=PC3，均 AF5。
 * 跳线前提：JP54(2,3)=I2S1_WS、JP58(2,3)=I2S1_CK、JP63=I2S1_MCK、
 *          JP40(SDRAM 页)=I2S1_SD 档。
 *
 * 喂数方式照抄官方 12_HPDF_I2S_Audio demo（已在本板验证）：
 *   16kHz / 16bit 立体声 / MCK 输出开 / 主机发送 / MSB 标准 / CKPL 高，
 *   内核时钟选 PLL0Q，TP（发送 FIFO 有空位）中断逐帧喂数。
 *   32k 中断/秒 @600MHz 可承受；将来要省 CPU 再换 DMA 循环（博客素材）。
 *
 * 用法：i2s_play_write(stereo_int16, frames) 往环形缓冲推立体声帧；
 *       缓冲区空时硬件侧喂 0（静音），不怕欠载。
 *       msh 命令 i2s_beep [ms] [hz]：正弦波试音，不依赖麦克风，
 *       单独验证 "I2S -> DAC -> 耳机" 半条链路。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <math.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_I2S1)

#define DBG_TAG             "drv.i2s"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define I2S_SAMPLE_RATE     I2S_AUDIOSAMPLE_16K
#define PLAY_RING_FRAMES    8192            /* 立体声帧数，~512ms @16kHz，吸调度抖动 */

/* 环形缓冲：int16 交错 LRLR...。ISR 只读 play_rd，线程只写 play_wr，
 * 单产单消无锁。 */
static int16_t  play_ring[PLAY_RING_FRAMES * 2];
static volatile rt_uint32_t play_rd;
static rt_uint32_t play_wr;
static volatile rt_bool_t  play_enable = RT_TRUE;

/* 统计（i2s_dbg 用） */
static volatile rt_uint32_t dbg_frames_out, dbg_underrun;

/* ---------- TP 中断：发送 FIFO 有空位就喂一帧 ---------- */
void SPI1_IRQHandler(void)
{
    rt_interrupt_enter();
    if (SET == spi_i2s_interrupt_flag_get(SPI1, SPI_I2S_INT_FLAG_TP)) {
        int16_t v = 0;                          /* 欠载喂静音 */
        if (play_enable && play_rd != play_wr) {
            v = play_ring[play_rd];
            play_rd = (play_rd + 1) % (PLAY_RING_FRAMES * 2);
            dbg_frames_out++;
        } else {
            dbg_underrun++;
        }
        spi_i2s_data_transmit(SPI1, (uint16_t)v);
    }
    rt_interrupt_leave();
}

/* ---------- 生产者接口：往环形缓冲推 frames 个立体声帧（交错 LRLR） ----------
 * 返回实际写入帧数；满了就丢（采集/播放速率 15.625k vs 16k 的微小失配
 * 靠这个吸收）。 */
rt_uint32_t i2s_play_write(const int16_t *stereo, rt_uint32_t frames)
{
    rt_uint32_t i;
    for (i = 0; i < frames; i++) {
        rt_uint32_t next = (play_wr + 2) % (PLAY_RING_FRAMES * 2);
        if (next == play_rd) break;             /* 满：丢帧保实时 */
        play_ring[play_wr] = stereo[i * 2];
        play_ring[(play_wr + 1) % (PLAY_RING_FRAMES * 2)] = stereo[i * 2 + 1];
        play_wr = next;
    }
    return i;
}

void i2s_play_enable(rt_bool_t en) { play_enable = en; }

/* ---------- 初始化 ---------- */
static void i2s_gpio_config(void)
{
    /* I2S1_MCK=PC6 / I2S1_SD=PC3 / I2S1_WS=PB9 / I2S1_CK=PD3，全 AF5 */
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);

    gpio_af_set(GPIOC, GPIO_AF_5, GPIO_PIN_6 | GPIO_PIN_3);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_6 | GPIO_PIN_3);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_6 | GPIO_PIN_3);
    gpio_af_set(GPIOB, GPIO_AF_5, GPIO_PIN_9);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_9);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_9);
    gpio_af_set(GPIOD, GPIO_AF_5, GPIO_PIN_3);
    gpio_mode_set(GPIOD, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_3);
    gpio_output_options_set(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_3);
}

static int rt_hw_i2s_init(void)
{
    i2s_gpio_config();

    /* 内核时钟：I2S1(SPI1) 选 PLL0Q（官方 demo 同款）。
     * 打印出来防"PLL0Q 没开"的暗病——为 0 就是系统时钟树没配 */
    rcu_periph_clock_enable(RCU_SPI1);
    rcu_spi_clock_config(IDX_SPI1, RCU_SPISRC_PLL0Q);

    spi_i2s_deinit(SPI1);
    i2s_psc_config(SPI1, I2S_SAMPLE_RATE, I2S_FRAMEFORMAT_DT16B_CH16B,
                   I2S_MCKOUT_ENABLE);
    i2s_init(SPI1, I2S_MODE_MASTERTX, I2S_STD_MSB, I2S_CKPL_HIGH);
    i2s_enable(SPI1);
    spi_master_transfer_start(SPI1, SPI_TRANS_START);

    nvic_irq_enable(SPI1_IRQn, 2, 0);
    spi_i2s_interrupt_enable(SPI1, SPI_I2S_INT_TP);

    LOG_I("i2s1(CS4344->J2) ready: 16kHz stereo, PLL0Q=%lu Hz",
          rcu_clock_freq_get(CK_PLL0Q));
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_i2s_init);

/* ---------- msh 命令 ---------- */
#ifdef RT_USING_FINSH
#include <finsh.h>

/* 正弦试音：i2s_beep [ms=1000] [hz=1000]。验证 I2S->DAC->耳机 通路，
 * 不依赖麦克风——先确认这条半链路是通的，再接拾音。 */
static void i2s_beep(int argc, char **argv)
{
    rt_uint32_t ms = (argc > 1) ? (rt_uint32_t)atoi(argv[1]) : 1000;
    rt_uint32_t hz = (argc > 2) ? (rt_uint32_t)atoi(argv[2]) : 1000;
    rt_uint32_t total = 16 * ms, done = 0;      /* 16 帧/ms @16kHz */
    int16_t buf[320];                           /* 160 帧/批 = 10ms 音频 */
    float phase = 0.0f, step = 6.2831853f * hz / 16000.0f;
    /* 取证：本次 beep 期间的欠载增量。增量>0 = 生产端断粮（听感上的截断） */
    rt_uint32_t underrun0 = dbg_underrun, frames0 = dbg_frames_out;

    while (done < total) {
        int i;
        for (i = 0; i < 160 && done < total; i++, done++) {
            int16_t s = (int16_t)(sinf(phase) * 12000.0f);
            phase += step;
            buf[i * 2] = s; buf[i * 2 + 1] = s;
        }
        i2s_play_write(buf, i);
        rt_thread_mdelay(5);                    /* 5ms 补一次货，生产速率 20帧/ms > 消费 16帧/ms */
    }
    rt_kprintf("i2s_beep: %lu ms @ %lu Hz done, played=%lu frames, underrun +%lu during beep\n",
               ms, hz, dbg_frames_out - frames0, dbg_underrun - underrun0);
}
MSH_CMD_EXPORT(i2s_beep, i2s_beep [ms] [hz] - sine test tone to J2);

/* ISTAT 数边沿法测时钟：引脚处于 AF 模式时 GPIO_ISTAT 仍反映真实电平。
 * 100ms 窗口。hpdf_dbg 同款，仪器自由。 */
static rt_uint32_t count_edges(uint32_t gpio, uint32_t pin)
{
    rt_tick_t start = rt_tick_get();
    rt_uint32_t prev = GPIO_ISTAT(gpio) & pin, edges = 0;
    while (rt_tick_get() - start < 100) {
        rt_uint32_t n;
        for (n = 0; n < 256; n++) {
            rt_uint32_t cur = GPIO_ISTAT(gpio) & pin;
            if (cur && !prev) edges++;
            prev = cur;
        }
    }
    return edges * 10;  /* -> Hz */
}

static void i2s_dbg(int argc, char **argv)
{
    rt_kprintf("i2s: frames_out=%lu underrun=%lu ring_used=%ld/%d frames\n",
               dbg_frames_out, dbg_underrun,
               ((rt_int32_t)(play_wr - play_rd + PLAY_RING_FRAMES * 2)
                 % (PLAY_RING_FRAMES * 2)) / 2, PLAY_RING_FRAMES);
    /* H7 无独立 I2SPSC 寄存器：DIV/OF/MCKOEN 都长在 I2SCTL 里，顺带解码 */
    rt_kprintf("i2s: SPI1 CTL0=0x%08lx I2SCTL=0x%08lx STAT=0x%08lx (DIV=%lu OF=%lu MCKOEN=%lu)\n",
               SPI_CTL0(SPI1), SPI_I2SCTL(SPI1), SPI_STAT(SPI1),
               (SPI_I2SCTL(SPI1) & SPI_I2SCTL_DIV) >> 16,
               (SPI_I2SCTL(SPI1) & SPI_I2SCTL_OF) >> 24,
               (SPI_I2SCTL(SPI1) & SPI_I2SCTL_MCKOEN) >> 25);
    /* 时钟实测（16kHz 立体声理论值）：
     * 全在动 = MCU 侧无罪，查跳线/DAC；MCK=0 = PLL0Q 或 MCKOEN 问题 */
    rt_kprintf("i2s: MCK(PC6)=%lu Hz (期望~4.1M)  CK(PD3)=%lu Hz (期望~512k)  WS(PB9)=%lu Hz (期望16k)\n",
               count_edges(GPIOC, GPIO_PIN_6),
               count_edges(GPIOD, GPIO_PIN_3),
               count_edges(GPIOB, GPIO_PIN_9));
    /* GPIO 配置回读：四脚应均为 AF5 */
    rt_kprintf("i2s: GPIOC CTL=0x%08lX AFSEL0=0x%08lX (PC6/PC3 期望 AF5)\n",
               GPIO_CTL(GPIOC), GPIO_AFSEL0(GPIOC));
    rt_kprintf("i2s: GPIOB AFSEL1=0x%08lX (PB9 期望 0x5)  GPIOD CTL=0x%08lX AFSEL0=0x%08lX (PD3 期望 AF5)\n",
               GPIO_AFSEL1(GPIOB), GPIO_CTL(GPIOD), GPIO_AFSEL0(GPIOD));
}
MSH_CMD_EXPORT(i2s_dbg, dump i2s playback internals);
#endif /* RT_USING_FINSH */

#endif /* BSP_USING_I2S1 */