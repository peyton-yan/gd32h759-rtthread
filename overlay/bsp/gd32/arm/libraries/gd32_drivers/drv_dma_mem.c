/*
 * drv_dma_mem.c — D-Cache 无关的 DMA 内存区（MPU Normal Non-cacheable）+ DMA 池分配器
 *
 * 背景：M7 带 D-Cache，DMA 与 CPU 访问同一块内存必须做 cache 维护。
 * 而 cache 维护按 32B cache line 对齐放大，稍有不慎就会"invalidate 放大"
 * 踩到相邻内存（本项目 SDIO 曾实测 pc=0 INVSTATE 这一类崩溃）。
 *
 * 方案：链接脚本划出 .dma_buffer 段（DMARAM，见 link.ld），本文件：
 *   1) 用一条 MPU 区域把它配成 Normal Non-cacheable Shareable；
 *   2) 在上面实现一个极简 first-fit 池分配器，供需要 DMA 的驱动取缓冲
 *      （如串口 DMA 接收缓冲）——放进来的缓冲 CPU/DMA 天然一致，无需 cache 维护。
 *
 * 用法：
 *   静态缓冲：__attribute__((section(".dma_buffer"))) static rt_uint8_t buf[N];
 *   动态缓冲：void *p = rt_dma_malloc(n); ... rt_dma_free(p);
 */
#include <rtthread.h>
#include <rthw.h>
#include <gd32h7xx.h>

#define DBG_TAG "drv.dma"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 必须与 link.ld 的 DMARAM 一致：0x24070000 / 64KB */
#define DMA_MPU_REGION   4
#define DMA_MPU_BASE     0x24070000UL

extern char _dma_buffer_start;
extern char _dma_buffer_end;

/* ===================== 极简 DMA 池分配器 ===================== */
/* 32B 对齐，first-fit + 前后合并。仅供 DMA 缓冲使用，别拿它当通用堆 */

typedef struct dma_blk {
    rt_size_t        size;   /* 含头部，32B 对齐 */
    struct dma_blk  *next;   /* 空闲链表（按地址升序） */
} dma_blk_t;

#define DMA_ALIGN(x) (((x) + 31U) & ~31U)
#define DMA_HDR      DMA_ALIGN(sizeof(dma_blk_t))

static rt_uint8_t *g_pool_start, *g_pool_end, *g_pool_cur;
static dma_blk_t  *g_free_list;
static rt_size_t   g_pool_peak;

void rt_dma_pool_init(void *start, rt_size_t size)
{
    g_pool_start = (rt_uint8_t *)start;
    g_pool_end   = g_pool_start + (size & ~31U);
    g_pool_cur   = g_pool_start;
    g_free_list  = RT_NULL;
    g_pool_peak  = 0;
}

void *rt_dma_malloc(rt_size_t size)
{
    dma_blk_t *prev = RT_NULL, *b = g_free_list;
    rt_size_t  need;

    if (size == 0)
        return RT_NULL;
    need = DMA_ALIGN(size) + DMA_HDR;

    while (b && b->size < need) { prev = b; b = b->next; }

    if (b) {                         /* 命中空闲块 */
        if (prev) prev->next = b->next; else g_free_list = b->next;
    } else {                         /* bump 新块 */
        if (g_pool_cur + need > g_pool_end)
            return RT_NULL;
        b = (dma_blk_t *)g_pool_cur;
        b->size = need;
        g_pool_cur += need;
        if (g_pool_cur - g_pool_start > (rt_int32_t)g_pool_peak)
            g_pool_peak = (rt_size_t)(g_pool_cur - g_pool_start);
    }
    return (void *)((rt_uint8_t *)b + DMA_HDR);
}

void rt_dma_free(void *ptr)
{
    dma_blk_t *b, *it, *prev;

    if (ptr == RT_NULL)
        return;
    b = (dma_blk_t *)((rt_uint8_t *)ptr - DMA_HDR);
    if ((rt_uint8_t *)b < g_pool_start || (rt_uint8_t *)b >= g_pool_end)
        return;                      /* 非本池指针，忽略 */

    prev = RT_NULL; it = g_free_list;
    while (it && (rt_uint8_t *)it < (rt_uint8_t *)b) { prev = it; it = it->next; }
    b->next = it;
    if (prev) prev->next = b; else g_free_list = b;

    if (b->next && (rt_uint8_t *)b + b->size == (rt_uint8_t *)b->next) {
        b->size += b->next->size;
        b->next  = b->next->next;
    }
    if (prev && (rt_uint8_t *)prev + prev->size == (rt_uint8_t *)b) {
        prev->size += b->size;
        prev->next  = b->next;
    }
}

rt_size_t rt_dma_pool_used(void)
{
    return g_pool_peak;
}

/* ===================== 串口框架的 DMA 缓冲分配钩子 ===================== */
/* dev_serial.c 里的弱定义默认走 rt_malloc；这里给出强定义，改走非cacheable 池。
 * 仅用于 DMA_RX 模式的接收 FIFO（正被 DMA 写入的那块内存）。 */
void *rt_serial_dma_buf_alloc(rt_size_t size)
{
    /* 多留 32B：drv_usart.c 为绕 cache 会在 buffer 末尾(±32B)做备份/还原，
     * 缓冲已是非cacheable（这些操作其实无必要），但保留驱动逻辑时不能让写越界到池的下一块 */
    return rt_dma_malloc(size + 32);
}

void rt_serial_dma_buf_free(void *ptr)
{
    rt_dma_free(ptr);
}

/* ===================== 初始化 ===================== */
static int rt_hw_dma_mem_init(void)
{
    rt_size_t pool_size = (rt_size_t)(&_dma_buffer_end - &_dma_buffer_start);

    rt_dma_pool_init(&_dma_buffer_start, pool_size);

    ARM_MPU_Disable();
    /* ARMv7-M 属性：XN=1, AP=full, TEX=1,S=1,C=0,B=0 => Normal Non-cacheable Shareable */
    ARM_MPU_SetRegion(ARM_MPU_RBAR(DMA_MPU_REGION, DMA_MPU_BASE),
                      ARM_MPU_RASR(1, ARM_MPU_AP_FULL, 1, 1, 0, 0, 0,
                                   ARM_MPU_REGION_SIZE_64KB));
    ARM_MPU_Enable(MPU_MODE_PRIV_DEFAULT);

    LOG_I("DMA non-cacheable: 0x%08X +%uKB (region %d), pool %u B",
          (unsigned)DMA_MPU_BASE, (unsigned)(pool_size >> 10),
          DMA_MPU_REGION, (unsigned)pool_size);
    return 0;
}
INIT_BOARD_EXPORT(rt_hw_dma_mem_init);
