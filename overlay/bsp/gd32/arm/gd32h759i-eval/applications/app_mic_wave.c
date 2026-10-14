/*
 * app_mic_wave.c - 麦克风实时波形示波器（HPDF -> ST7789 SPI LCD）
 *
 * 数据通路：
 *   MP34DT06J(PDM 麦) -> HPDF 滤波 -> DMA 双缓冲 -> wave_buf 历史环形缓冲
 *   -> 本文件每帧取 512 点 -> 画成 240x240 RGB565 帧 -> st7789_blit 推屏
 *
 * 渲染模型（每帧）：
 *   黑底 + 中心零线(暗灰) + 每列一条竖线（从中心到采样值），240 列
 *   采样映射：x 列取 wave[x*512/240]，幅值 int16 压到 ±120 像素
 *
 * 测试命令：
 *   lcd_wave [sec]  —— 默认 30 秒，结束打印实际 fps
 */

#include <rtthread.h>
#include <rtdevice.h>

#if defined(BSP_USING_HPDF) && defined(BSP_USING_ST7789)

#include <string.h>

#define DBG_TAG             "app.wave"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define LCD_W               240
#define LCD_H               240
#define WAVE_POINTS         512                 /* 与 drv_hpdf_h7 的 WAVE_HISTORY 对齐 */

#define COLOR_BG            0x0000              /* 黑 */
#define COLOR_ZERO          0x4208              /* 暗灰零线 */
#define COLOR_WAVE          0x07E0              /* 示波器绿 */
   
extern void st7789_blit(rt_uint16_t x0, rt_uint16_t y0, rt_uint16_t x1, rt_uint16_t y1,
                        const rt_uint8_t *pixels);
extern void hpdf_get_wave(int16_t *buf, rt_uint32_t n);

static rt_uint16_t wave_fb[LCD_W * LCD_H];    /* 115200 字节帧缓冲 */
static int16_t       wave_snap[WAVE_POINTS];

/* 画一列竖线：从 y_a 到 y_b（含端点），自动排序 */
static inline void vline(int x, int y_a, int y_b, rt_uint16_t color)
{
    int y;
    if (y_a > y_b) { int t = y_a; y_a = y_b; y_b = t; }
    if (y_a < 0) y_a = 0;
    if (y_b > LCD_H - 1) y_b = LCD_H - 1;
    for (y = y_a; y <= y_b; y++)
        wave_fb[y * LCD_W + x] = color;
}

static void wave_render_frame(void)
{
    int x;

    /* 黑底（逐字写比 memset 快：RGB565 是 16 位） */
    for (x = 0; x < LCD_W * LCD_H; x++)
        wave_fb[x] = COLOR_BG;

    /* 中心零线 */
    for (x = 0; x < LCD_W; x++)
        wave_fb[LCD_H / 2 * LCD_W + x] = COLOR_ZERO;

    /* 波形：240 列，每列从中心画到采样幅值 */
    hpdf_get_wave(wave_snap, WAVE_POINTS);
    for (x = 0; x < LCD_W; x++) {
        int s  = wave_snap[x * WAVE_POINTS / LCD_W];
        int dy = s >> 8;                    /* int16 -> ±128 */
        if (dy >  LCD_H / 2 - 1) dy =  LCD_H / 2 - 1;
        if (dy < -(LCD_H / 2))     dy = -(LCD_H / 2);
        vline(x, LCD_H / 2, LCD_H / 2 - dy, COLOR_WAVE);
    }
}

static void lcd_wave(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 30;
    rt_tick_t start, end;
    rt_uint32_t frames = 0;

    rt_kprintf("lcd_wave: %ds, speak / tap / whistle at the mic...\n", secs);
    start = rt_tick_get();
    while (rt_tick_get() - start < secs * RT_TICK_PER_SECOND) {
        wave_render_frame();
        st7789_blit(0, 0, LCD_W - 1, LCD_H - 1, (const rt_uint8_t *)wave_fb);
        frames++;
    }
    end = rt_tick_get();

    rt_kprintf("lcd_wave: %lu frames in %lu ms -> %lu.%02lu fps\n",
               frames, end - start,
               frames * 1000 / (end - start),
               (frames * 100000 / (end - start)) % 100);
}
MSH_CMD_EXPORT(lcd_wave, lcd_wave [sec] - mic oscilloscope on SPI LCD);

#endif /* BSP_USING_HPDF && BSP_USING_SPI_LCD */