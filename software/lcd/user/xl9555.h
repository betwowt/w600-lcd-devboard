/*
 * XL9555 16-bit I2C GPIO expander driver (software I2C).
 *
 * Board wiring (W600 module dev board):
 *   XL9555_SCL = PB09   (no hardware I2C SCL mux on this pin)
 *   XL9555_SDA = PB12
 *   XL9555_INT = PB08   (not used here)
 *   A2 = 1 (R16 to 3.3V), A1 = 0, A0 = 0  ->  7-bit address 0x24
 *
 * Port assignment:
 *   P0_0 LCD_RST   P0_1 TP_RST   P0_2 LCD_PWR   P0_3 LCD_DC
 *   P0_4..7 KEY0..KEY3 (inputs)
 *   P1_0..2 LED_R/G/B (active low)  P1_3..7 unused
 */
#ifndef XL9555_H
#define XL9555_H

#include "board.h"

#define XL9555_ADDR_DEFAULT 0x24

#define XL9555_P0_LCD_RST   (1u << 0)
#define XL9555_P0_TP_RST    (1u << 1)
#define XL9555_P0_LCD_PWR   (1u << 2)
#define XL9555_P0_LCD_DC    (1u << 3)
#define XL9555_P0_KEY0      (1u << 4)
#define XL9555_P0_KEY1      (1u << 5)
#define XL9555_P0_KEY2      (1u << 6)
#define XL9555_P0_KEY3      (1u << 7)

#define XL9555_P1_LED_R     (1u << 0)
#define XL9555_P1_LED_G     (1u << 1)
#define XL9555_P1_LED_B     (1u << 2)

/* Register map */
#define XL9555_REG_IN0      0x00
#define XL9555_REG_IN1      0x01
#define XL9555_REG_OUT0     0x02
#define XL9555_REG_OUT1     0x03
#define XL9555_REG_POL0     0x04
#define XL9555_REG_POL1     0x05
#define XL9555_REG_CFG0     0x06
#define XL9555_REG_CFG1     0x07

/* Configure pins, power the panel, assert reset. Returns 0 on success. */
int xl9555_init(void);
void xl9555_diag(void);

/* Reset the touch controller through P0_1 (10 ms low, 120 ms high). */
int xl9555_touch_reset(void);

/* Update output port 0/1 latches. Only changed bytes are written. */
int xl9555_p0_patch(u8 mask, u8 value);
int xl9555_p1_patch(u8 mask, u8 value);

int xl9555_write_regs(u8 reg, const u8 *buf, u8 len);
int xl9555_read_regs(u8 reg, u8 *buf, u8 len);

u8 xl9555_p0_value(void);
u8 xl9555_p1_value(void);
u8 xl9555_address(void);

#endif /* XL9555_H */
