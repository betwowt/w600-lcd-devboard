/*
 * STM32 P024C135 LCD demo ported to the W600 module dev board.
 *
 * Reachable states of the original demo, in order:
 *   text -> full-screen colors -> color bars -> grayscale -> handwriting
 * The original LOGO/IMAGE states are replaced by a TF-card reader: the
 * boot logo is /logo.bin (240x320 RGB565) on the FAT16/FAT32 card.
 */
#include "board.h"
#include "lcd.h"
#include "logo.h"
#include "touch.h"
#include "lcd_demo.h"

#define ST_TEXT        0u
#define ST_COLOR_FULL  1u
#define ST_COLOR_BAR   2u
#define ST_GRAY        3u
#define ST_DRAW        4u

#define TEXT_MS        2000u
#define COLOR_MS       1000u
#define BARS_MS        3000u
#define GRAY_MS        3000u

#define BTN_W          60
#define BTN_H          30
#define BTN_X          (LCD_W - BTN_W)
#define BTN_Y          (LCD_H - BTN_H)

#define BRUSH_W        2
#define BRUSH_LO       ((BRUSH_W - 1) / 2)
#define BRUSH_HI       (BRUSH_W / 2)
#define BRUSH_COLOR    LCD_WHITE

static u32 g_state;
static u32 g_state_start;
static u8  g_color_idx;

/* tls_os_get_time() counts 500 Hz ticks on this SDK. */
static u32 now_ms(void)
{
    return tls_os_get_time() * 2u;
}

static void fill_screen(u16 color)
{
    lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, color);
}

static void draw_text_screen(void)
{
    fill_screen(LCD_BLACK);
    lcd_text(12, 40, "W600 Display", LCD_WHITE, LCD_BLACK, 2);
    lcd_text(20, 90, "ILI9341 240x320", LCD_BLUE, LCD_BLACK, 1);
    lcd_text(12, 130, "Puyang LCD", LCD_RED, LCD_BLACK, 2);
    lcd_text(20, 180, "FT6236 Touch", LCD_GREEN, LCD_BLACK, 1);
    lcd_text(20, 210, "XL9555 + SPI", LCD_CYAN, LCD_BLACK, 1);
}

static void draw_color_bars(void)
{
    static const u16 colors[6] = {
        LCD_RED, LCD_GREEN, LCD_BLUE, LCD_YELLOW, LCD_CYAN, LCD_MAGENTA
    };
    int i;

    for (i = 0; i < 6; i++) {
        u16 x0 = (u16)(i * 40);
        lcd_fill(x0, 0, (u16)(x0 + 39), LCD_H - 1, colors[i]);
    }
}

static void draw_grayscale(void)
{
    int n;

    for (n = 0; n < 24; n++) {
        u16 x0 = (u16)(n * 10);
        u8 level = (u8)((n * 255) / 23);
        lcd_fill(x0, 0, (u16)(x0 + 9), LCD_H - 1, LCD_COLOR(level, level, level));
    }
}

static void draw_clear_button(void)
{
    lcd_fill(BTN_X, BTN_Y, LCD_W - 1, LCD_H - 1, LCD_GRAY);
    lcd_text(BTN_X + 5, BTN_Y + 7, "Clear", LCD_BLACK, LCD_GRAY, 1);
}

/* One brush stamp: a BRUSH_W square, clipped to the panel. */
static void brush_stamp(u16 x, u16 y)
{
    int x0 = (int)x - BRUSH_LO;
    int y0 = (int)y - BRUSH_LO;
    int x1 = (int)x + BRUSH_HI;
    int y1 = (int)y + BRUSH_HI;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= LCD_W) x1 = LCD_W - 1;
    if (y1 >= LCD_H) y1 = LCD_H - 1;
    if (x0 <= x1 && y0 <= y1)
        lcd_fill((u16)x0, (u16)y0, (u16)x1, (u16)y1, BRUSH_COLOR);
}

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/*
 * Mostly horizontal segment: one fill per row. A 50 px horizontal stroke
 * needs 5 window setups instead of ~25 square stamps.
 */
static void brush_row_spans(u16 x0, u16 y0, u16 x1, u16 y1)
{
    int dx = (int)x1 - (int)x0;
    int dy = (int)y1 - (int)y0;
    int minx = x0 < x1 ? (int)x0 : (int)x1;
    int maxx = x0 > x1 ? (int)x0 : (int)x1;
    int miny = y0 < y1 ? (int)y0 : (int)y1;
    int maxy = y0 > y1 ? (int)y0 : (int)y1;
    int y;

    for (y = miny - BRUSH_LO; y <= maxy + BRUSH_HI; y++) {
        int lo_y = y - BRUSH_LO;
        int hi_y = y + BRUSH_HI;
        int xa;
        int xb;
        int lo;
        int hi;

        if (y < 0 || y >= LCD_H)
            continue;
        if (lo_y < miny)
            lo_y = miny;
        if (hi_y > maxy)
            hi_y = maxy;

        if (dy == 0) {
            xa = minx;
            xb = maxx;
        } else {
            int a = (int)x0 + ((lo_y - (int)y0) * dx) / dy;
            int b = (int)x0 + ((hi_y - (int)y0) * dx) / dy;

            xa = a < b ? a : b;
            xb = a > b ? a : b;
        }

        lo = xa - BRUSH_LO;
        hi = xb + BRUSH_HI;
        if (lo < 0)
            lo = 0;
        if (hi >= LCD_W)
            hi = LCD_W - 1;
        if (lo <= hi)
            lcd_fill((u16)lo, (u16)y, (u16)hi, (u16)y, BRUSH_COLOR);
    }
}

/* Mostly vertical segment: one fill per column. */
static void brush_col_spans(u16 x0, u16 y0, u16 x1, u16 y1)
{
    int dx = (int)x1 - (int)x0;
    int dy = (int)y1 - (int)y0;
    int minx = x0 < x1 ? (int)x0 : (int)x1;
    int maxx = x0 > x1 ? (int)x0 : (int)x1;
    int miny = y0 < y1 ? (int)y0 : (int)y1;
    int maxy = y0 > y1 ? (int)y0 : (int)y1;
    int x;

    for (x = minx - BRUSH_LO; x <= maxx + BRUSH_HI; x++) {
        int lo_x = x - BRUSH_LO;
        int hi_x = x + BRUSH_HI;
        int ya;
        int yb;
        int lo;
        int hi;

        if (x < 0 || x >= LCD_W)
            continue;
        if (lo_x < minx)
            lo_x = minx;
        if (hi_x > maxx)
            hi_x = maxx;

        if (dx == 0) {
            ya = miny;
            yb = maxy;
        } else {
            int a = (int)y0 + ((lo_x - (int)x0) * dy) / dx;
            int b = (int)y0 + ((hi_x - (int)x0) * dy) / dx;

            ya = a < b ? a : b;
            yb = a > b ? a : b;
        }

        lo = ya - BRUSH_LO;
        hi = yb + BRUSH_HI;
        if (lo < 0)
            lo = 0;
        if (hi >= LCD_H)
            hi = LCD_H - 1;
        if (lo <= hi)
            lcd_fill((u16)x, (u16)lo, (u16)x, (u16)hi, BRUSH_COLOR);
    }
}

/*
 * A 2 px brush is too thin to hide the gaps between 2 px stamps, so every
 * segment is rasterized as contiguous row or column runs. Choose the axis
 * with fewer runs.
 */
static void brush_line(u16 x0, u16 y0, u16 x1, u16 y1)
{
    int adx = iabs((int)x1 - (int)x0);
    int ady = iabs((int)y1 - (int)y0);

    /* Exact horizontal/vertical segment: the whole line is one rect. */
    if (y0 == y1) {
        int lx = (x0 < x1 ? (int)x0 : (int)x1) - BRUSH_LO;
        int hx = (x0 > x1 ? (int)x0 : (int)x1) + BRUSH_HI;
        int ly = (int)y0 - BRUSH_LO;
        int hy = (int)y0 + BRUSH_HI;

        if (lx < 0) lx = 0;
        if (hx >= LCD_W) hx = LCD_W - 1;
        if (ly < 0) ly = 0;
        if (hy >= LCD_H) hy = LCD_H - 1;
        lcd_fill((u16)lx, (u16)ly, (u16)hx, (u16)hy, BRUSH_COLOR);
        return;
    }

    if (x0 == x1) {
        int lx = (int)x0 - BRUSH_LO;
        int hx = (int)x0 + BRUSH_HI;
        int ly = (y0 < y1 ? (int)y0 : (int)y1) - BRUSH_LO;
        int hy = (y0 > y1 ? (int)y0 : (int)y1) + BRUSH_HI;

        if (lx < 0) lx = 0;
        if (hx >= LCD_W) hx = LCD_W - 1;
        if (ly < 0) ly = 0;
        if (hy >= LCD_H) hy = LCD_H - 1;
        lcd_fill((u16)lx, (u16)ly, (u16)hx, (u16)hy, BRUSH_COLOR);
        return;
    }

    if (adx >= ady)
        brush_row_spans(x0, y0, x1, y1);
    else
        brush_col_spans(x0, y0, x1, y1);
}

static void enter_state(u32 st)
{
    g_state = st;
    g_state_start = now_ms();

    switch (st) {
    case ST_TEXT:
        draw_text_screen();
        break;
    case ST_COLOR_FULL:
        g_color_idx = 0;
        fill_screen(LCD_RED);
        break;
    case ST_COLOR_BAR:
        draw_color_bars();
        break;
    case ST_GRAY:
        draw_grayscale();
        break;
    case ST_DRAW:
        fill_screen(LCD_BLACK);
        draw_clear_button();
        break;
    }
}

void lcd_demo_run(void)
{
    static const u16 full_colors[5] = {
        LCD_RED, LCD_GREEN, LCD_BLUE, LCD_WHITE, LCD_BLACK
    };
    u16 last_x = 0;
    u16 last_y = 0;
    int smooth_x = 0;
    int smooth_y = 0;
    int have_last = 0;
    int touching = 0;

    enter_state(ST_TEXT);

    for (;;) {
        u32 now = now_ms();

        switch (g_state) {
        case ST_TEXT:
            if (now - g_state_start >= TEXT_MS)
                enter_state(ST_COLOR_FULL);
            break;

        case ST_COLOR_FULL:
            if (now - g_state_start >= COLOR_MS) {
                g_color_idx++;
                if (g_color_idx >= 5) {
                    enter_state(ST_COLOR_BAR);
                } else {
                    fill_screen(full_colors[g_color_idx]);
                    g_state_start = now;
                }
            }
            break;

        case ST_COLOR_BAR:
            if (now - g_state_start >= BARS_MS)
                enter_state(ST_GRAY);
            break;

        case ST_GRAY:
            if (now - g_state_start >= GRAY_MS)
                enter_state(ST_DRAW);
            break;

        case ST_DRAW: {
            u16 x = 0;
            u16 y = 0;
            int ret = touch_read(&x, &y);

            if (ret > 0) {
                if (x >= BTN_X && y >= BTN_Y) {
                    fill_screen(LCD_BLACK);
                    draw_clear_button();
                    have_last = 0;
                    touching = 0;
                } else {
                    u16 px;
                    u16 py;

                    /*
                     * One-pole smoothing at the 5 ms report rate. The raw
                     * FT6236 coordinates jitter by a pixel or two; drawing
                     * the smoothed point removes most of the staircase.
                     */
                    if (!have_last) {
                        smooth_x = x;
                        smooth_y = y;
                    } else {
                        smooth_x = (smooth_x + (int)x) / 2;
                        smooth_y = (smooth_y + (int)y) / 2;
                    }
                    px = (u16)smooth_x;
                    py = (u16)smooth_y;

                    if (!have_last)
                        brush_stamp(px, py);
                    else if (px != last_x || py != last_y)
                        brush_line(last_x, last_y, px, py);

                    last_x = px;
                    last_y = py;
                    have_last = 1;
                    touching = 1;
                }
            } else if (ret == 0 && touching) {
                touching = 0;
                have_last = 0;
            }
            break;
        }
        }

        /* The FT6236 reports every 10 ms; don't add latency while drawing. */
        if (g_state == ST_DRAW)
            DELAY_MS(touching ? 4 : 16);
        else
            DELAY_MS(16);
    }
}
