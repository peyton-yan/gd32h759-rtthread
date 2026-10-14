/*
 * GD32H7xx SDIO host driver for RT-Thread v5.x MMCSD framework
 *
 * 设计说明：
 *   只实现 rt_mmcsd_host_ops 三件事：request / set_iocfg / get_card_status。
 *   卡片识别、CSD 解析、块设备注册交给 components/drivers/sdio 的 mmcsd
 *   框架，DFS/elm-FatFS 挂载框架生成的 "sd0"。
 *
 *   H7 的 SDIO 是新 IP（多实例、IDMA、trans_start/stop），本驱动以官方
 *   GD32H7xx Demo Suites 18_SDIO_SDCardTest 的 sdcard.c 为协议层蓝本。
 *   数据通路支持 IDMA（默认）与 FIFO polling 双模式，SDIO_USE_IDMA 切换；
 *   IDMA 由 SDIO 内置 DMA 直搬内存，不占 DMA 通道。缓冲区不满足
 *   8 字节对齐 / 32 字节整数倍 / <=8160 字节时自动回退 polling。
 *
 * 移植指南：
 *   1. 引脚：本板 CLK=PC12 CMD=PD2 D0=PB13 D1~D3=PC9/PC10/PC11，全部 AF12
 *      注意 D0 经过跳线 JP65，必须拨 (2,3)=SDIO；(1,2) 是 CAN1_TX
 *   2. 时钟：SDIOCLK 默认取自 PLL1R=200MHz（官方 demo 配置 25M/5*40）。
 *      若 SystemClock_Config 未使能 PLL1R 输出，需在 board.c 补上：
 *      rcu_pll_input_output_clock_range_config(IDX_PLL1, RCU_PLL1RNG_4M_8M,
 *          RCU_PLL1VCO_192M_836M); rcu_pll1_config(5,40,1,1,1);
 *      rcu_pll_clock_output_enable(RCU_PLL1R); rcu_osci_on(RCU_PLL1_CK);
 *      改动时钟源后同步修改 SDIO_CLK_SRC_HZ。分频为直接分频：
 *      400kHz=500, 25MHz=8, 50MHz=4（驱动按目标频率自动计算）
 *   3. 换用 SDIO1 实例时改 SDIO_PERIPH/SDIO_RCU/时钟配置三处
 *   4. 有卡检测引脚的板子在 get_card_status 里返回真实电平
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#if defined(BSP_USING_SDIO)

#include <drivers/dev_mmcsd_core.h>
#include <drivers/mmcsd_host.h>
#include <finsh.h>
#if defined(BSP_SDIO_REPROBE_CMD)
#include <dfs_fs.h>                     /* sd_reprobe 移除前主动 umount */
#endif
#include "gd32h7xx_sdio.h"
#include "drv_gpio.h"

#define DBG_TAG             "drv.sdio"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

/* ===================== 配置区 ===================== */

#define SDIO_PERIPH         SDIO0               /* 使用 SDIO0 实例 */
#define SDIO_RCU            RCU_SDIO0

/* 总线宽度由 Kconfig 决定：BSP_SDIO_1BIT=y 时 1-bit（本板开 TLI 必须，
 * PC11=SDIO_D3 与 LCD_B4 经 JP52 物理二选一，拨到 LCD 侧后 D3 不存在，
 * 4-bit 会在 ACMD6 切宽后通信全挂；1-bit 只用 CLK/CMD/D0 与 LCD 零冲突，
 * ~6MB/s@50MHz，CSV 记录足够）。自画板无此冲突时不选即恢复 4-bit */
#if defined(BSP_SDIO_1BIT)
#define SDIO_1BIT_MODE      1
#else
#define SDIO_1BIT_MODE      0
#endif

/* SDIO0 内核时钟源：PLL1R = 25MHz HXTAL /5 *40 /1 = 200MHz
 * 置 1：驱动自行配置 PLL1（board.c 未配置 PLL1 时使用，推荐——补丁自包含）
 * 置 0：系统时钟树已配置 PLL1，驱动不再重复配置 */
#define SDIO_PLL1_SELF_CONFIG   1

/* SDIO 内核时钟源频率（分频为直接分频，非 F4 的 div+2） */
#define SDIO_CLK_SRC_HZ     200000000UL

/* 数据通路选择：1=IDMA（推荐，根治 RXORE/TXURE），0=FIFO polling。
 * H7 的 IDMA 是 SDIO 外设内置 DMA，不占 DMA 通道、不用 DMAMUX。
 * polling 通路在 RTOS 中断抢占下必然溢出：25MHz/4bit=12.5MB/s，
 * FIFO 32 字仅缓冲 ~10us，任何 >2.5us 的抢占即 RXORE（实测
 * bytes=220/512）。IDMA 由硬件直搬内存，与 CPU 抢占无关。 */
#define SDIO_USE_IDMA       1

/* FIFO polling 通路的时钟上限：12MHz（div=16，FIFO 缓冲 ~21us） */
#define SDIO_POLLING_MAX_HZ 12000000UL

/* IDMA 通路的时钟上限：50MHz（div=4，4bit 理论 25MB/s） */
#define SDIO_IDMA_MAX_HZ    50000000UL

/* IDMA 约束（gd32h7xx_sdio.h 寄存器定义）：
 * - IDMASIZE 位宽 BITS(5,12)，单位 32 字节 => 单次最大 255*32=8160 字节
 * - IDMAADDR0 要求地址 8 字节对齐
 *
 * 为什么不直接用框架给的 data->buf 做 IDMA：M7 的 D-cache 维护必须按
 * 32B cache line 对齐放大，而 invalidate 会丢弃 line 内的脏数据。
 * 框架 buffer 来自 rt_malloc/栈（8B 对齐），两端与堆元数据/栈变量共享
 * cache line，invalidate 放大的区域会悄悄破坏相邻内存（实测调度路径
 * 崩溃 pc=0 INVSTATE）。因此 IDMA 只操作驱动私有的 bounce buffer，
 * 框架 buffer 用 memcpy 进出——8KB 一次的 memcpy 代价远小于 RXORE 重传。 */
#define SDIO_IDMA_MAX_BYTES (255UL * 32UL)

/* IDMA 专用 bounce buffer：放进 .dma_buffer（MPU 非cacheable 区，见 drv_dma_mem.c），
 * 驱动独占 => CPU 与 IDMA 天然一致，cache 维护退化为可选保险。
 * 请求被 host 锁串行化，一块静态 buffer 足够 */
static rt_uint8_t sdio_idma_bounce[SDIO_IDMA_MAX_BYTES]
    __attribute__((aligned(32)))
    __attribute__((section(".dma_buffer")));

/* 命令/数据超时（rt_tick 起始相对式） */
#define SDIO_CMD_TIMEOUT    rt_tick_from_millisecond(200)
#define SDIO_DAT_TIMEOUT    rt_tick_from_millisecond(2000)

/* DSM 数据超时计数（官方 demo 值） */
#define SDIO_DATATIMEOUT    0xFFFF0000UL

/* 标志集合（来自官方 demo，新 IP 位定义与 F4 不同） */
#define SDIO_MASK_CMD_FLAGS     0x002000C5UL    /* 命令通道标志清除掩码 */
#define SDIO_MASK_DATA_FLAGS    0x18000F3AUL    /* 数据通道标志清除掩码 */
#define SDIO_DAT_READ_END       (SDIO_FLAG_DTCRCERR | SDIO_FLAG_DTTMOUT | \
                                 SDIO_FLAG_RXORE | SDIO_FLAG_DTBLKEND | SDIO_FLAG_DTEND)
#define SDIO_DAT_WRITE_END      (SDIO_FLAG_DTCRCERR | SDIO_FLAG_DTTMOUT | \
                                 SDIO_FLAG_TXURE | SDIO_FLAG_DTBLKEND | SDIO_FLAG_DTEND)
#define SDIO_DAT_IDMA_END       (SDIO_FLAG_DTCRCERR | SDIO_FLAG_DTTMOUT | \
                                 SDIO_FLAG_IDMAERR | SDIO_FLAG_DTEND)
#define SD_FIFOHALF_WORDS       8               /* FIFO 半满/半空阈值（字） */

/* IDMA 前置条件：总长不超 bounce 容量即可（buffer 对齐由 bounce 兜底） */
static rt_bool_t sdio_idma_capable(const struct rt_mmcsd_data *data)
{
#if SDIO_USE_IDMA
    return (data->blksize * data->blks) <= SDIO_IDMA_MAX_BYTES;
#else
    return RT_FALSE;
#endif
}

/* bounce buffer 的 cache 维护。bounce 本身 32B 对齐且驱动独占，
 * 对齐放大不越界，invalidate/flush 都安全 */
static void sdio_bounce_cache_ops(rt_uint32_t total, rt_bool_t is_read)
{
#ifdef RT_USING_CACHE
    rt_uint32_t size = (total + 0x1FU) & ~0x1FU;

    if (is_read)
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_INVALIDATE, sdio_idma_bounce, size);
    else
        rt_hw_cpu_dcache_ops(RT_HW_CACHE_FLUSH, sdio_idma_bounce, size);
#endif
}

/* ===================== 内部工具 ===================== */

/* 等待 STAT 中任一标志位置位，rt_tick 起始相对式超时 */
static rt_err_t sdio_wait_stat(rt_uint32_t flags, rt_tick_t timeout)
{
    rt_tick_t start = rt_tick_get();

    while ((SDIO_STAT(SDIO_PERIPH) & flags) == 0)
    {
        if (rt_tick_get() - start > timeout)
            return -RT_ETIMEOUT;
    }
    return RT_EOK;
}

/* 块大小编码：512 -> DATACTL_BLKSZ(9) */
static rt_uint32_t sdio_blocksize_encode(rt_uint32_t bytes)
{
    rt_uint32_t exp = 0;
    while (bytes > 1) { bytes >>= 1; exp++; }
    return DATACTL_BLKSZ(exp);
}

/* 数据阶段等待（FIFO 轮询本体，配置在 request 中已完成），前置声明 */
static rt_err_t sdio_transfer_data_wait(struct rt_mmcsd_data *data);

/* ===================== mmcsd host ops ===================== */

static void h7_sdio_request(struct rt_mmcsd_host *host, struct rt_mmcsd_req *req)
{
    struct rt_mmcsd_cmd *cmd = req->cmd;
    rt_uint32_t resp_cfg;
    rt_uint32_t wait_flags;
    rt_bool_t no_crc;
    rt_err_t err = RT_EOK;

    /* 响应类型映射 */
    switch (cmd->flags & RESP_MASK)
    {
    case RESP_NONE:
        resp_cfg   = SDIO_RESPONSETYPE_NO;
        wait_flags = SDIO_FLAG_CMDSEND;         /* 无响应：等命令发出 */
        break;
    case RESP_R2:
        resp_cfg   = SDIO_RESPONSETYPE_LONG;
        wait_flags = SDIO_FLAG_CMDRECV | SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT;
        break;
    default:
        resp_cfg   = SDIO_RESPONSETYPE_SHORT;
        wait_flags = SDIO_FLAG_CMDRECV | SDIO_FLAG_CCRCERR | SDIO_FLAG_CMDTMOUT;
        break;
    }
    /* R3/R4 响应按协议无 CRC，硬件不校验，CCRCERR 不作错误 */
    no_crc = ((cmd->flags & RESP_MASK) == RESP_R3 ||
              (cmd->flags & RESP_MASK) == RESP_R4);

    /* 数据通路必须在命令发出前就位（读响应后数据立即到来） */
    if (req->data)
    {
        rt_uint32_t total = req->data->blksize * req->data->blks;
        rt_bool_t is_read = (req->data->flags & DATA_DIR_READ);

        /* IDMA 必须在数据通路使能前就位（官方 demo 顺序：
         * dma_config -> idma_enable -> data_config -> trans_start）。
         * 官方 dma_config 本体只有两行：sdio_idma_set + buffer0_address_set */
        if (sdio_idma_capable(req->data))
        {
            if (!is_read)
            {
                rt_memcpy(sdio_idma_bounce, req->data->buf, total);
                sdio_bounce_cache_ops(total, RT_FALSE); /* 写出前刷到内存 */
            }
            else
            {
                /* 读前 invalidate：丢弃 bounce 区域可能存在的脏行，
                 * 防止传输中途被回写覆盖 IDMA 刚写入的数据 */
                sdio_bounce_cache_ops(total, RT_TRUE);
            }
            sdio_idma_set(SDIO_PERIPH, SDIO_IDMA_SINGLE_BUFFER,
                          (total + 31) >> 5);
            sdio_idma_buffer0_address_set(SDIO_PERIPH,
                                          (rt_uint32_t)sdio_idma_bounce);
            sdio_idma_enable(SDIO_PERIPH);
        }

        sdio_data_config(SDIO_PERIPH, SDIO_DATATIMEOUT, total,
                         sdio_blocksize_encode(req->data->blksize));
        sdio_data_transfer_config(SDIO_PERIPH, SDIO_TRANSMODE_BLOCKCOUNT,
                                  is_read ? SDIO_TRANSDIRECTION_TOSDIO
                                          : SDIO_TRANSDIRECTION_TOCARD);
        sdio_trans_start_enable(SDIO_PERIPH);
    }

    /* 发命令；CMD12 需要 CSM 带停止位（sd_transfer_stop 的做法） */
    sdio_command_response_config(SDIO_PERIPH, cmd->cmd_code, cmd->arg, resp_cfg);
    sdio_wait_type_set(SDIO_PERIPH, SDIO_WAITTYPE_NO);
    if (cmd->cmd_code == STOP_TRANSMISSION)
        sdio_trans_stop_enable(SDIO_PERIPH);
    sdio_csm_enable(SDIO_PERIPH);

    err = sdio_wait_stat(wait_flags, SDIO_CMD_TIMEOUT);

    if (cmd->cmd_code == STOP_TRANSMISSION)
        sdio_trans_stop_disable(SDIO_PERIPH);

    if (err == RT_EOK && resp_cfg != SDIO_RESPONSETYPE_NO)
    {
        if (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_CMDTMOUT)
            err = -RT_ETIMEOUT;
        else if (!no_crc && (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_CCRCERR))
            err = -RT_EIO;
    }
    sdio_flag_clear(SDIO_PERIPH, SDIO_MASK_CMD_FLAGS);

    if (err != RT_EOK)
    {
        /* CMD5(IO_SEND_OP_COND) 是框架探测 SD IO 功能卡（WiFi 类）的
         * 标准步骤，纯存储卡按协议必须不响应——超时属正常流程，
         * 只记调试日志，避免误导；其余命令失败才是真错误 */
        if (cmd->cmd_code == 5)
            LOG_D("cmd5 (sdio io probe) no response, not an io card");
        else
            LOG_E("cmd%d arg=0x%x err=%d STAT=0x%x", cmd->cmd_code, cmd->arg,
                  err, SDIO_STAT(SDIO_PERIPH));
        cmd->err = err;
        if (req->data)
        {
            sdio_trans_start_disable(SDIO_PERIPH);
            sdio_idma_disable(SDIO_PERIPH);
            req->data->err = err;
        }
        mmcsd_req_complete(host);   /* 唤醒 mmcsd_send_request 的 sem_ack */
        return;
    }

    /* 取响应 */
    if (resp_cfg == SDIO_RESPONSETYPE_SHORT)
    {
        cmd->resp[0] = sdio_response_get(SDIO_PERIPH, SDIO_RESPONSE0);
    }
    else if (resp_cfg == SDIO_RESPONSETYPE_LONG)
    {
        /* R2：128 位。硬件 RESPONSE0 为最高字（官方 demo csd[0]=RESPONSE0）；
         * 框架 GET_BITS 约定 resp[0] 放最高字（off = 3 - bit/32）。
         * 两边都是直序，切勿倒序——倒序会把 CSD_STRUCTURE 读错，
         * 32GB SDHC 卡被按 CSD v1 公式误算成 1.88GB（实测踩坑） */
        cmd->resp[0] = sdio_response_get(SDIO_PERIPH, SDIO_RESPONSE0);
        cmd->resp[1] = sdio_response_get(SDIO_PERIPH, SDIO_RESPONSE1);
        cmd->resp[2] = sdio_response_get(SDIO_PERIPH, SDIO_RESPONSE2);
        cmd->resp[3] = sdio_response_get(SDIO_PERIPH, SDIO_RESPONSE3);
    }
    cmd->err = RT_EOK;

    /* R1B 带忙等待（CMD7 选中/擦除类命令），给卡片一点时间退出 busy */
    if ((cmd->flags & RESP_MASK) == RESP_R1B)
        rt_thread_mdelay(2);

    /* 数据阶段：配置已在命令前完成，这里只做 FIFO 轮询 */
    if (req->data)
        req->data->err = sdio_transfer_data_wait(req->data);

    /* 多块传输框架会附带 stop 命令（CMD12），数据结束后发出 */
    if (req->stop)
    {
        struct rt_mmcsd_cmd *stop = req->stop;

        sdio_flag_clear(SDIO_PERIPH, SDIO_MASK_CMD_FLAGS);
        sdio_command_response_config(SDIO_PERIPH, stop->cmd_code, stop->arg,
                                     SDIO_RESPONSETYPE_SHORT);
        sdio_wait_type_set(SDIO_PERIPH, SDIO_WAITTYPE_NO);
        sdio_trans_stop_enable(SDIO_PERIPH);
        sdio_csm_enable(SDIO_PERIPH);
        stop->err = sdio_wait_stat(SDIO_FLAG_CMDRECV | SDIO_FLAG_CCRCERR |
                                   SDIO_FLAG_CMDTMOUT, SDIO_CMD_TIMEOUT);
        sdio_trans_stop_disable(SDIO_PERIPH);
        sdio_flag_clear(SDIO_PERIPH, SDIO_MASK_CMD_FLAGS);
        if (stop->err == RT_EOK)
            stop->resp[0] = sdio_response_get(SDIO_PERIPH, SDIO_RESPONSE0);
    }

    /* request 完成，释放 sem_ack 唤醒框架（mmcsd_send_request 在等它） */
    mmcsd_req_complete(host);
}

/* 数据配置已在 request 中完成的等待部分 */
static rt_err_t sdio_transfer_data_wait(struct rt_mmcsd_data *data)
{
    rt_uint32_t total = data->blksize * data->blks;
    rt_uint8_t *buf = (rt_uint8_t *)data->buf;
    rt_bool_t is_read = (data->flags & DATA_DIR_READ);
    rt_bool_t use_idma = sdio_idma_capable(data);
    rt_uint32_t end_flags = use_idma ? SDIO_DAT_IDMA_END :
                            (is_read ? SDIO_DAT_READ_END : SDIO_DAT_WRITE_END);
    rt_uint32_t transferred = 0;
    rt_tick_t start = rt_tick_get();
    rt_err_t err = RT_EOK;

    while ((SDIO_STAT(SDIO_PERIPH) & end_flags) == 0)
    {
        if (use_idma)
        {
            /* IDMA 由硬件直搬内存，CPU 只需等结束标志——
             * 与中断抢占彻底解耦，这就是必须上 DMA 的原因 */
            rt_thread_mdelay(1);
        }
        else if (is_read)
        {
            /* RTOS 上下文 polling 策略：FIFO 有货就搬（!RFE），让 FIFO
             * 始终保持接近空，把延迟容忍度用满整个 FIFO 深度。
             * 不能等 RFH 半满才搬——25MHz/4bit 下 8 字只缓冲 ~2.5us，
             * 任意中断抢占即 RXORE（实测 bytes=220/512 溢出） */
            while (!sdio_flag_get(SDIO_PERIPH, SDIO_FLAG_RFE) &&
                   transferred < total)
            {
                rt_uint32_t w = sdio_data_read(SDIO_PERIPH);
                rt_memcpy(buf + transferred, &w, 4);
                transferred += 4;
            }
        }
        else
        {
            if (sdio_flag_get(SDIO_PERIPH, SDIO_FLAG_TFH) && transferred < total)
            {
                rt_int32_t i;
                for (i = 0; i < SD_FIFOHALF_WORDS && transferred < total; i++)
                {
                    rt_uint32_t w = 0;
                    rt_memcpy(&w, buf + transferred, 4);
                    sdio_data_write(SDIO_PERIPH, w);
                    transferred += 4;
                }
            }
        }
        if (rt_tick_get() - start > SDIO_DAT_TIMEOUT)
        {
            err = -RT_ETIMEOUT;
            break;
        }
    }

    if (err == RT_EOK)
    {
        if (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_DTCRCERR)
        {
            LOG_E("DTCRCERR (data crc) bytes=%d/%d STAT=0x%x",
                  transferred, total, SDIO_STAT(SDIO_PERIPH));
            err = -RT_EIO;
        }
        else if (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_DTTMOUT)
        {
            LOG_E("DTTMOUT (data timeout) bytes=%d/%d STAT=0x%x",
                  transferred, total, SDIO_STAT(SDIO_PERIPH));
            err = -RT_ETIMEOUT;
        }
        else if (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_RXORE)
        {
            LOG_E("RXORE (fifo overrun: polling too slow) bytes=%d/%d STAT=0x%x",
                  transferred, total, SDIO_STAT(SDIO_PERIPH));
            err = -RT_EIO;
        }
        else if (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_TXURE)
        {
            LOG_E("TXURE (fifo underrun: polling too slow) bytes=%d/%d STAT=0x%x",
                  transferred, total, SDIO_STAT(SDIO_PERIPH));
            err = -RT_EIO;
        }
        else if (SDIO_STAT(SDIO_PERIPH) & SDIO_FLAG_IDMAERR)
        {
            LOG_E("IDMAERR (idma error) STAT=0x%x", SDIO_STAT(SDIO_PERIPH));
            err = -RT_EIO;
        }
    }

    /* 读完成后再次 invalidate：丢弃传输期间被推测/预取进 cache
     * 的旧行，然后把真实数据从 bounce 拷回框架 buffer */
    if (use_idma && is_read && err == RT_EOK)
    {
        sdio_bounce_cache_ops(total, RT_TRUE);
        rt_memcpy(data->buf, sdio_idma_bounce, total);
    }

    if (!use_idma && is_read && err == RT_EOK)
    {
        while (!sdio_flag_get(SDIO_PERIPH, SDIO_FLAG_RFE) &&
               sdio_flag_get(SDIO_PERIPH, SDIO_FLAG_DATSTA) &&
               transferred < total)
        {
            rt_uint32_t w = sdio_data_read(SDIO_PERIPH);
            rt_memcpy(buf + transferred, &w, 4);
            transferred += 4;
        }
    }

    sdio_trans_start_disable(SDIO_PERIPH);
    if (use_idma)
        sdio_idma_disable(SDIO_PERIPH);
    sdio_flag_clear(SDIO_PERIPH, SDIO_MASK_DATA_FLAGS);
    return err;
}

static void h7_sdio_set_iocfg(struct rt_mmcsd_host *host, struct rt_mmcsd_io_cfg *io_cfg)
{
    static rt_uint32_t cur_clock = 0, cur_width = 0;

    /* 时钟：clock=0 表示关钟下电 */
    if (io_cfg->clock != cur_clock)
    {
        if (io_cfg->clock == 0)
        {
            sdio_hardware_clock_disable(SDIO_PERIPH);
            sdio_power_state_set(SDIO_PERIPH, SDIO_POWER_OFF);
        }
        else
        {
            rt_uint32_t target = io_cfg->clock;
            rt_uint32_t div;

            /* 时钟上限按数据通路区分：
             * IDMA    -> 50MHz（硬件搬数据，总线速率与 CPU 无关）
             * polling -> 12MHz（FIFO 缓冲 ~21us，再高抢占即 RXORE/TXURE） */
#if SDIO_USE_IDMA
            if (target > SDIO_IDMA_MAX_HZ)
                target = SDIO_IDMA_MAX_HZ;
#else
            if (target > SDIO_POLLING_MAX_HZ)
                target = SDIO_POLLING_MAX_HZ;
#endif
            div = SDIO_CLK_SRC_HZ / target;     /* 新 IP 直接分频 */
            if (div < 1) div = 1;

            sdio_clock_config(SDIO_PERIPH, SDIO_SDIOCLKEDGE_RISING,
                              SDIO_CLOCKPWRSAVE_DISABLE, div);
            sdio_power_state_set(SDIO_PERIPH, SDIO_POWER_ON);
            sdio_hardware_clock_disable(SDIO_PERIPH);   /* polling 模式关硬件流控 */
            LOG_I("sdio clock %d Hz (div=%d)%s", target, div,
                  target != io_cfg->clock ? " [clamped]" : "");
        }
        cur_clock = io_cfg->clock;
    }

    /* 总线宽度 */
    if (io_cfg->bus_width != cur_width)
    {
        sdio_bus_mode_set(SDIO_PERIPH,
                          io_cfg->bus_width == MMCSD_BUS_WIDTH_4 ?
                          SDIO_BUSMODE_4BIT : SDIO_BUSMODE_1BIT);
        cur_width = io_cfg->bus_width;
        LOG_I("sdio bus %s", cur_width == MMCSD_BUS_WIDTH_4 ? "4bit" : "1bit");
    }
}

static rt_int32_t h7_sdio_get_card_status(struct rt_mmcsd_host *host)
{
    /* GD32H759I-EVAL 卡座无 CD 引脚：始终有卡。有 CD 引脚请返回真实电平 */
    return 1;
}

static void h7_sdio_enable_sdio_irq(struct rt_mmcsd_host *host, rt_int32_t en)
{
    /* 不承载 SDIO 外设（WiFi 等）功能 */
}

static const struct rt_mmcsd_host_ops h7_sdio_ops = {
    .request         = h7_sdio_request,
    .set_iocfg       = h7_sdio_set_iocfg,
    .get_card_status = h7_sdio_get_card_status,
    .enable_sdio_irq = h7_sdio_enable_sdio_irq,
};

#if defined(BSP_SDIO_REPROBE_CMD)
static struct rt_mmcsd_host *h7_sdio_host = RT_NULL;

/* 手动重新探测——仅供"卡座 CD 信号未接 MCU"的板子使用（如本评估板，
 * 原理图卡座仅 CLK/CMD/D0~D3 六线，无 CD 走线，框架永远收不到插拔
 * 事件）。自画板若将 CD 脚接入 MCU，正确机制是：GPIO 外部中断 +
 * 消抖后调用 mmcsd_change()，且 get_card_status() 返回真实电平
 * ——本驱动未实现该机制，届时应关闭 BSP_SDIO_REPROBE_CMD。
 *
 * mmcsd_change 是乒乓语义：card 非空调用走移除分支，为空调用走识别
 * 分支，故重探测需调用两次。
 *
 * 顺序铁律：必须先 umount 再移除——若文件系统还挂着，sd0/sd0p0 的
 * 引用计数非零，旧设备注销失败变成僵尸；重新识别时同名分区注册
 * 冲突（Put partition ... error），整轮 init 失败。因此本命令在
 * 移除前主动 umount /，用户拔卡重插后只需敲 sdprobe 一条命令。
 * 注意：有文件句柄打开时 umount 会失败，需先关掉再用本命令 */
static void sd_reprobe(int argc, char **argv)
{
    if (h7_sdio_host == RT_NULL)
    {
        rt_kprintf("sdio host not initialized\n");
        return;
    }
    if (h7_sdio_host->card != RT_NULL)
    {
        if (dfs_unmount("/") == 0)
            rt_kprintf("unmount / ok\n");
        mmcsd_change(h7_sdio_host);     /* 移除旧 card 与 sd0 块设备 */
        rt_thread_mdelay(200);          /* 等 mmcsd_detect 线程处理完 */
    }
    mmcsd_change(h7_sdio_host);         /* 完整重新识别 + 注册 sd0 */
}
MSH_CMD_EXPORT_ALIAS(sd_reprobe, sdprobe, re-detect sd card after reinsert);
#endif /* BSP_SDIO_REPROBE_CMD */

/* ===================== 初始化与注册 ===================== */

static void h7_sdio_gpio_init(void)
{
    /* 官方 gpio_config 基础上按总线宽度裁剪：全 AF12，100_220MHZ 驱动，
     * CLK 无上下拉，CMD/D0~D3 上拉（板载另有 10K 上拉）。
     * 1-bit 模式只配 CLK/CMD/D0——D3=PC11 已让给 LCD_B4，碰都不能碰；
     * D1/D2(PC9/PC10) 虽空闲，一并跳过保持引脚干净 */
    rcu_periph_clock_enable(RCU_GPIOB);
    rcu_periph_clock_enable(RCU_GPIOC);
    rcu_periph_clock_enable(RCU_GPIOD);

    gpio_af_set(GPIOB, GPIO_AF_12, GPIO_PIN_13);                            /* D0 */
    gpio_af_set(GPIOC, GPIO_AF_12, GPIO_PIN_12);                            /* CLK */
#if !SDIO_1BIT_MODE
    gpio_af_set(GPIOC, GPIO_AF_12, GPIO_PIN_9 | GPIO_PIN_10 |
                       GPIO_PIN_11);                                        /* D1~D3 */
#endif
    gpio_af_set(GPIOD, GPIO_AF_12, GPIO_PIN_2);                             /* CMD */

    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_13);
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO_PIN_12);
#if !SDIO_1BIT_MODE
    gpio_mode_set(GPIOC, GPIO_MODE_AF, GPIO_PUPD_PULLUP,
                  GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11);
#endif
    gpio_mode_set(GPIOD, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_2);

    gpio_output_options_set(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, GPIO_PIN_13);
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, GPIO_PIN_12);
#if !SDIO_1BIT_MODE
    gpio_output_options_set(GPIOC, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ,
                            GPIO_PIN_9 | GPIO_PIN_10 | GPIO_PIN_11);
#endif
    gpio_output_options_set(GPIOD, GPIO_OTYPE_PP, GPIO_OSPEED_100_220MHZ, GPIO_PIN_2);
}

int rt_hw_sdio_init(void)
{
    struct rt_mmcsd_host *host;

#if SDIO_PLL1_SELF_CONFIG
    /* PLL1R = 25MHz HXTAL /5 *40 /1 = 200MHz，供 SDIO0 内核时钟。
     * 症状验证：若此处未配置，所有命令超时且 SDIO_STAT=0x1000（CMDSTA
     * 命令状态机挂死在激活态——内核时钟缺失的铁证） */
    rcu_pll_input_output_clock_range_config(IDX_PLL1, RCU_PLL1RNG_4M_8M,
                                            RCU_PLL1VCO_192M_836M);
    rcu_pll1_config(5U, 40U, 1U, 1U, 1U);     /* ndiv=5, n=40, p/q/r=1 */
    rcu_pll_clock_output_enable(RCU_PLL1R);
    rcu_osci_on(RCU_PLL1_CK);
    if (ERROR == rcu_osci_stab_wait(RCU_PLL1_CK))
    {
        LOG_E("PLL1 lock failed");
        return -RT_ERROR;
    }
#endif

    /* SDIO 时钟源：PLL1R=200MHz */
    rcu_sdio_clock_config(IDX_SDIO0, RCU_SDIO0SRC_PLL1R);
    rcu_periph_clock_enable(SDIO_RCU);
    h7_sdio_gpio_init();
    sdio_deinit(SDIO_PERIPH);

    host = mmcsd_alloc_host();
    if (!host)
    {
        LOG_E("alloc host failed");
        return -RT_ENOMEM;
    }

    host->ops        = &h7_sdio_ops;
    host->freq_min   = 400000;                  /* 识别阶段 400kHz，div=500 */
#if SDIO_USE_IDMA
    host->freq_max   = SDIO_IDMA_MAX_HZ;        /* IDMA 上限 50MHz */
#else
    host->freq_max   = SDIO_POLLING_MAX_HZ;     /* polling 上限 12MHz（见 set_iocfg 钳位） */
#endif
    host->valid_ocr  = VDD_32_33 | VDD_33_34;
#if SDIO_1BIT_MODE
    /* 不声明 BUSWIDTH_4：框架不会发 ACMD6 切宽，全程 1-bit */
    host->flags      = MMCSD_MUTBLKWRITE;
#else
    host->flags      = MMCSD_BUSWIDTH_4 | MMCSD_MUTBLKWRITE;
#endif
    host->max_seg_size    = 2048;
    host->max_dma_segs    = 1;
    host->max_blk_size    = 512;
    /* 单次传输必须装进 bounce buffer：8 块 * 512 = 4KB < 8160B 上限。
     * 更大的多擦/多写由框架拆成多次 request（读性能损失可忽略） */
    host->max_blk_count   = 8;

#if defined(BSP_SDIO_REPROBE_CMD)
    h7_sdio_host = host;
#endif
    mmcsd_change(host);

    LOG_I("sdio0 host registered");
    return RT_EOK;
}
INIT_DEVICE_EXPORT(rt_hw_sdio_init);

#endif /* BSP_USING_SDIO */