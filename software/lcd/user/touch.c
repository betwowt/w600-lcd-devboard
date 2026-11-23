/*
 * FT6236 / CST816 touch controller on the LCD FPC.
 *
 * TP_RST  = XL9555 P0_1
 * TP_INT  = PB10 (not used, polling)
 * I2C_SCL = PB13, I2C_SDA = PB14
 *
 * Both controllers use the same first-point layout:
 *   0x02 = touch count, 0x03..0x06 = XH/XL/YH/YL.
 */
#include "board.h"
#include "touch.h"
#include "xl9555.h"
#include "soft_i2c.h"
#include "lcd.h"

#define TOUCH_SCL       WM_IO_PB_13
#define TOUCH_SDA       WM_IO_PB_14

#define FT6236_ADDR     0x38
#define CST816_ADDR     0x15

#define REG_TD_STATUS   0x02
#define REG_TOUCH1_XH   0x03
#define REG_FT_CHIPID   0xA3
#define REG_FT_VENDOR   0xA8

/* Set to 1 if the panel reports coordinates mirrored on this build. */
#define TOUCH_MIRROR_X  0
#define TOUCH_MIRROR_Y  0

static u8 g_touch_addr;

/* 0xFF means the read NACKed (or the register really is 0xFF). */
static u8 touch_read_reg(u8 reg)
{
    u8 v = 0;

    if (si2c_read_regs_stop(g_touch_addr, reg, &v, 1) != 0)
        return 0xFF;
    return v;
}

static int touch_write_reg(u8 reg, u8 val)
{
    return si2c_write_regs(g_touch_addr, reg, &val, 1);
}

int touch_init(void)
{
    if (xl9555_touch_reset() != 0) {
        printf("TOUCH: TP_RST failed\r\n");
        return -1;
    }

    si2c_set_speed(60);
    si2c_set_pins(TOUCH_SCL, TOUCH_SDA);

    if (si2c_probe(FT6236_ADDR) == 0) {
        u8 id;
        u8 vendor;

        g_touch_addr = FT6236_ADDR;
        id = touch_read_reg(REG_FT_CHIPID);
        vendor = touch_read_reg(REG_FT_VENDOR);

        /* Same init sequence as the STM32 reference driver. */
        touch_write_reg(0x80, 128);   /* threshold */
        DELAY_MS(5);
        touch_write_reg(0xA4, 1);     /* trigger mode */
        DELAY_MS(5);
        touch_write_reg(0x88, 5);     /* 5 ms active report rate */
        DELAY_MS(5);

        printf("TOUCH: FT6236 id=%02X vendor=%02X A4=%02X 88=%02X\r\n",
               id, vendor, touch_read_reg(0xA4), touch_read_reg(0x88));
        return 0;
    }

    if (si2c_probe(CST816_ADDR) == 0) {
        g_touch_addr = CST816_ADDR;
        printf("TOUCH: CST816 at 0x15\r\n");
        return 0;
    }

    printf("TOUCH: no controller ACK (0x38/0x15)\r\n");
    return -1;
}

int touch_read(u16 *x, u16 *y)
{
    u8 st;
    u8 d[4];

    if (g_touch_addr == 0)
        return 0;

    /* The LCD DC line shares the I2C helper and leaves the XL bus selected. */
    si2c_set_speed(60);
    si2c_set_pins(TOUCH_SCL, TOUCH_SDA);

    if (si2c_read_regs_stop(g_touch_addr, REG_TD_STATUS, &st, 1) != 0)
        return -1;

    if ((st & 0x0F) == 0)
        return 0;

    if (si2c_read_regs_stop(g_touch_addr, REG_TOUCH1_XH, d, 4) != 0)
        return -1;

    *x = (u16)(((d[0] & 0x0F) << 8) | d[1]);
    *y = (u16)(((d[2] & 0x0F) << 8) | d[3]);

#if TOUCH_MIRROR_X
    *x = (u16)(LCD_W - 1 - *x);
#endif
#if TOUCH_MIRROR_Y
    *y = (u16)(LCD_H - 1 - *y);
#endif

    if (*x >= LCD_W)
        *x = LCD_W - 1;
    if (*y >= LCD_H)
        *y = LCD_H - 1;

    return 1;
}
