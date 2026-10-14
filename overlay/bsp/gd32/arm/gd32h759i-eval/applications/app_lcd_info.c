/*
 * app_lcd_info.c - ST7789 开机系统信息仪表盘（纯英文点阵 UI）
 *
 * 定位：应用层（app）。ST7789 驱动(drv_st7789.c)是"外设的外设"，
 *       本应用是它的第一个用户——验证刷屏通路 + 让板子上电就有看相。
 *
 * 设计约束：
 *   - 240x240 小屏，无中文字库（做字库不值当），全大写英文 UI；
 *   - 5x7 点阵字，2 倍放大（字符 12x14，行高 20），只收
 *     A-Z / 0-9 / 少量符号，字库总共几百字节；
 *   - 静态行只画一遍，动态行（HEAP/THREAD/UPTIME）1Hz 局部刷新，
 *     避免整屏重绘浪费 SPI 带宽；
 *   - 左下角心跳方块 1Hz 闪烁——肉眼即可判断系统活着、调度器在跑。
 *
 * 布局（240x240）：
 *   y=0    标题条（藏青底白字）：GD32H759I-EVAL
 *   y=28+  信息行：OS / CPU / ROM+RAM / HEAP(动态) / THREAD(动态)
 *          / UPTIME(动态) / LCD / BUILD
 *   y=216  状态条：心跳方块 + SYSTEM RUNNING
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <stdio.h>
#include <string.h>
#include "gd32h7xx.h"

#if defined(BSP_USING_SPI4)

/* ST7789 驱动提供的刷屏接口（drv_st7789.c，applications/） */
extern void st7789_blit(rt_uint16_t x0, rt_uint16_t y0, rt_uint16_t x1, rt_uint16_t y1,
                        const rt_uint8_t *pixels);
extern void st7789_fill(rt_uint16_t color);

#define SCR_W       240
#define ROW_H       20              /* 5x7 字 2 倍放大 + 行距 */
#define CHAR_W      12              /* (5 列 + 1 间距) * 2 倍 */

/* RGB565 配色 */
#define C_BG        0x0000          /* 底：黑 */
#define C_TITLE_BG  0x000F          /* 标题条：藏青 */
#define C_TITLE     0xFFFF          /* 标题字：白 */
#define C_LABEL     0xC618          /* 标签：浅灰 */
#define C_VALUE     0x07FF          /* 数值：青 */
#define C_OK        0x07E0          /* 心跳/正常：绿 */
#define C_AMBER     0xFD20          /* 强调：琥珀 */

/* ---------- 5x7 点阵字库（列式，LSB=顶像素） ---------- */
static const rt_uint8_t font_dig[10][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
};
static const rt_uint8_t font_az[26][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};
/* 符号表：chars 与 glyphs 一一对应 */
static const char     sym_chars[] = " .:-/%()[]+*=_";
static const rt_uint8_t sym_glyphs[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x63,0x13,0x08,0x64,0x63}, /* % */
    {0x1C,0x22,0x41,0x00,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x7F,0x41,0x41,0x00,0x00}, /* [ */
    {0x41,0x41,0x7F,0x00,0x00}, /* ] */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
};

static const rt_uint8_t *glyph_of(char c)
{
    const char *p;
    if (c >= 'a' && c <= 'z') c -= 32;              /* 小写一律升大写 */
    if (c >= '0' && c <= '9') return font_dig[c - '0'];
    if (c >= 'A' && c <= 'Z') return font_az[c - 'A'];
    p = strchr(sym_chars, c);
    if (p) return sym_glyphs[p - sym_chars];
    return sym_glyphs[0];                            /* 不认识的字画空格 */
}

/* ---------- 行渲染：一行 240x20 的局部帧缓冲，画完整块 blit ---------- */
static rt_uint16_t row_fb[SCR_W * ROW_H];

static void fb_put_pixel(int x, int y, rt_uint16_t color)
{
    if (x >= 0 && x < SCR_W && y >= 0 && y < ROW_H)
        row_fb[y * SCR_W + x] = color;
}

static void fb_fill(rt_uint16_t color)
{
    int i;
    for (i = 0; i < SCR_W * ROW_H; i++) row_fb[i] = color;
}

/* 在 (x,y) 起画一个字符，2 倍放大 */
static void fb_draw_char(int x, int y, char c, rt_uint16_t color)
{
    const rt_uint8_t *g = glyph_of(c);
    int col, row, dx, dy;
    for (col = 0; col < 5; col++)
        for (row = 0; row < 7; row++)
            if (g[col] & (1 << row))
                for (dx = 0; dx < 2; dx++)
                    for (dy = 0; dy < 2; dy++)
                        fb_put_pixel(x + col * 2 + dx, y + row * 2 + dy, color);
}

static void fb_draw_str(int x, int y, const char *s, rt_uint16_t color)
{
    while (*s)
    {
        fb_draw_char(x, y, *s++, color);
        x += CHAR_W;
    }
}

/* 画一整行并推上屏：label 灰字靠左，value 彩字跟在后面 */
static void info_row(rt_uint16_t y, const char *label, const char *value,
                     rt_uint16_t val_color)
{
    fb_fill(C_BG);
    fb_draw_str(4, 3, label, C_LABEL);
    fb_draw_str(4 + (int)strlen(label) * CHAR_W + CHAR_W, 3, value, val_color);
    st7789_blit(0, y, SCR_W - 1, y + ROW_H - 1, (const rt_uint8_t *)row_fb);
}

/* ---------- 动态数据源 ---------- */
static void get_heap(char *buf, int len)
{
    rt_size_t total = 0, used = 0, max_used = 0;
    rt_memory_info(&total, &used, &max_used);
    rt_snprintf(buf, len, "%luK FREE/%luK",
                (rt_uint32_t)(total - used) / 1024, (rt_uint32_t)total / 1024);
}

static void get_threads(char *buf, int len)
{
    struct rt_object_information *info;
    rt_list_t *node;
    rt_uint32_t cnt = 0;
    info = rt_object_get_information(RT_Object_Class_Thread);
    rt_list_for_each(node, &info->object_list) cnt++;
    rt_snprintf(buf, len, "%lu", cnt);
}

static void get_uptime(char *buf, int len)
{
    rt_uint32_t s = rt_tick_get() / RT_TICK_PER_SECOND;
    rt_snprintf(buf, len, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

/* ---------- 整屏绘制 ---------- */
static void lcd_info_render_all(void)
{
    char v[40];

    st7789_fill(C_BG);

    /* 标题条 */
    fb_fill(C_TITLE_BG);
    fb_draw_str(8, 3, "GD32H759I-EVAL", C_TITLE);
    st7789_blit(0, 0, SCR_W - 1, ROW_H - 1, (const rt_uint8_t *)row_fb);

    rt_snprintf(v, sizeof(v), "RT-THREAD %d.%d.%d",
                RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);
    info_row(28,  "OS",     v, C_VALUE);
    rt_snprintf(v, sizeof(v), "M7 %luMHZ APB2 %luM",
                rcu_clock_freq_get(CK_SYS) / 1000000,
                rcu_clock_freq_get(CK_APB2) / 1000000);
    info_row(48,  "CPU",    v, C_VALUE);
    info_row(68,  "MEM",    "FLASH 3840K SRAM 1024K", C_VALUE);

    get_heap(v, sizeof(v));   info_row(88,  "HEAP",   v, C_OK);
    get_threads(v, sizeof(v));info_row(108, "THREAD", v, C_OK);
    get_uptime(v, sizeof(v)); info_row(128, "UPTIME", v, C_OK);

    info_row(148, "LCD",    "SPI4 ST7789 240X240", C_VALUE);
    rt_snprintf(v, sizeof(v), "%s", __DATE__);
    info_row(168, "BUILD",  v, C_AMBER);

    /* 状态条 */
    info_row(196, "STATUS", "SYSTEM RUNNING", C_OK);
}

/* 心跳方块：绿/暗交替，肉眼判活 */
static void heartbeat(int on)
{
    fb_fill(C_BG);
    int x, y;
    for (y = 0; y < 12; y++)
        for (x = 0; x < 12; x++)
            row_fb[(y + 2) * SCR_W + x + 4] = on ? C_OK : 0x0320;
    st7789_blit(0, 216, SCR_W - 1, 216 + ROW_H - 1, (const rt_uint8_t *)row_fb);
}

/* ---------- 1Hz 刷新线程 ---------- */
static void lcd_info_entry(void *param)
{
    char v[40];
    int beat = 0;

    rt_thread_mdelay(300);          /* 等 st7789 初始化（同为 APP 级）完成 */
    lcd_info_render_all();

    while (1)
    {
        get_heap(v, sizeof(v));    info_row(88,  "HEAP",   v, C_OK);
        get_threads(v, sizeof(v)); info_row(108, "THREAD", v, C_OK);
        get_uptime(v, sizeof(v));  info_row(128, "UPTIME", v, C_OK);
        heartbeat(beat ^= 1);
        rt_thread_mdelay(1000);
    }
}

static int lcd_info_start(void)
{
    rt_thread_t tid = rt_thread_create("lcd_info", lcd_info_entry, RT_NULL,
                                       2048, 20, 10);
    if (tid) rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(lcd_info_start);

/* 手动重绘（跑过 lcd_test 等把屏画花之后恢复用） */
static void lcd_info(int argc, char **argv)
{
    lcd_info_render_all();
    rt_kprintf("lcd_info: dashboard redrawn\n");
}
MSH_CMD_EXPORT(lcd_info, redraw system info dashboard);

#endif /* BSP_USING_SPI4 */