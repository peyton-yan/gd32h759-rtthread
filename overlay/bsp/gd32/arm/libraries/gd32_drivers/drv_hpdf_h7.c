/*
 * drv_hpdf_h7.c - GD32H7xx HPDF 驱动（双数字麦克风 -> PCM）
 *
 * 硬件事实（EVAL 原理图 §4.10 实锤）：
 *   两颗 MP34DT06J PDM 麦克风（U11 左 / U12 右），共用时钟与数据线：
 *   HPDF_CKOUT = PB0（AF6，MCU 输出 1MHz 采样时钟）
 *   HPDF_DATA  = PB1（AF6，PDM 比特流输入）——官方拓扑；
 *                备胎 PC3（AF3，JP7-35），开 BSP_HPDF_DATA_ON_PC3 启用。
 *                （本板 PB1 走线断已终审：官方 demo 同样无声。备胎与 I2S1_SD 互斥。）
 *   跳线 JP55 / JP56 拨 HPDF 侧（2,3）。
 *
 * 链路（参数全部对齐官方 12_HPDF_I2S_Audio demo）：
 *   PER 时钟源选 HXTAL(25MHz)，CKOUT 25 分频 -> 麦克风时钟 1MHz；
 *   CH0 配"下一引脚 + 下降沿"采左声道，CH1 配"当前引脚 + 上升沿"采右声道
 *   （立体声两根麦共一根数据线的标准玩法）；
 *   SINC4 滤波 + 64 倍抽取 -> PCM 码率 = 1MHz/64 = 15.625kHz；
 *   两路各挂 DMA1 CH1/CH3（CH2 被 drv_usart 占用，换道），循环双缓冲，半满/全满中断驱动处理线程。
 *
 * 对外 API：
 *   hpdf_get_level(&left, &right)  - 最近一块的 RMS 电平（0~32767 量级）
 *   hpdf_get_wave(buf, n)         - 最近的单声道波形（int16，显示用）
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_HPDF)

#define DBG_TAG             "drv.hpdf"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define PCM_BLOCK           512         /* 半缓冲长度（int32 样本数） */
#define PCM_BUF             (PCM_BLOCK * 2)
#define WAVE_HISTORY        512         /* 波形历史深度（int16 样本数） */

/* DMA 目标缓冲：32 字节对齐（cache line），中断里只做标志不做数据 */
__attribute__((aligned(32))) __attribute__((section(".dma_buffer"))) static int32_t pcm_left[PCM_BUF];
__attribute__((aligned(32))) __attribute__((section(".dma_buffer"))) static int32_t pcm_right[PCM_BUF];

static struct rt_semaphore pcm_sem;     /* 半满/全满各 release 一次 */
static volatile rt_uint32_t pcm_half;   /* 本次该处理哪一半：0 / PCM_BLOCK */

/* 对外状态（显示线程读，处理线程写） */
static volatile rt_uint16_t level_left, level_right;
static int16_t  wave_buf[WAVE_HISTORY];
static volatile rt_uint32_t wave_wr;    /* 写指针 */

/* ---------- 中断：只记账，活全丢给线程 ---------- */
void DMA1_Channel1_IRQHandler(void)  /* HPDF FLT0 -> 右声道 */
{
    rt_interrupt_enter();
    if (dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_HTF)) {
        dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_HTF);
        pcm_half = 0;
        rt_sem_release(&pcm_sem);
    }
    extern volatile rt_uint32_t dbg_irq_r; dbg_irq_r++;
    if (dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_FTF)) {
        dma_flag_clear(DMA1, DMA_CH1, DMA_FLAG_FTF);
        pcm_half = PCM_BLOCK;
        rt_sem_release(&pcm_sem);
    }
    rt_interrupt_leave();
}

void DMA1_Channel3_IRQHandler(void)  /* HPDF FLT1 -> 左声道 */
{
    rt_interrupt_enter();
    if (dma_flag_get(DMA1, DMA_CH3, DMA_FLAG_HTF)) {
        dma_flag_clear(DMA1, DMA_CH3, DMA_FLAG_HTF);
        pcm_half = 0;
        rt_sem_release(&pcm_sem);
    }
    extern volatile rt_uint32_t dbg_irq_l; dbg_irq_l++;
    if (dma_flag_get(DMA1, DMA_CH3, DMA_FLAG_FTF)) {
        dma_flag_clear(DMA1, DMA_CH3, DMA_FLAG_FTF);
        pcm_half = PCM_BLOCK;
        rt_sem_release(&pcm_sem);
    }
    rt_interrupt_leave();
}

/* 64 位整数开方（牛顿迭代，~6 次收敛） */
static rt_uint32_t isqrt_u64(rt_uint64_t v)
{
    rt_uint64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (rt_uint32_t)x;
}

/* ---------- 处理线程：RMS 电平 + 波形降采样 + 耳机直通 ---------- */
#ifdef BSP_USING_I2S1
/* 播放通路（drv_i2s_h7.c）：交错立体声 int16，写进环形缓冲由 I2S1 中断消费 */
extern rt_uint32_t i2s_play_write(const int16_t *stereo, rt_uint32_t frames);
static int16_t play_buf[PCM_BLOCK * 2];
#endif

static void hpdf_process_entry(void *parameter)
{
    for (;;) {
        rt_sem_take(&pcm_sem, RT_WAITING_FOREVER);

        rt_uint32_t base = pcm_half;    /* 本块起点 */
        rt_uint64_t sum_l = 0, sum_r = 0;
        int i;

        /* D-cache：DMA 写过的区域，CPU 读前 invalidate */
        SCB_InvalidateDCache_by_Addr((uint32_t *)&pcm_left[base],  PCM_BLOCK * 4);
        SCB_InvalidateDCache_by_Addr((uint32_t *)&pcm_right[base], PCM_BLOCK * 4);

        for (i = 0; i < PCM_BLOCK; i++) {
            int32_t l = pcm_left[base + i] >> 9;    /* demo 的缩放：>>9 到 int16 量级 */
            int32_t r = pcm_right[base + i] >> 9;
            if (l > 32767) l = 32767; if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; if (r < -32768) r = -32768;
            sum_l += (int32_t)l * l;
            sum_r += (int32_t)r * r;

            /* 波形：每 PCM_BLOCK/WAVE_HISTORY 组抽 1 点（这里等长，逐点存） */
            if (i < WAVE_HISTORY) {
                wave_buf[wave_wr % WAVE_HISTORY] = (int16_t)((l + r) / 2);
                wave_wr++;
            }

#ifdef BSP_USING_I2S1
            /* 耳机直通：偶数下标 = 左，奇数下标 = 右（对齐官方 demo 交错序）。
             * 采样率 15.625kHz 对 I2S 16kHz，差 2.4%，长期听会有轻微变调，
             * 但 ring 缓冲靠"满则丢帧"吸收速率差，链路本身不受影响。 */
            play_buf[2 * i]     = (int16_t)l;
            play_buf[2 * i + 1] = (int16_t)r;
#endif
        }
        /* RMS = sqrt(mean(x^2))，整数开方用 rt 库没有就用牛顿迭代 */
        level_left  = (rt_uint16_t)isqrt_u64(sum_l / PCM_BLOCK);
        level_right = (rt_uint16_t)isqrt_u64(sum_r / PCM_BLOCK);

#ifdef BSP_USING_I2S1
        i2s_play_write(play_buf, PCM_BLOCK);
#endif
    }
}

/* ---------- 对外 API ---------- */
void hpdf_get_level(rt_uint16_t *left, rt_uint16_t *right)
{
    if (left)  *left  = level_left;
    if (right) *right = level_right;
}

/* 取最近 n 点波形（时间顺序，老的在前） */
void hpdf_get_wave(int16_t *buf, rt_uint32_t n)
{
    rt_uint32_t end = wave_wr, i;
    rt_base_t lvl = rt_hw_interrupt_disable();
    if (n > WAVE_HISTORY) n = WAVE_HISTORY;
    for (i = 0; i < n; i++)
        buf[i] = wave_buf[(end - n + i + WAVE_HISTORY * 2) % WAVE_HISTORY];
    rt_hw_interrupt_enable(lvl);
}

/* ---------- 初始化 ---------- */
static void hpdf_gpio_dma_config(void)
{
    dma_single_data_parameter_struct dma_para;

    /* GPIO：PB0=CKOUT(AF6) 不变。
     * 数据线二选一（Kconfig: BSP_HPDF_DATA_ON_PC3）：
     *   默认 PB1=DATAIN1(AF6)——官方拓扑；
     *   备胎 PC3=DATAIN1(AF3，扩展排针 JP7-35)——本板 PB1 走线断
     *   （官方 demo 对照实验同样无声，终审成立），飞线 JP55-3->JP7-35 复活。
     *   注意备胎与 I2S1_SD(PC3) 互斥：开备胎时必须关 BSP_USING_I2S1。 */
    rcu_periph_clock_enable(RCU_GPIOB);
    gpio_af_set(GPIOB, GPIO_AF_6, GPIO_PIN_0);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_0);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_0);
#ifdef BSP_HPDF_DATA_ON_PC3
    rcu_periph_clock_enable(RCU_GPIOC);
    gpio_af_set(GPIOC, GPIO_AF_3, GPIO_PIN_3);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_3);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_3);
#else
    gpio_af_set(GPIOB, GPIO_AF_6, GPIO_PIN_1);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_1);
    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_1);
#endif

    /* 时钟：HPDF 内核选 APB2。
     * 注意：CKOUT 分频配置不在这里写——hpdf_module_config 开头的 hpdf_deinit()
     * 是外设级复位，会抹掉一切配置，CKOUT 必须在 deinit 之后配（官方 demo 同款顺序） */
    rcu_hpdf_clock_config(RCU_HPDFSRC_APB2);
    rcu_periph_clock_enable(RCU_HPDF);

    /* DMA：FLT0->CH1 右声道，FLT1->CH3 左声道（demo 用 CH2，与 drv_usart 冲突，换道） */
    rcu_periph_clock_enable(RCU_DMA1);
    rcu_periph_clock_enable(RCU_DMAMUX);

    dma_single_data_para_struct_init(&dma_para);
    dma_para.periph_inc          = DMA_PERIPH_INCREASE_DISABLE;
    dma_para.memory_inc          = DMA_MEMORY_INCREASE_ENABLE;
    dma_para.periph_memory_width = DMA_PERIPH_WIDTH_32BIT;
    dma_para.circular_mode       = DMA_CIRCULAR_MODE_ENABLE;
    dma_para.direction           = DMA_PERIPH_TO_MEMORY;
    dma_para.number              = PCM_BUF;
    dma_para.priority            = DMA_PRIORITY_ULTRA_HIGH;

    dma_deinit(DMA1, DMA_CH1);
    dma_para.request      = DMA_REQUEST_HPDF_FLT0;
    dma_para.periph_addr  = (uint32_t)&HPDF_FLTYRDATA(FLT0);
    dma_para.memory0_addr = (uint32_t)pcm_right;
    dma_single_data_mode_init(DMA1, DMA_CH1, &dma_para);
    dma_interrupt_enable(DMA1, DMA_CH1, DMA_INT_HTF);
    dma_interrupt_enable(DMA1, DMA_CH1, DMA_INT_FTF);
    dma_channel_enable(DMA1, DMA_CH1);

    dma_deinit(DMA1, DMA_CH3);
    dma_para.request      = DMA_REQUEST_HPDF_FLT1;
    dma_para.periph_addr  = (uint32_t)&HPDF_FLTYRDATA(FLT1);
    dma_para.memory0_addr = (uint32_t)pcm_left;
    dma_single_data_mode_init(DMA1, DMA_CH3, &dma_para);
    dma_interrupt_enable(DMA1, DMA_CH3, DMA_INT_HTF);
    dma_interrupt_enable(DMA1, DMA_CH3, DMA_INT_FTF);
    dma_channel_enable(DMA1, DMA_CH3);

    nvic_irq_enable(DMA1_Channel1_IRQn, 1, 0);
    nvic_irq_enable(DMA1_Channel3_IRQn, 1, 0);
}

static void hpdf_module_config(void)
{
    hpdf_channel_parameter_struct ch;
    hpdf_filter_parameter_struct  flt;
    hpdf_rc_parameter_struct      rc;

    hpdf_deinit();

    /* 串行时钟输出 = PER(HXTAL 25M)/25 = 1MHz。
     * 必须在 hpdf_deinit() 之后配，否则被复位抹掉（本案根因） */
    rcu_sai_clock_config(IDX_SAI0, RCU_SAISRC_PER);
    rcu_per_clock_config(RCU_PERSRC_HXTAL);
    hpdf_clock_output_config(SERIAL_AUDIO_CLK, 25, CKOUTDM_ENABLE);

    hpdf_channel_struct_para_init(&ch);
    hpdf_filter_struct_para_init(&flt);
    hpdf_rc_struct_para_init(&rc);

    /* CH0：下一引脚（PB1）、下降沿 -> 右声道 */
    ch.data_packing_mode    = DPM_STANDARD_MODE;
    ch.channel_pin_select   = CHPINSEL_NEXT;
    ch.ck_loss_detector     = CLK_LOSS_DISABLE;
    ch.malfunction_monitor  = MM_ENABLE;
    ch.spi_ck_source        = INTERNAL_CKOUT;
    ch.channel_multiplexer  = SERIAL_INPUT;
    ch.serial_interface     = SPI_FALLING_EDGE;
    ch.calibration_offset   = 0;
    ch.right_bit_shift      = 0x06;
    ch.tm_filter            = TM_FASTSINC;
    ch.tm_filter_oversample = TM_FLT_BYPASS;
    ch.mm_break_signal      = DISABLE;
    ch.mm_counter_threshold = 255;
    ch.plsk_value           = 0;
    hpdf_channel_init(CHANNEL0, &ch);

    /* CH1：当前引脚、上升沿 -> 左声道 */
    ch.channel_pin_select   = CHPINSEL_CURRENT;
    ch.serial_interface     = SPI_RISING_EDGE;
    hpdf_channel_init(CHANNEL1, &ch);

    /* SINC4 + 64 倍抽取：PCM = 1MHz/64 = 15.625kHz */
    flt.sinc_filter           = FLT_SINC4;
    flt.sinc_oversample       = 64;
    flt.integrator_oversample = INTEGRATOR_BYPASS;
    hpdf_filter_init(FLT0, &flt);
    hpdf_filter_init(FLT1, &flt);

    /* 规则转换：连续 + DMA */
    rc.fast_mode       = FAST_ENABLE;
    rc.rcdmaen         = RCDMAEN_ENABLE;
    rc.continuous_mode = RCCM_ENABLE;
    rc.rcs_channel     = RCS_CHANNEL0;
    hpdf_rc_init(FLT0, &rc);
    rc.rcs_channel     = RCS_CHANNEL1;
    hpdf_rc_init(FLT1, &rc);

    hpdf_channel_enable(CHANNEL0);
    hpdf_channel_enable(CHANNEL1);
    hpdf_filter_enable(FLT0);
    hpdf_filter_enable(FLT1);
    hpdf_enable();
    hpdf_rc_start_by_software(FLT0);
    hpdf_rc_start_by_software(FLT1);
}

int rt_hw_hpdf_init(void)
{
    rt_thread_t tid;

    rt_sem_init(&pcm_sem, "hpdf", 0, RT_IPC_FLAG_PRIO);
    hpdf_gpio_dma_config();
    hpdf_module_config();

    tid = rt_thread_create("hpdf", hpdf_process_entry, RT_NULL,
                           2048, RT_THREAD_PRIORITY_MAX / 3, 10);
    if (tid) rt_thread_startup(tid);

    LOG_I("hpdf ready: dual MP34DT06J, 1MHz PDM, 15.6kHz PCM");
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_hpdf_init);

/* ---------- 测试命令 ---------- */
#ifdef RT_USING_FINSH
#include <finsh.h>

/* 实时电平条：对着麦克风说话/敲击，看电平跳动 */
static void hpdf_test(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 10;
    int i;

    rt_kprintf("hpdf_test: %ds, speak or tap the mics...\n", secs);
    for (i = 0; i < secs * 5; i++) {
        rt_uint16_t l, r;
        int nb;
        hpdf_get_level(&l, &r);
        nb = l / 1024; if (nb > 31) nb = 31;
        rt_kprintf("L[%5u] %.*s | R[%5u]\r", l, nb,
                   "###############################", r);
        rt_thread_mdelay(200);
    }
    rt_kprintf("\n");
}
MSH_CMD_EXPORT(hpdf_test, hpdf_test [sec] - live mic level meter);
#endif /* RT_USING_FINSH */


/* ================= hpdf_dbg：取证相机 =================
 * 恒零两种剧本：A. 中断没来（线程睡死）；B. 中断在来，但 HPDF 吐出全零。
 * 一眼区分：irq_cnt 涨不涨、DMA 剩余计数动不动、RDATA 是不是零。 */
volatile rt_uint32_t dbg_irq_r, dbg_irq_l;          /* 在 IRQHandler 里 ++（hpdf_dbg 读） */

static void hpdf_dbg(int argc, char **argv)
{
    rt_kprintf("irq_cnt: FLT0(右,CH1)=%lu  FLT1(左,CH3)=%lu\n", dbg_irq_r, dbg_irq_l);
    rt_kprintf("wave_wr=%lu pcm_half=%lu\n", wave_wr, pcm_half);
    rt_kprintf("DMA 剩余计数: CH1=%lu CH3=%lu (满=%d，不转=一直满)\n",
               dma_transfer_number_get(DMA1, DMA_CH1),
               dma_transfer_number_get(DMA1, DMA_CH3), PCM_BUF);
    rt_kprintf("DMA 标志: CH1 HTF=%d FTF=%d | CH3 HTF=%d FTF=%d\n",
               dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_HTF),
               dma_flag_get(DMA1, DMA_CH1, DMA_FLAG_FTF),
               dma_flag_get(DMA1, DMA_CH3, DMA_FLAG_HTF),
               dma_flag_get(DMA1, DMA_CH3, DMA_FLAG_FTF));
    rt_kprintf("pcm_right[0..3]=%ld %ld %ld %ld\n",
               pcm_right[0], pcm_right[1], pcm_right[2], pcm_right[3]);
    rt_kprintf("pcm_left[0..3] =%ld %ld %ld %ld\n",
               pcm_left[0], pcm_left[1], pcm_left[2], pcm_left[3]);
    rt_kprintf("RDATA 直读: FLT0=%ld FLT1=%ld\n",
               (int32_t)HPDF_FLTYRDATA(FLT0), (int32_t)HPDF_FLTYRDATA(FLT1));

    /* ISTAT 数边沿法测 CKOUT：PB0 处于 AF 模式时 GPIOB_ISTAT 仍反映电平。
     * 100ms 内应数到 ~100000 个上升沿（1MHz） */
    {
        rt_tick_t start = rt_tick_get();
        rt_uint32_t prev = GPIO_ISTAT(GPIOB) & GPIO_PIN_0, edges = 0;
        while (rt_tick_get() - start < 100) {
            rt_uint32_t n;
            for (n = 0; n < 256; n++) {
                rt_uint32_t cur = GPIO_ISTAT(GPIOB) & GPIO_PIN_0;
                if (cur && !prev) edges++;
                prev = cur;
            }
        }
        rt_kprintf("CKOUT(PB0) 边沿数=%lu/100ms -> ~%lu Hz (期望 1000000)\n",
                   edges, edges * 10);
    }
    /* 数据线边沿计数：和 PB0 同款 ISTAT 数边沿法。
     * 麦克风在驱动数据线时，PDM 流的翻转率最高可达时钟频率（1MHz），
     * 安静环境下也有大量跳变；恒电平则边沿数≈0。
     * 判据：边沿≈0 -> 信号没进数据脚（通路/接触问题）；
     *       边沿几十万 但 RDATA 仍满幅 -> 信号已进脚，是 HPDF 配置问题。
     * 数据脚随 BSP_HPDF_DATA_ON_PC3 切换：默认 PB1，备胎 PC3(JP7-35)。 */
#ifdef BSP_HPDF_DATA_ON_PC3
#define DATA_GPIO   GPIOC
#define DATA_PIN    GPIO_PIN_3
#define DATA_NAME   "PC3(JP7-35)"
#else
#define DATA_GPIO   GPIOB
#define DATA_PIN    GPIO_PIN_1
#define DATA_NAME   "PB1"
#endif
    {
        rt_tick_t start = rt_tick_get();
        rt_uint32_t prev = GPIO_ISTAT(DATA_GPIO) & DATA_PIN, edges = 0;
        while (rt_tick_get() - start < 100) {
            rt_uint32_t n;
            for (n = 0; n < 256; n++) {
                rt_uint32_t cur = GPIO_ISTAT(DATA_GPIO) & DATA_PIN;
                if (cur && !prev) edges++;
                prev = cur;
            }
        }
        rt_kprintf("DATA(%s) 边沿数=%lu/100ms (麦克风在说话=数万以上，死寂=≈0)\n",
                   DATA_NAME, edges);
    }
    rt_kprintf("DATA(%s) 当前电平=%d (单次采样，仅供参考)\n",
               DATA_NAME, gpio_input_bit_get(DATA_GPIO, DATA_PIN) == SET ? 1 : 0);

    /* GPIO 配置回读：PB0=AF6；数据脚 PB1=AF6 或 PC3=AF3。
     * 期望：GPIOB CTL&0xF=0xA、AFSEL0 低 8 位=0x66（PB1 模式）；
     *       GPIOC AFSEL0[15:12]=0x3（PC3 备胎模式）。 */
    rt_kprintf("GPIOB: CTL=0x%08lX AFSEL0=0x%08lX | GPIOC: CTL=0x%08lX AFSEL0=0x%08lX\n",
               GPIO_CTL(GPIOB), GPIO_AFSEL0(GPIOB),
               GPIO_CTL(GPIOC), GPIO_AFSEL0(GPIOC));
}
MSH_CMD_EXPORT(hpdf_dbg, dump hpdf/dma internals);

/* pin_scope <A..H> <pin>：通用接收探针——把引脚设为浮空输入，数 100ms 内
 * 上升沿个数并打印折算频率。万用表只能看平均电压，这玩意是"穷人示波器"。
 * 用法示例：杜邦线把待测点引到某个扩展排针脚，然后 pin_scope 该脚。 */
static void pin_scope(int argc, char **argv)
{
    static const uint32_t ports[] = { GPIOA, GPIOB, GPIOC, GPIOD,
                                      GPIOE, GPIOF, GPIOG, GPIOH };
    static const uint32_t rcus[]  = { RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD,
                                      RCU_GPIOE, RCU_GPIOF, RCU_GPIOG, RCU_GPIOH };
    char port = (argc > 1) ? argv[1][0] : 'C';
    int pin = (argc > 2) ? atoi(argv[2]) : 3;
    int idx = port - 'A';
    uint32_t g, pmask;
    rt_tick_t start;
    rt_uint32_t prev, edges = 0;

    if (idx < 0 || idx > 7 || pin < 0 || pin > 15) {
        rt_kprintf("usage: pin_scope <A..H> <0..15>\n");
        return;
    }
    g = ports[idx]; pmask = 1U << pin;
    rcu_periph_clock_enable(rcus[idx]);
    gpio_mode_set(g, GPIO_MODE_INPUT, GPIO_PUPD_NONE, pmask);

    prev = GPIO_ISTAT(g) & pmask;
    start = rt_tick_get();
    while (rt_tick_get() - start < 100) {
        rt_uint32_t n;
        for (n = 0; n < 256; n++) {
            rt_uint32_t cur = GPIO_ISTAT(g) & pmask;
            if (cur && !prev) edges++;
            prev = cur;
        }
    }
    rt_kprintf("pin_scope: P%c%d 边沿=%lu/100ms -> ~%lu Hz, 当前电平=%d\n",
               port, pin, edges, edges * 10,
               gpio_input_bit_get(g, pmask) == SET ? 1 : 0);
}
MSH_CMD_EXPORT(pin_scope, pin_scope <port> <pin> - count edges on input pin);

/* pin_blink <A..K> <pin> [sec=10]：通用引脚探测器。
 * 把指定引脚临时改成推挽输出打 2Hz 方波，同时对比 OSTAT/ISTAT：
 *   OSTAT 翻、ISTAT 不翻 -> 焊盘被外力按住（板上其他驱动器/跳线抢线）；
 *   OSTAT/ISTAT 都翻，但排针量不到 -> 引脚到排针的走线断/孔位错；
 *   排针能量到 0<->3.3V -> 一切正常。
 * 警告：仅用于空闲引脚！总线上跑着的引脚会被抢权（测试完恢复 AF 需重配外设）。 */
static void pin_blink(int argc, char **argv)
{
    /* BGA176 实有端口 A~H（J/K 在本封装不存在或极少用，需要时再加） */
    static const uint32_t ports[] = { GPIOA, GPIOB, GPIOC, GPIOD,
                                      GPIOE, GPIOF, GPIOG, GPIOH };
    static const uint32_t rcus[]  = { RCU_GPIOA, RCU_GPIOB, RCU_GPIOC, RCU_GPIOD,
                                      RCU_GPIOE, RCU_GPIOF, RCU_GPIOG, RCU_GPIOH };
    char port = (argc > 1) ? argv[1][0] : 'C';
    int pin = (argc > 2) ? atoi(argv[2]) : 3;
    int sec = (argc > 3) ? atoi(argv[3]) : 10;
    int idx = port - 'A', i;
    uint32_t g, pmask;
    rt_uint32_t mism = 0;

    if (idx < 0 || idx > 7 || pin < 0 || pin > 15) {
        rt_kprintf("usage: pin_blink <A..H> <0..15> [sec]\n");
        return;
    }
    g = ports[idx]; pmask = 1U << pin;
    rcu_periph_clock_enable(rcus[idx]);

    gpio_mode_set(g, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, pmask);
    gpio_output_options_set(g, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, pmask);
    rt_kprintf("pin_blink: P%c%d 输出 2Hz %ds, CTL=0x%08lX\n",
               port, pin, sec, GPIO_CTL(g));
    for (i = 0; i < sec * 2; i++) {
        rt_uint32_t want = (i & 1) ? pmask : 0;
        gpio_bit_write(g, pmask, (i & 1) ? SET : RESET);
        rt_thread_mdelay(400);                  /* 留时间给电平稳定/测量 */
        if (!!(GPIO_ISTAT(g) & pmask) != !!want) mism++;
        rt_thread_mdelay(100);
    }
    rt_kprintf("pin_blink: 翻转 %d 次，ISTAT 不跟 %d 次 %s\n",
               sec * 2, mism,
               mism == 0 ? "(焊盘自由，去量排针)" : "(焊盘被外力按住！查跳线/其他驱动器)");
}
MSH_CMD_EXPORT(pin_blink, pin_blink <port> <pin> [sec] - GPIO probe w/ OSTAT vs ISTAT);

/* pc3_blink [sec=10]：HPDF 备胎专用，测完自动恢复 PC3=AF3 */
#ifdef BSP_HPDF_DATA_ON_PC3
static void pc3_blink(int argc, char **argv)
{
    char *args[] = { "pin_blink", "C", "3",
                     (argc > 1) ? argv[1] : "10" };
    pin_blink(4, args);
    gpio_af_set(GPIOC, GPIO_AF_3, GPIO_PIN_3);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_3);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_60MHZ, GPIO_PIN_3);
    rt_kprintf("pc3_blink: 已恢复 PC3=AF3(HPDF_DATAIN1)\n");
}
MSH_CMD_EXPORT(pc3_blink, PC3 drives 2Hz square then restore AF3);
#endif

#endif /* BSP_USING_HPDF */