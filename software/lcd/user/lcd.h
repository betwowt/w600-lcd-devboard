/*
 * ILI9341 240x320 SPI panel on the W600 module dev board.
 *
 *   SPI_CLK  PA01  (SPI CK, option 3)
 *   SPI_MOSI PB18  (SPI DO, option 3)
 *   LCD_CS   PB11  (plain GPIO, active low)
 *   LCD_DC   XL9555 P0_3
 *   LCD_RST  XL9555 P0_0
 *   LCD_PWR  XL9555 P0_2
 *
 * The external W25Q64 shares CLK/MOSI/MISO; its CS (PB15) is kept high.
 */
#ifndef LCD_PANEL_H
#define LCD_PANEL_H

#include "board.h"

#define LCD_W 240
#define LCD_H 320

#define LCD_COLOR(r, g, b) \
    ((u16)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define LCD_BLACK   0x0000
#define LCD_WHITE   0xFFFF
#define LCD_RED     0xF800
#define LCD_GREEN   0x07E0
#define LCD_BLUE    0x001F
#define LCD_YELLOW  0xFFE0
#define LCD_CYAN    0x07FF
#define LCD_MAGENTA 0xF81F
#define LCD_GRAY    0x8410
#define LCD_ORANGE  0xFD20

#define LCD_SPI_HZ  20000000u

/* Returns 0 on success, -1 if the XL9555 (panel control) is unreachable. */
int lcd_init(void);

/* Full XL9555 + panel re-init with LCD_PWR driven high (1) or low (0). */
int lcd_init_pwr(u8 pwr_on);

/* Restore the master SPI mux after SD bit-bang used PA01/PB18/PB17. */
int lcd_spi_resume(void);

void lcd_fill(u16 x0, u16 y0, u16 x1, u16 y1, u16 color);
void lcd_image_begin(u16 x, u16 y, u16 w, u16 h);
void lcd_image_data(const u8 *buf, u32 len);
void lcd_draw_pixel(u16 x, u16 y, u16 color);
void lcd_draw_hline(u16 x0, u16 x1, u16 y, u16 color);
void lcd_draw_vline(u16 x, u16 y0, u16 y1, u16 color);
void lcd_draw_rect(u16 x0, u16 y0, u16 x1, u16 y1, u16 color);

void lcd_text(u16 x, u16 y, const char *s, u16 fg, u16 bg, u8 scale);

#endif /* LCD_PANEL_H */
