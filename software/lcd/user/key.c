#include "board.h"
#include "xl9555.h"
#include "key.h"

static u8  g_stable;
static u8  g_raw_last;
static u32 g_raw_since_ms;

static u32 now_ms(void)
{
    return tls_os_get_time() * 2u;      /* 500 Hz tick */
}

void key_init(void)
{
    u8 cfg;

    /* P0_4..7 must be inputs; xl9555_init() normally already did this. */
    if (xl9555_read_regs(XL9555_REG_CFG0, &cfg, 1) == 0) {
        if ((cfg & (XL9555_P0_KEY0 | XL9555_P0_KEY1 |
                    XL9555_P0_KEY2 | XL9555_P0_KEY3)) !=
            (XL9555_P0_KEY0 | XL9555_P0_KEY1 |
             XL9555_P0_KEY2 | XL9555_P0_KEY3)) {
            u8 want = (u8)(cfg | XL9555_P0_KEY0 | XL9555_P0_KEY1 |
                           XL9555_P0_KEY2 | XL9555_P0_KEY3);
            xl9555_write_regs(XL9555_REG_CFG0, &want, 1);
        }
    }

    g_stable = key_read_raw();
    g_raw_last = g_stable;
    g_raw_since_ms = now_ms();
}

u8 key_read_raw(void)
{
    u8 in;

    if (xl9555_read_regs(XL9555_REG_IN0, &in, 1) != 0)
        return 0;

    /* Active low, pins 4..7; return them as bits 0..3. */
    return (u8)((~in >> 4) & 0x0Fu);
}

u8 key_scan(void)
{
    u8 raw = key_read_raw();
    u32 t = now_ms();

    if (raw != g_raw_last) {
        g_raw_last = raw;
        g_raw_since_ms = t;
    }
    if ((u32)(t - g_raw_since_ms) >= KEY_DEBOUNCE_MS)
        g_stable = raw;
    return g_stable;
}

int key_pressed(u8 index)
{
    if (index >= KEY_COUNT)
        return 0;
    return (key_scan() >> index) & 1u;
}
