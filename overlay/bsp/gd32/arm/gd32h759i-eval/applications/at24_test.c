/*
 * at24_test.c - AT24C02 EEPROM 测试（走 RT-Thread I2C 框架，板载 I2C1）
 *
 * AT24C02 两个经典坑，测试代码里正面处理：
 * 1. 页大小 8 字节：一次写跨界会在页内回卷（数据写飞），写函数按页切分；
 * 2. 写周期 tWR ~5ms：写后器件进入内部擦写，期间对一切命令 NACK。
 *    用 ACK polling（反复发起读，直到不再 NACK）代替死等 5ms——
 *    这是"NACK 是信息不是错误"的第二个用例（第一个是设备探测）。
 *
 * 命令：
 *   i2c_scan            扫描总线上所有应答的 7 位地址
 *   at24_read <addr> <len>          读 len 字节并 hexdump
 *   at24_write <addr> <b0> [b1..]   写若干字节（自动分页 + ACK polling）
 *   at24_selftest                   写-读-校验-还原，全链路自证
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>     /* strtoul：RT-Thread 无 rt_strtoul，用标准库 */
#include "gd32h7xx.h"   /* i2c_diag 需要 GPIO/I2C 寄存器宏 */

#if defined(BSP_USING_I2C1)

#define DBG_TAG             "app.at24"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#include "app_at24.h"

/* ---------------- finsh 命令 ---------------- */

static void i2c_scan(int argc, char **argv)
{
    struct rt_i2c_bus_device *bus = rt_i2c_bus_device_find("i2c1");
    rt_uint8_t addr, dummy;
    int found = 0;

    rt_kprintf("scanning i2c1...\n");
    for (addr = 0x03; addr < 0x78; addr++)
    {
        struct rt_i2c_msg msg = { addr, RT_I2C_RD, 1, &dummy };
        if (rt_i2c_transfer(bus, &msg, 1) == 1)
        {
            rt_kprintf("  ack @ 0x%02x\n", addr);
            found++;
        }
    }
    rt_kprintf("done, %d device(s)\n", found);
}
MSH_CMD_EXPORT(i2c_scan, scan i2c1 bus for acks);

static void at24_read(int argc, char **argv)
{
    rt_uint8_t buf[AT24_SIZE];
    rt_uint32_t addr, len, i;

    if (argc < 3) { rt_kprintf("usage: at24_read <addr> <len>\n"); return; }
    addr = strtoul(argv[1], RT_NULL, 0);
    len  = strtoul(argv[2], RT_NULL, 0);
    if (addr + len > AT24_SIZE || len == 0) { rt_kprintf("range error\n"); return; }

    if (at24_read_bytes(addr, buf, len) != RT_EOK)
    { rt_kprintf("read failed\n"); return; }
    for (i = 0; i < len; i++)
    {
        if (i % 16 == 0) rt_kprintf("\n%04x: ", addr + i);
        rt_kprintf("%02x ", buf[i]);
    }
    rt_kprintf("\n");
}
MSH_CMD_EXPORT(at24_read, at24_read <addr> <len>);

static void at24_write(int argc, char **argv)
{
    rt_uint8_t data[64];
    rt_uint32_t addr, i, n = argc - 2;

    if (argc < 3) { rt_kprintf("usage: at24_write <addr> <b0> [b1...]\n"); return; }
    addr = strtoul(argv[1], RT_NULL, 0);
    if (addr + n > AT24_SIZE) { rt_kprintf("range error\n"); return; }
    for (i = 0; i < n; i++)
        data[i] = strtoul(argv[2 + i], RT_NULL, 0);

    if (at24_write_bytes(addr, data, n) != RT_EOK)
    { rt_kprintf("write failed\n"); return; }
    rt_kprintf("written %lu bytes @ 0x%02lx\n", n, addr);
}
MSH_CMD_EXPORT(at24_write, at24_write <addr> <b0> [b1...]);

/* 自证：备份 -> 写伪随机 -> 读回校验 -> 还原。全程不动用户的其它数据。 */
static void at24_selftest(int argc, char **argv)
{
    rt_uint8_t backup[16], pattern[16], verify[16];
    rt_uint32_t i;
    rt_err_t ret;

    for (i = 0; i < 16; i++)
        pattern[i] = (rt_uint8_t)(0xA5 ^ (i * 37));   /* 固定伪随机，可复现 */

    ret = at24_read_bytes(0, backup, 16);
    if (ret != RT_EOK) goto _fail;
    ret = at24_write_bytes(0, pattern, 16);           /* 跨 8 字节页边界，考分页逻辑 */
    if (ret != RT_EOK) goto _fail;
    ret = at24_read_bytes(0, verify, 16);
    if (ret != RT_EOK) goto _fail;

    if (rt_memcmp(pattern, verify, 16) != 0)
    {
        rt_kprintf("MISMATCH! wrote:");
        for (i = 0; i < 16; i++) rt_kprintf(" %02x", pattern[i]);
        rt_kprintf("\n           read:");
        for (i = 0; i < 16; i++) rt_kprintf(" %02x", verify[i]);
        rt_kprintf("\n");
        at24_write_bytes(0, backup, 16);              /* 尽量还原现场 */
        return;
    }
    ret = at24_write_bytes(0, backup, 16);            /* 还原 */
    if (ret != RT_EOK) goto _fail;
    rt_kprintf("at24_selftest PASS (16B cross-page write/read/verify/restore)\n");
    return;
_fail:
    rt_kprintf("at24_selftest FAIL: %d\n", ret);
}
MSH_CMD_EXPORT(at24_selftest, write/read/verify/restore 16 bytes);

/*
 * i2c_diag - 三层现场取证（全 NACK 故障专用）：
 * 1. GPIO 配置层：PH4/PB11 是否仍是 AF4+OD+上拉（抓"引脚被别的驱动偷走"）；
 * 2. 物理层：临时切输入模式读 SCL/SDA 实际电平（抓"总线被拉死/上拉丢失"）；
 * 3. 外设层：I2C_STAT/CTL0/TIMING 寄存器原值（抓"时钟/使能/时序异常"）。
 */
static void i2c_diag(int argc, char **argv)
{
    uint32_t gpioh_ctl = GPIO_CTL(GPIOH);
    uint32_t gpiob_ctl = GPIO_CTL(GPIOB);
    /* 修正：脚 0~7 的 AF 在 AFSEL0，脚 8~15 才在 AFSEL1（上一版读错寄存器） */
    uint32_t afr_h_lo = GPIO_AFSEL0(GPIOH), afr_b_hi = GPIO_AFSEL1(GPIOB);
    uint32_t otype_h = GPIO_OMODE(GPIOH), otype_b = GPIO_OMODE(GPIOB);
    rt_uint8_t scl_lvl, sda_lvl;
    rt_uint32_t prev, edges, i, stat;

    rt_kprintf("== 1. GPIO config ==\n");
    rt_kprintf("PH4 : mode=%lu(2=AF) af=%lu(expect 4) od=%lu | PB11: mode=%lu af=%lu od=%lu\n",
               (gpioh_ctl >> (4 * 2)) & 0x3,
               (afr_h_lo >> (4 * 4)) & 0xF,
               (otype_h >> 4) & 0x1,
               (gpiob_ctl >> (11 * 2)) & 0x3,
               (afr_b_hi >> ((11 - 8) * 4)) & 0xF,
               (otype_b >> 11) & 0x1);

    rt_kprintf("== 2. bus level (pins temporarily -> input) ==\n");
    gpio_mode_set(GPIOH, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO_PIN_4);
    gpio_mode_set(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, GPIO_PIN_11);
    rt_thread_mdelay(1);
    scl_lvl = gpio_input_bit_get(GPIOH, GPIO_PIN_4) ? 1 : 0;
    sda_lvl = gpio_input_bit_get(GPIOB, GPIO_PIN_11) ? 1 : 0;
    rt_kprintf("SCL(PH4)=%d SDA(PB11)=%d (expect both 1 = pulled up & idle)\n",
               scl_lvl, sda_lvl);
    gpio_mode_set(GPIOH, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_4);
    gpio_mode_set(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, GPIO_PIN_11);

    rt_kprintf("== 3. peripheral regs ==\n");
    rt_kprintf("I2C_CTL0=0x%08lx (EN=%lu) TIMING=0x%08lx STAT=0x%08lx\n",
               I2C_CTL0(I2C1), I2C_CTL0(I2C1) & 1,
               I2C_TIMING(I2C1), I2C_STAT(I2C1));
    rt_kprintf("RCU_CFG3.I2C1SEL=%lu (0=APB1 1=PLL2R 2=IRC64MDIV 3=LPIRC4M)\n",
               RCU_CFG3 & 0x3);

    rt_kprintf("== 4. SCL edge forensics (poor man's scope) ==\n");
    /* 清残留状态，手动发起一次对 0x50 的 1 字节读 */
    I2C_STATC(I2C1) = I2C_STAT(I2C1) & (I2C_STAT_NACK | I2C_STAT_BERR |
                                        I2C_STAT_LOSTARB | I2C_STAT_TIMEOUT);
    i2c_master_addressing(I2C1, 0x50 << 1, I2C_MASTER_RECEIVE);
    i2c_transfer_byte_number_config(I2C1, 1);
    I2C_CTL1(I2C1) &= ~(I2C_CTL1_RELOAD | I2C_CTL1_AUTOEND);
    I2C_CTL1(I2C1) |= I2C_CTL1_AUTOEND;
    i2c_start_on_bus(I2C1);
    /* IDATA 在 AF 模式下照样反映引脚真实电平：紧轮询数 SCL 边沿 */
    prev = 1; edges = 0;
    for (i = 0; i < 2000000; i++)
    {
        rt_uint32_t lv = gpio_input_bit_get(GPIOH, GPIO_PIN_4) ? 1 : 0;
        if (lv != prev) { edges++; prev = lv; }
        if (!(I2C_STAT(I2C1) & I2C_STAT_I2CBSY) && i > 200)
            break;
    }
    stat = I2C_STAT(I2C1);
    rt_kprintf("SCL edges=%lu (expect ~18-20 for addr+ack+data+ack), loops=%lu\n",
               edges, i);
    rt_kprintf("STAT after xfer=0x%08lx (NACK=%lu TC=%lu BUSY=%lu)\n",
               stat, (stat >> 4) & 1, (stat >> 6) & 1, (stat >> 15) & 1);
    /* 清场 */
    I2C_STATC(I2C1) = stat & (I2C_STAT_NACK | I2C_STAT_BERR |
                              I2C_STAT_LOSTARB | I2C_STAT_TIMEOUT);
    I2C_CTL1(I2C1) |= I2C_CTL1_STOP;
}
MSH_CMD_EXPORT(i2c_diag, dump i2c1 gpio/bus/reg/scl-edge forensics);

#endif /* BSP_USING_I2C1 */