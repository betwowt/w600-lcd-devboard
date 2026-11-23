/*
 * ILI9341 240x320 panel driver for the W600 module dev board.
 *
 * Panel control lines (RST/DC/PWR) are on the XL9555 I2C expander;
 * SPI is the W600 master SPI in byte mode, with a GPIO chip select.
 */
#include "board.h"
#include "wm_gpio_afsel.h"
#include "wm_hostspi.h"
#include "lcd.h"
#include "xl9555.h"
#include "font8x16.h"
#include "spi_bus.h"


#define LCD_CS_PIN      WM_IO_PB_11
#define LCD_CS_LOW()    ((void)spi_bus_cs_low(SPI_DEV_LCD))
#define LCD_CS_HIGH()   ((void)spi_bus_cs_high(SPI_DEV_LCD))

#define LCD_DC_CMD      0
#define LCD_DC_DATA     XL9555_P0_LCD_DC

static u8 fill_buf[4096];
static u8 text_buf[2048];

struct lcd_init_cmd {
    u8 cmd;
    u8 len;
    u8 data[16];
};

static const struct lcd_init_cmd ili9341_init_seq[] = {
    {0x11, 0, {0}},                                             /* sleep out */
    {0xCF, 3, {0x00, 0xD9, 0x30}},
    {0xED, 4, {0x64, 0x03, 0x12, 0x81}},
    {0xE8, 3, {0x85, 0x10, 0x78}},
    {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}},
    {0xF7, 1, {0x20}},
    {0xEA, 2, {0x00, 0x00}},
    {0xC0, 1, {0x21}},                                          /* power control */
    {0xC1, 1, {0x12}},
    {0xC5, 2, {0x32, 0x3C}},
    {0xC7, 1, {0xC1}},
    {0x36, 1, {0x08}},                                          /* MADCTL: BGR, portrait */
    {0x3A, 1, {0x55}},                                          /* RGB565 */
    {0xB1, 2, {0x00, 0x18}},
    {0xB6, 2, {0x0A, 0xA2}},
    {0xF2, 1, {0x00}},
    {0x26, 1, {0x01}},
};

static const u8 gamma_p[15] = {
    0x0F, 0x20, 0x1E, 0x09, 0x12, 0x0B, 0x50, 0xBA,
    0x44, 0x09, 0x14, 0x05, 0x23, 0x21, 0x00
};
static const u8 gamma_n[15] = {
    0x00, 0x19, 0x19, 0x00, 0x12, 0x07, 0x2D, 0x28,
    0x3F, 0x02, 0x0A, 0x08, 0x25, 0x2D, 0x0F
};

static void lcd_spi_send(const u8 *buf, u32 len)
{
    if (len)
        tls_spi_write(buf, len);
}

static void lcd_cmd_params(u8 cmd, const u8 *params, u8 len)
{
    xl9555_p0_patch(XL9555_P0_LCD_DC, LCD_DC_CMD);
    LCD_CS_LOW();
    tls_spi_write(&cmd, 1);
    if (len) {
        xl9555_p0_patch(XL9555_P0_LCD_DC, LCD_DC_DATA);
        lcd_spi_send(params, len);
    }
    LCD_CS_HIGH();
}

static void lcd_cmd(u8 cmd)
{
    lcd_cmd_params(cmd, 0, 0);
}

static void lcd_data_buf(const u8 *buf, u32 len)
{
    xl9555_p0_patch(XL9555_P0_LCD_DC, LCD_DC_DATA);
    LCD_CS_LOW();
    lcd_spi_send(buf, len);
    LCD_CS_HIGH();
}

static void lcd_data16(u16 v)
{
    u8 b[2];

    b[0] = (u8)(v >> 8);
    b[1] = (u8)v;
    lcd_data_buf(b, 2);
}

/* Column/row window, followed by memory write. */
static void lcd_set_window(u16 x0, u16 y0, u16 x1, u16 y1)
{
    u8 p[4];

    p[0] = (u8)(x0 >> 8);
    p[1] = (u8)x0;
    p[2] = (u8)(x1 >> 8);
    p[3] = (u8)x1;
    lcd_cmd_params(0x2A, p, 4);

    p[0] = (u8)(y0 >> 8);
    p[1] = (u8)y0;
    p[2] = (u8)(y1 >> 8);
    p[3] = (u8)y1;
    lcd_cmd_params(0x2B, p, 4);

    lcd_cmd(0x2C);
}

static void lcd_panel_reset(void)
{
    xl9555_p0_patch(XL9555_P0_LCD_RST, 0);
    DELAY_MS(20);
    xl9555_p0_patch(XL9555_P0_LCD_RST, XL9555_P0_LCD_RST);
    DELAY_MS(120);
}

static void lcd_panel_init(void)
{
    u32 i;

    for (i = 0; i < sizeof(ili9341_init_seq) / sizeof(ili9341_init_seq[0]); i++) {
        const struct lcd_init_cmd *c = &ili9341_init_seq[i];

        lcd_cmd_params(c->cmd, c->data, c->len);
        if (c->cmd == 0x11)
            DELAY_MS(120);
    }

    lcd_cmd_params(0xE0, gamma_p, (u8)sizeof(gamma_p));
    lcd_cmd_params(0xE1, gamma_n, (u8)sizeof(gamma_n));
    lcd_cmd(0x29);                                              /* display on */
}

int lcd_spi_resume(void)
{
    u32 spi_hz = LCD_SPI_HZ;
    int ret;

    /*
     * The SD fallback bit-bangs PA01/PB18/PB17 as GPIO. Restore the master
     * SPI mux before any LCD write.
     */
    wm_spi_ck_config(WM_IO_PA_01);
    wm_spi_do_config(WM_IO_PB_18);
    wm_spi_di_config(WM_IO_PB_17);
    wm_spi_cs_config(WM_IO_PB_00);   /* keep hspi_cs off PB15 */
    tls_spi_trans_type(SPI_DMA_TRANSFER);
    ret = tls_spi_setup(TLS_SPI_MODE_0, TLS_SPI_CS_LOW, spi_hz);
    if (ret != 0) {
        spi_hz = 10000000u;
        ret = tls_spi_setup(TLS_SPI_MODE_0, TLS_SPI_CS_LOW, spi_hz);
    }
    return ret;
}

static int lcd_init_pwr_locked(u8 pwr_on)
{
    int ret;

    ret = xl9555_init();
    if (ret != 0)
        return ret;

    /* xl9555_init() defaults PWR high; override before reset/init. */
    xl9555_p0_patch(XL9555_P0_LCD_PWR,
                    pwr_on ? XL9555_P0_LCD_PWR : 0);
    DELAY_MS(20);

    ret = lcd_spi_resume();
    if (ret != 0) {
        printf("LCD: SPI setup failed (%d)\r\n", ret);
        return ret;
    }
    printf("LCD: SPI ready (%u Hz, mode 0), PWR=%u\r\n",
           (unsigned)LCD_SPI_HZ, (unsigned)pwr_on);

    lcd_panel_reset();
    lcd_panel_init();
    printf("LCD: ILI9341 init done (PWR=%u)\r\n", (unsigned)pwr_on);
    return 0;
}

static int lcd_init_pwr_op(void *arg)
{
    return lcd_init_pwr_locked((u8)(u32)arg);
}

int lcd_init_pwr(u8 pwr_on)
{
    return spi_bus_run(SPI_DEV_LCD, lcd_init_pwr_op, (void *)(u32)pwr_on);
}

int lcd_init(void)
{
    int ret = lcd_init_pwr(1);

    if (ret != 0)
        return ret;
    lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, LCD_BLACK);
    return 0;
}

static void lcd_fill_locked(u16 x0, u16 y0, u16 x1, u16 y1, u16 color)
{
    u32 total;
    u32 n;
    u32 i;

    if (x0 >= LCD_W || y0 >= LCD_H)
        return;
    if (x1 >= LCD_W)
        x1 = LCD_W - 1;
    if (y1 >= LCD_H)
        y1 = LCD_H - 1;
    if (x1 < x0 || y1 < y0)
        return;

    lcd_set_window(x0, y0, x1, y1);

    total = (u32)(x1 - x0 + 1) * (u32)(y1 - y0 + 1) * 2u;
    n = total > sizeof(fill_buf) ? sizeof(fill_buf) : total;
    for (i = 0; i < n; i += 2) {
        fill_buf[i] = (u8)(color >> 8);
        fill_buf[i + 1] = (u8)color;
    }

    xl9555_p0_patch(XL9555_P0_LCD_DC, LCD_DC_DATA);
    LCD_CS_LOW();
    while (total) {
        u32 chunk = total > n ? n : total;
        tls_spi_write(fill_buf, chunk);
        total -= chunk;
    }
    LCD_CS_HIGH();
}

struct lcd_fill_arg {
    u16 x0, y0, x1, y1, color;
};

static int lcd_fill_op(void *arg)
{
    struct lcd_fill_arg *a = (struct lcd_fill_arg *)arg;

    lcd_fill_locked(a->x0, a->y0, a->x1, a->y1, a->color);
    return 0;
}

void lcd_fill(u16 x0, u16 y0, u16 x1, u16 y1, u16 color)
{
    struct lcd_fill_arg a = {x0, y0, x1, y1, color};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_fill_op, &a);
}

/*
 * Start a raw RGB565 image (big-endian) at x,y. The window stays open for
 * any number of lcd_image_data() chunks; the ILI9341 address counter
 * continues across chip-select toggles.
 */
static void lcd_image_begin_locked(u16 x, u16 y, u16 w, u16 h)
{
    if (w == 0 || h == 0 || x >= LCD_W || y >= LCD_H)
        return;
    if (x + w > LCD_W)
        w = (u16)(LCD_W - x);
    if (y + h > LCD_H)
        h = (u16)(LCD_H - y);

    lcd_set_window(x, y, (u16)(x + w - 1), (u16)(y + h - 1));
    xl9555_p0_patch(XL9555_P0_LCD_DC, LCD_DC_DATA);
}

struct lcd_image_begin_arg {
    u16 x, y, w, h;
};

static int lcd_image_begin_op(void *arg)
{
    struct lcd_image_begin_arg *a = (struct lcd_image_begin_arg *)arg;

    lcd_image_begin_locked(a->x, a->y, a->w, a->h);
    return 0;
}

void lcd_image_begin(u16 x, u16 y, u16 w, u16 h)
{
    struct lcd_image_begin_arg a = {x, y, w, h};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_image_begin_op, &a);
}

static void lcd_image_data_locked(const u8 *buf, u32 len)
{
    if (len == 0)
        return;

    LCD_CS_LOW();
    tls_spi_write(buf, len);
    LCD_CS_HIGH();
}

struct lcd_image_data_arg {
    const u8 *buf;
    u32 len;
};

static int lcd_image_data_op(void *arg)
{
    struct lcd_image_data_arg *a = (struct lcd_image_data_arg *)arg;

    lcd_image_data_locked(a->buf, a->len);
    return 0;
}

void lcd_image_data(const u8 *buf, u32 len)
{
    struct lcd_image_data_arg a = {buf, len};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_image_data_op, &a);
}

static void lcd_draw_pixel_locked(u16 x, u16 y, u16 color)
{
    if (x >= LCD_W || y >= LCD_H)
        return;
    lcd_set_window(x, y, x, y);
    lcd_data16(color);
}

struct lcd_pixel_arg {
    u16 x, y, color;
};

static int lcd_draw_pixel_op(void *arg)
{
    struct lcd_pixel_arg *a = (struct lcd_pixel_arg *)arg;

    lcd_draw_pixel_locked(a->x, a->y, a->color);
    return 0;
}

void lcd_draw_pixel(u16 x, u16 y, u16 color)
{
    struct lcd_pixel_arg a = {x, y, color};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_draw_pixel_op, &a);
}

static void lcd_draw_hline_locked(u16 x0, u16 x1, u16 y, u16 color)
{
    if (x0 > x1) {
        u16 t = x0;
        x0 = x1;
        x1 = t;
    }
    lcd_fill_locked(x0, y, x1, y, color);
}

struct lcd_line_arg {
    u16 x0, y0, x1, y1, color;
};

static int lcd_draw_hline_op(void *arg)
{
    struct lcd_line_arg *a = (struct lcd_line_arg *)arg;

    lcd_draw_hline_locked(a->x0, a->x1, a->y0, a->color);
    return 0;
}

void lcd_draw_hline(u16 x0, u16 x1, u16 y, u16 color)
{
    struct lcd_line_arg a = {x0, y, x1, 0, color};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_draw_hline_op, &a);
}

static void lcd_draw_vline_locked(u16 x, u16 y0, u16 y1, u16 color)
{
    if (y0 > y1) {
        u16 t = y0;
        y0 = y1;
        y1 = t;
    }
    lcd_fill_locked(x, y0, x, y1, color);
}

static int lcd_draw_vline_op(void *arg)
{
    struct lcd_line_arg *a = (struct lcd_line_arg *)arg;

    lcd_draw_vline_locked(a->x0, a->y0, a->y1, a->color);
    return 0;
}

void lcd_draw_vline(u16 x, u16 y0, u16 y1, u16 color)
{
    struct lcd_line_arg a = {x, y0, 0, y1, color};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_draw_vline_op, &a);
}

static void lcd_draw_rect_locked(u16 x0, u16 y0, u16 x1, u16 y1, u16 color)
{
    lcd_draw_hline_locked(x0, x1, y0, color);
    lcd_draw_hline_locked(x0, x1, y1, color);
    lcd_draw_vline_locked(x0, y0, y1, color);
    lcd_draw_vline_locked(x1, y0, y1, color);
}

static int lcd_draw_rect_op(void *arg)
{
    struct lcd_line_arg *a = (struct lcd_line_arg *)arg;

    lcd_draw_rect_locked(a->x0, a->y0, a->x1, a->y1, a->color);
    return 0;
}

void lcd_draw_rect(u16 x0, u16 y0, u16 x1, u16 y1, u16 color)
{
    struct lcd_line_arg a = {x0, y0, x1, y1, color};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_draw_rect_op, &a);
}

static void lcd_text_locked(u16 x, u16 y, const char *s, u16 fg, u16 bg, u8 scale)
{
    u16 start_x = x;
    u16 cw;
    u16 ch;

    if (scale == 0)
        scale = 1;
    if (scale > 2)
        scale = 2;
    cw = FONT8X16_W * scale;
    ch = FONT8X16_H * scale;

    while (*s) {
        u8 c = (u8)*s++;
        u16 k = 0;
        u8 row, col, ry, rx;

        if (c == '\n') {
            x = start_x;
            y += ch;
            continue;
        }
        if (c < FONT8X16_FIRST || c > FONT8X16_LAST)
            c = '?';
        if (x + cw > LCD_W) {
            x = start_x;
            y += ch;
        }
        if (y + ch > LCD_H)
            break;

        for (row = 0; row < FONT8X16_H; row++) {
            for (ry = 0; ry < scale; ry++) {
                for (col = 0; col < FONT8X16_W; col++) {
                    u16 pixel = (font8x16[c - FONT8X16_FIRST][row] >> col) & 1u
                                    ? fg : bg;
                    for (rx = 0; rx < scale; rx++) {
                        text_buf[k++] = (u8)(pixel >> 8);
                        text_buf[k++] = (u8)pixel;
                    }
                }
            }
        }

        lcd_set_window(x, y, (u16)(x + cw - 1), (u16)(y + ch - 1));
        lcd_data_buf(text_buf, k);
        x += cw;
    }
}

struct lcd_text_arg {
    u16 x, y, fg, bg;
    const char *s;
    u8 scale;
};

static int lcd_text_op(void *arg)
{
    struct lcd_text_arg *a = (struct lcd_text_arg *)arg;

    lcd_text_locked(a->x, a->y, a->s, a->fg, a->bg, a->scale);
    return 0;
}

void lcd_text(u16 x, u16 y, const char *s, u16 fg, u16 bg, u8 scale)
{
    struct lcd_text_arg a = {x, y, fg, bg, s, scale};

    (void)spi_bus_run(SPI_DEV_LCD, lcd_text_op, &a);
}
