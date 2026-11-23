/*
 * XL9555 16-bit I2C GPIO expander.
 *
 * Board wiring:
 *   XL9555_SCL = PB09, XL9555_SDA = PB12
 *   A2 = 1 (R16 to 3.3V), A1 = 0, A0 = 0 -> address 0x24
 *
 * P0_0 LCD_RST, P0_1 TP_RST, P0_2 LCD_PWR, P0_3 LCD_DC
 * P1_0..2 LED_R/G/B (active low)
 */
#include "board.h"
#include "xl9555.h"
#include "soft_i2c.h"

#define XL_SCL  WM_IO_PB_09
#define XL_SDA  WM_IO_PB_12
#define XL_SPEED 30

static void xl_select(void)
{
    si2c_set_speed(XL_SPEED);
    si2c_set_pins(XL_SCL, XL_SDA);
}

static u8 g_addr = XL9555_ADDR_DEFAULT;
static u8 g_out0;
static u8 g_out1;
static u8 g_cfg0 = 0xF0;   /* P0_0..3 outputs, P0_4..7 keys */
static u8 g_cfg1 = 0xF8;   /* P1_0..2 LED outputs */

u8 xl9555_address(void)
{
    return g_addr;
}

u8 xl9555_p0_value(void)
{
    return g_out0;
}

u8 xl9555_p1_value(void)
{
    return g_out1;
}

int xl9555_write_regs(u8 reg, const u8 *buf, u8 len)
{
    xl_select();
    return si2c_write_regs(g_addr, reg, buf, len);
}

int xl9555_read_regs(u8 reg, u8 *buf, u8 len)
{
    xl_select();
    return si2c_read_regs(g_addr, reg, buf, len);
}

/*
 * The expander powers up with every pin configured as an input. A write to
 * OUT0/OUT1 then only changes the latch, not the pin. Make sure the pins
 * touched by this call are outputs before driving them.
 */
static int xl9555_ensure_outputs(u8 cfg_reg, u8 mask, u8 *cfg)
{
    if ((*cfg & mask) == 0)
        return 0;
    *cfg &= (u8)~mask;
    return si2c_write_regs(g_addr, cfg_reg, cfg, 1);
}

int xl9555_p0_patch(u8 mask, u8 value)
{
    u8 v;

    xl_select();

    if (xl9555_ensure_outputs(XL9555_REG_CFG0, mask, &g_cfg0) != 0)
        return -1;

    v = (u8)((g_out0 & ~mask) | (value & mask));
    if (v == g_out0)
        return 0;
    if (si2c_write_regs(g_addr, XL9555_REG_OUT0, &v, 1) != 0)
        return -1;
    g_out0 = v;
    return 0;
}

int xl9555_p1_patch(u8 mask, u8 value)
{
    u8 v;

    xl_select();

    if (xl9555_ensure_outputs(XL9555_REG_CFG1, mask, &g_cfg1) != 0)
        return -1;

    v = (u8)((g_out1 & ~mask) | (value & mask));
    if (v == g_out1)
        return 0;
    if (si2c_write_regs(g_addr, XL9555_REG_OUT1, &v, 1) != 0)
        return -1;
    g_out1 = v;
    return 0;
}

int xl9555_touch_reset(void)
{
    u8 cfg = 0;

    xl_select();

    /*
     * Power-on default is all inputs. Read the real CFG0 (the shadow may be
     * stale after a rail dip) and make P0_1 an output before pulsing it.
     */
    if (si2c_read_regs(g_addr, XL9555_REG_CFG0, &cfg, 1) == 0) {
        if (cfg & XL9555_P0_TP_RST) {
            cfg &= (u8)~XL9555_P0_TP_RST;
            if (si2c_write_regs(g_addr, XL9555_REG_CFG0, &cfg, 1) != 0)
                return -1;
        }
        g_cfg0 = cfg;
    }

    if (xl9555_p0_patch(XL9555_P0_TP_RST, 0) != 0)
        return -1;
    DELAY_MS(10);
    if (xl9555_p0_patch(XL9555_P0_TP_RST, XL9555_P0_TP_RST) != 0)
        return -1;
    DELAY_MS(120);

    return 0;
}

static int xl9555_find(u8 *addr)
{
    u8 a;

    for (a = 0x20; a <= 0x27; a++) {
        if (si2c_probe(a) == 0) {
            *addr = a;
            return 0;
        }
    }
    return -1;
}

void xl9555_diag(void)
{
    xl_select();
    printf("I2C XL9555: idle SCL=%d SDA=%d\r\n",
           si2c_scl_level(), si2c_sda_level());
    si2c_pin_check("I2C XL9555");
    si2c_scan(0x20, 0x27, "I2C XL9555");

    si2c_set_pins(XL_SDA, XL_SCL);
    si2c_scan(0x20, 0x27, "I2C XL9555 swapped");

    si2c_set_pins(WM_IO_PB_13, WM_IO_PB_14);
    printf("I2C touch: idle SCL=%d SDA=%d\r\n",
           si2c_scl_level(), si2c_sda_level());
    si2c_pin_check("I2C touch");
    si2c_scan(0x08, 0x77, "I2C touch");

    si2c_set_pins(WM_IO_PB_14, WM_IO_PB_13);
    si2c_scan(0x08, 0x77, "I2C touch swapped");

    xl_select();
}

int xl9555_init(void)
{
    u8 cfg[2];

    printf("XL9555: driver rev4\r\n");

    xl_select();
    printf("XL9555: idle SCL=%d SDA=%d\r\n",
           si2c_scl_level(), si2c_sda_level());

    if (!si2c_scl_level() || !si2c_sda_level()) {
        si2c_bus_recover();
        printf("XL9555: recover -> SCL=%d SDA=%d\r\n",
               si2c_scl_level(), si2c_sda_level());
    }

    if (xl9555_find(&g_addr) == 0) {
        printf("XL9555: ACK at 0x%02X (SCL PB9, SDA PB12)\r\n", g_addr);
    } else {
        si2c_set_pins(XL_SDA, XL_SCL);
        if (xl9555_find(&g_addr) == 0) {
            printf("XL9555: ACK at 0x%02X with SWAPPED pins\r\n", g_addr);
        } else {
            xl_select();
            printf("XL9555: no ACK on 0x20..0x27 (normal or swapped)\r\n");
            return -1;
        }
    }

    /* Diagnostic only: writes below must work even if reads are flaky. */
    if (si2c_read_regs(g_addr, XL9555_REG_CFG0, cfg, 1) == 0)
        printf("XL9555: found at 0x%02X, cfg=%02X\r\n", g_addr, cfg[0]);
    else
        printf("XL9555: ACK at 0x%02X, CFG0 read failed; write-only init\r\n", g_addr);

    /*
     * Power-on default is all inputs. Configure the directions first, then
     * load the output latches; writing a latch first would not drive a pin
     * that is still an input.
     */
    cfg[0] = 0xF0;   /* P0_0..3 outputs, P0_4..7 keys */
    cfg[1] = 0xF8;   /* P1_0..2 LED outputs */
    if (si2c_write_regs(g_addr, XL9555_REG_CFG0, cfg, 2) != 0) {
        printf("XL9555: CFG write failed\r\n");
        return -1;
    }
    g_cfg0 = cfg[0];
    g_cfg1 = cfg[1];

    g_out0 = XL9555_P0_LCD_PWR;                       /* TP_RST low, panel on, DC low */
    g_out1 = XL9555_P1_LED_R | XL9555_P1_LED_G | XL9555_P1_LED_B;

    if (si2c_write_regs(g_addr, XL9555_REG_OUT0, &g_out0, 1) != 0) {
        printf("XL9555: OUT0 write failed\r\n");
        return -1;
    }
    if (si2c_write_regs(g_addr, XL9555_REG_OUT1, &g_out1, 1) != 0) {
        printf("XL9555: OUT1 write failed\r\n");
        return -1;
    }

    DELAY_MS(10);
    return 0;
}
