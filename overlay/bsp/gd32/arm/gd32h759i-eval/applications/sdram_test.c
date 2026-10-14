/*
 * SDRAM 测试命令 —— 配合 drv_sdram_h7.c (EXMC DEVICE0 @0xC0000000, 32MB)
 *
 *   sdram_test          完整测试: 数据总线走步 + 地址总线走步 + 全容量伪随机校验
 *   sdram_speed         32MB 顺序写/读带宽测试
 *
 * 注意: SDRAM 区 MPU 已配 non-cacheable, 无需 cache 维护, 读到的就是真实存储值。
 */

#include <rtthread.h>
#include <rtdevice.h>

#define SDRAM_BASE  ((volatile rt_uint32_t *)0xC0000000U)
#define SDRAM_WORDS (32U * 1024U * 1024U / 4U)

static rt_uint32_t lfsr_next(rt_uint32_t x)
{
    /* 32-bit LFSR, xorshift */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* 数据总线走步: 只动首地址的数据, 走动的 1 应在读回值中走动 */
static rt_err_t test_data_bus(void)
{
    rt_uint32_t i, v;
    SDRAM_BASE[0] = 0;
    for (i = 0; i < 32; i++) {
        SDRAM_BASE[0] = (1UL << i);
        v = SDRAM_BASE[0];
        if (v != (1UL << i)) {
            rt_kprintf("[data bus] bit %lu failed: wrote 0x%08lx read 0x%08lx\n",
                       i, 1UL << i, v);
            return -RT_ERROR;
        }
    }
    rt_kprintf("[data bus] walking-1 over 32 bits: OK\n");
    return RT_EOK;
}

/* 地址总线走步: 在每个 2^n 字偏移写特征值, 最后回读验证无串扰 */
static rt_err_t test_addr_bus(void)
{
    rt_uint32_t i;
    rt_uint32_t offsets[25];

    for (i = 0; i < 25; i++) {
        offsets[i] = (1UL << i);           /* 1 字 .. 16M 字(64MB 边界内取 32MB 内) */
    }
    if (offsets[24] >= SDRAM_WORDS) {
        /* 1<<23 字 = 32MB, 已到顶; 只保留范围内的 */
    }

    for (i = 0; i < 24 && offsets[i] < SDRAM_WORDS; i++) {
        SDRAM_BASE[offsets[i]] = 0xA5000000UL | offsets[i];
    }
    for (i = 0; i < 24 && offsets[i] < SDRAM_WORDS; i++) {
        rt_uint32_t expect = 0xA5000000UL | offsets[i];
        rt_uint32_t v = SDRAM_BASE[offsets[i]];
        if (v != expect) {
            rt_kprintf("[addr bus] offset 0x%lx failed: expect 0x%08lx read 0x%08lx\n",
                       offsets[i], expect, v);
            return -RT_ERROR;
        }
    }
    rt_kprintf("[addr bus] walking-1 over 24 address lines: OK\n");
    return RT_EOK;
}

/* 全容量伪随机校验 */
static rt_err_t test_full_random(void)
{
    rt_uint32_t i, seed = 0x12345678UL, x;
    rt_tick_t t0;

    t0 = rt_tick_get();
    x = seed;
    for (i = 0; i < SDRAM_WORDS; i++) {
        x = lfsr_next(x);
        SDRAM_BASE[i] = x;
    }
    x = seed;
    for (i = 0; i < SDRAM_WORDS; i++) {
        x = lfsr_next(x);
        if (SDRAM_BASE[i] != x) {
            rt_kprintf("[full] mismatch @0x%08lx: expect 0x%08lx read 0x%08lx\n",
                       (rt_uint32_t)&SDRAM_BASE[i], x, SDRAM_BASE[i]);
            return -RT_ERROR;
        }
    }
    rt_kprintf("[full] 32MB pseudo-random write+verify: OK (%lu ms)\n",
               rt_tick_get() - t0);
    return RT_EOK;
}

void sdram_test(int argc, char **argv)
{
    rt_kprintf("=== SDRAM test: 32MB @ 0xC0000000 ===\n");
    if (test_data_bus() != RT_EOK)    goto fail;
    if (test_addr_bus() != RT_EOK)    goto fail;
    if (test_full_random() != RT_EOK) goto fail;
    rt_kprintf("=== ALL PASS ===\n");
    return;
fail:
    rt_kprintf("=== FAILED ===\n");
}
MSH_CMD_EXPORT(sdram_test, SDRAM full test);

void sdram_speed(int argc, char **argv)
{
    rt_uint32_t i;
    rt_tick_t t0;
    rt_uint32_t ms, kbps;

    t0 = rt_tick_get();
    for (i = 0; i < SDRAM_WORDS; i++) {
        SDRAM_BASE[i] = i;
    }
    ms = rt_tick_get() - t0;
    kbps = ms ? (rt_uint32_t)((rt_uint64_t)SDRAM_WORDS * 4 * 1000 / ms / 1024) : 0;
    rt_kprintf("write 32MB: %lu ms, %lu KB/s\n", ms, kbps);

    t0 = rt_tick_get();
    {
        rt_uint32_t sink = 0;
        for (i = 0; i < SDRAM_WORDS; i++) {
            sink += SDRAM_BASE[i];
        }
        ms = rt_tick_get() - t0;
        kbps = ms ? (rt_uint32_t)((rt_uint64_t)SDRAM_WORDS * 4 * 1000 / ms / 1024) : 0;
        rt_kprintf("read 32MB: %lu ms, %lu KB/s (sink=0x%08lx)\n", ms, kbps, sink);
    }
}
MSH_CMD_EXPORT(sdram_speed, SDRAM bandwidth test);