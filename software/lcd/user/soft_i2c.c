/*
 * Bit-banged I2C.
 *
 * W600 note: configure the pin as an output *before* writing the data
 * register. Writing first and switching to output afterwards leaves the
 * line high on this SoC.
 */
#include "board.h"
#include "soft_i2c.h"

static u8 g_dly = 60;       /* ~100 kHz with the GPIO register overhead */

static void i2c_dly(void)
{
    volatile int i = g_dly;
    while (i-- > 0) {
    }
}

void si2c_set_speed(u8 dly)
{
    g_dly = dly < 5 ? 5 : dly;
}

static enum tls_io_name g_scl;
static enum tls_io_name g_sda;
static int g_selected;

static void scl_release(void)
{
    tls_gpio_cfg(g_scl, WM_GPIO_DIR_INPUT, WM_GPIO_ATTR_PULLHIGH);
}

static void scl_low(void)
{
    tls_gpio_cfg(g_scl, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(g_scl, 0);
}

static void sda_release(void)
{
    tls_gpio_cfg(g_sda, WM_GPIO_DIR_INPUT, WM_GPIO_ATTR_PULLHIGH);
}

static void sda_low(void)
{
    tls_gpio_cfg(g_sda, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(g_sda, 0);
}

static void i2c_start(void)
{
    sda_release();
    scl_release();
    i2c_dly();
    sda_low();
    i2c_dly();
    scl_low();
    i2c_dly();
}

static void i2c_stop(void)
{
    scl_low();
    sda_low();
    i2c_dly();
    scl_release();
    i2c_dly();
    sda_release();
    i2c_dly();
}

/* returns 0 when the slave acknowledged */
static int i2c_write_byte(u8 b)
{
    int i;
    int ack;

    for (i = 7; i >= 0; i--) {
        if (b & (1u << i))
            sda_release();
        else
            sda_low();
        i2c_dly();
        scl_release();
        i2c_dly();
        scl_low();
        i2c_dly();
    }

    sda_release();
    i2c_dly();
    scl_release();
    i2c_dly();
    ack = tls_gpio_read(g_sda) ? 1 : 0;
    scl_low();
    i2c_dly();

    return ack;
}

static u8 i2c_read_byte(int ack)
{
    u8 b = 0;
    int i;

    sda_release();
    for (i = 7; i >= 0; i--) {
        i2c_dly();
        scl_release();
        i2c_dly();
        if (tls_gpio_read(g_sda))
            b |= (u8)(1u << i);
        scl_low();
        i2c_dly();
    }

    if (ack)
        sda_low();
    else
        sda_release();
    i2c_dly();
    scl_release();
    i2c_dly();
    scl_low();
    i2c_dly();

    return b;
}

void si2c_set_pins(enum tls_io_name scl, enum tls_io_name sda)
{
    /* Every public transaction ends with STOP, so the bus is idle here.
     * The LCD DC line switches bus thousands of times per stroke; avoid
     * reconfiguring both GPIOs when the requested bus is already selected. */
    if (g_selected && scl == g_scl && sda == g_sda)
        return;

    g_scl = scl;
    g_sda = sda;
    g_selected = 1;
    sda_release();
    scl_release();
    /* Called on every DC toggle, so keep this short; the pull-ups settle
     * well within one bit time. */
    i2c_dly();
}

int si2c_scl_level(void)
{
    return tls_gpio_read(g_scl) ? 1 : 0;
}

int si2c_sda_level(void)
{
    return tls_gpio_read(g_sda) ? 1 : 0;
}

int si2c_probe(u8 addr)
{
    int nack;

    i2c_start();
    nack = i2c_write_byte((u8)(addr << 1));
    i2c_stop();

    return nack ? -1 : 0;
}

int si2c_write_regs(u8 addr, u8 reg, const u8 *buf, u8 len)
{
    u8 i;
    int nack = 0;

    i2c_start();
    nack |= i2c_write_byte((u8)(addr << 1));
    nack |= i2c_write_byte(reg);
    for (i = 0; i < len; i++)
        nack |= i2c_write_byte(buf[i]);
    i2c_stop();

    return nack ? -1 : 0;
}

int si2c_read_regs(u8 addr, u8 reg, u8 *buf, u8 len)
{
    u8 i;
    int nack = 0;

    if (len == 0)
        return -1;

    i2c_start();
    nack |= i2c_write_byte((u8)(addr << 1));
    nack |= i2c_write_byte(reg);
    i2c_start();
    nack |= i2c_write_byte((u8)((addr << 1) | 1));
    if (nack) {
        i2c_stop();
        return -1;
    }

    for (i = 0; i < len; i++)
        buf[i] = i2c_read_byte(i + 1 < len);
    i2c_stop();

    return 0;
}

/*
 * Same as si2c_read_regs(), but frees the bus (STOP) between the register
 * write and the data read. Some panels/clones only latch the register
 * pointer on a STOP; the repeated-start form then returns stale data.
 */
int si2c_read_regs_stop(u8 addr, u8 reg, u8 *buf, u8 len)
{
    u8 i;
    int nack = 0;

    if (len == 0)
        return -1;

    i2c_start();
    nack |= i2c_write_byte((u8)(addr << 1));
    nack |= i2c_write_byte(reg);
    i2c_stop();
    if (nack)
        return -1;

    i2c_start();
    nack |= i2c_write_byte((u8)((addr << 1) | 1));
    if (nack) {
        i2c_stop();
        return -1;
    }

    for (i = 0; i < len; i++)
        buf[i] = i2c_read_byte(i + 1 < len);
    i2c_stop();

    return 0;
}

void si2c_bus_recover(void)
{
    int i;

    sda_release();
    scl_release();
    DELAY_MS(1);

    for (i = 0; i < 9; i++) {
        scl_low();
        i2c_dly();
        scl_release();
        i2c_dly();
    }
    i2c_stop();
}

void si2c_pin_check(const char *tag)
{
    sda_low();
    i2c_dly();
    printf("%s: SDA low=%d", tag, tls_gpio_read(g_sda) ? 1 : 0);
    sda_release();
    i2c_dly();
    printf(" release=%d\r\n", tls_gpio_read(g_sda) ? 1 : 0);

    scl_low();
    i2c_dly();
    printf("%s: SCL low=%d", tag, tls_gpio_read(g_scl) ? 1 : 0);
    scl_release();
    i2c_dly();
    printf(" release=%d\r\n", tls_gpio_read(g_scl) ? 1 : 0);
}

void si2c_scan(u8 lo, u8 hi, const char *tag)
{
    u8 a;
    int any = 0;

    for (a = lo; a <= hi; a++) {
        if (si2c_probe(a) == 0) {
            printf("%s: ACK 0x%02X\r\n", tag, a);
            any = 1;
        }
    }
    if (!any)
        printf("%s: no ACK 0x%02X..0x%02X\r\n", tag, lo, hi);
}
