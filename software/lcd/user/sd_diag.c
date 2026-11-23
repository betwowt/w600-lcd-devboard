/*
 * SD-card bring-up probe.
 *
 * This intentionally bypasses the W600 SPI controller: PA01/PB18/PB17/PB16
 * are driven as plain GPIO in SPI mode 0. If CMD0 still returns 0xFF here,
 * the card/socket is the problem rather than the host SPI read path.
 */
#include "board.h"
#include "sd_diag.h"

#define D_CS    WM_IO_PB_16
#define D_CLK   WM_IO_PA_01
#define D_MOSI  WM_IO_PB_18
#define D_MISO  WM_IO_PB_17

static void dly(void)
{
    /* SD initialization must stay at or below 400 kHz. */
    volatile int i = 80;
    while (i-- > 0) {
    }
}

static void d_pins(void)
{
    tls_gpio_cfg(D_CS, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    tls_gpio_write(D_CS, 1);
    tls_gpio_cfg(D_CLK, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(D_CLK, 0);
    tls_gpio_cfg(D_MOSI, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(D_MOSI, 1);
    tls_gpio_cfg(D_MISO, WM_GPIO_DIR_INPUT, WM_GPIO_ATTR_PULLHIGH);
}

static u8 d_xfer(u8 out)
{
    u8 in = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        tls_gpio_write(D_MOSI, (out >> i) & 1);
        dly();
        tls_gpio_write(D_CLK, 1);
        dly();
        in = (u8)((in << 1) | (tls_gpio_read(D_MISO) ? 1 : 0));
        tls_gpio_write(D_CLK, 0);
        dly();
    }
    return in;
}

static u8 d_cmd(u8 cmd, u32 arg, u8 crc)
{
    u8 r = 0xFF;
    int i;

    tls_gpio_write(D_CS, 0);
    d_xfer((u8)(0x40 | cmd));
    d_xfer((u8)(arg >> 24));
    d_xfer((u8)(arg >> 16));
    d_xfer((u8)(arg >> 8));
    d_xfer((u8)arg);
    d_xfer(crc);
    for (i = 0; i < 16; i++) {
        r = d_xfer(0xFF);
        if ((r & 0x80) == 0)
            break;
    }
    return r;
}

static void d_deselect(void)
{
    tls_gpio_write(D_CS, 1);
    d_xfer(0xFF);
}

void sd_diag(void)
{
    u8 r;
    u8 p[4];
    int i;

    printf("SDDIAG: pins CS=PB16 CLK=PA1 MOSI=PB18 MISO=PB17\r\n");

    d_pins();
    printf("SDDIAG: idle CS=%d CLK=%d MOSI=%d MISO=%d\r\n",
           tls_gpio_read(D_CS), tls_gpio_read(D_CLK),
           tls_gpio_read(D_MOSI), tls_gpio_read(D_MISO));

    for (i = 0; i < 10; i++)
        d_xfer(0xFF);
    DELAY_MS(5);

    r = d_cmd(0, 0, 0x95);
    d_deselect();
    printf("SDDIAG: CMD0 -> %02X\r\n", r);

    if (r != 0x01) {
        printf("SDDIAG: no SPI-mode response (card seated/power?)\r\n");
        return;
    }

    r = d_cmd(8, 0x1AA, 0x87);
    if (r == 0x01) {
        for (i = 0; i < 4; i++)
            p[i] = d_xfer(0xFF);
        printf("SDDIAG: CMD8 -> %02X %02X %02X %02X %02X\r\n",
               r, p[0], p[1], p[2], p[3]);
    } else {
        printf("SDDIAG: CMD8 -> %02X\r\n", r);
    }
    d_deselect();

    for (i = 0; i < 20; i++) {
        d_cmd(55, 0, 0x65);
        d_deselect();
        r = d_cmd(41, 0x40000000u, 0x77);
        d_deselect();
        if (r == 0x00)
            break;
        if (r != 0x01)
            break;
        DELAY_MS(10);
    }
    printf("SDDIAG: ACMD41 -> %02X after %d tries\r\n", r, i + 1);
    if (r != 0x00)
        return;

    r = d_cmd(58, 0, 0xFD);
    if (r == 0x00) {
        for (i = 0; i < 4; i++)
            p[i] = d_xfer(0xFF);
        printf("SDDIAG: OCR -> %02X %02X %02X %02X\r\n",
               p[0], p[1], p[2], p[3]);
    }
    d_deselect();
}
