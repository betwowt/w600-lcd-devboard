/*
 * Boot logo from the external W25Q64.
 *
 * The TF card is only a source: on every boot, if /logo.bin exists it is
 * copied to a reserved area of the external Flash.  The panel always loads
 * the logo from that Flash area, so removing or changing the card does not
 * change the boot path after the update.
 *
 * Flash layout: 0x700000 + 256 KB reserved, file stored verbatim
 * ("LOGO" header + RGB565 data).  The last 4 KB sector is left for the
 * board-check Flash test.
 */
#include <string.h>

#include "board.h"
#include "lcd.h"
#include "logo.h"
#include "sd_fs.h"
#include "w25qxx.h"

#define LOGO_NAME       "LOGO    BIN"
#define LOGO_FLASH_ADDR 0x700000u
#define LOGO_FLASH_MAX  0x40000u
#define LOGO_CHUNK      4096u

static u8 g_sd_buf[LOGO_CHUNK] __attribute__((aligned(4)));
static u8 g_flash_buf[LOGO_CHUNK] __attribute__((aligned(4)));
static u16 g_logo_w;
static u16 g_logo_h;
static u32 g_logo_size;
static u32 g_sd_logo_file_size;

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int logo_parse(const u8 *hdr, u32 file_size,
                      u16 *w, u16 *h, u32 *data_size)
{
    u16 ww;
    u16 hh;
    u16 fmt;
    u32 ds;

    if (file_size < 20u || memcmp(hdr, "LOGO", 4) != 0)
        return -1;

    ww = rd16(hdr + 4);
    hh = rd16(hdr + 6);
    fmt = rd16(hdr + 8);
    ds = rd32(hdr + 12);

    if (fmt != 1 || ww == 0 || hh == 0 ||
        ww > LCD_W || hh > LCD_H ||
        ds != (u32)ww * hh * 2u ||
        ds > LOGO_FLASH_MAX - 20u ||
        file_size < 20u + ds)
        return -1;

    *w = ww;
    *h = hh;
    *data_size = ds;
    return 0;
}

static int logo_sd_validate(void)
{
    u8 hdr[20];
    u16 w;
    u16 h;
    u32 data_size;
    u32 file_size;

    if (sd_file_open(LOGO_NAME, &file_size) != 0)
        return -1;
    if (file_size < sizeof(hdr) ||
        sd_file_read(hdr, sizeof(hdr)) != 0)
        return -1;
    if (logo_parse(hdr, file_size, &w, &h, &data_size) != 0)
        return -1;

    g_sd_logo_file_size = 20u + data_size;
    printf("LOGO: SD /logo.bin %ux%u %u bytes\r\n",
           w, h, (unsigned)data_size);
    return 0;
}

static int logo_flash_same(void)
{
    u32 off = 0;

    if (sd_file_rewind() != 0)
        return -1;

    while (off < g_sd_logo_file_size) {
        u32 n = g_sd_logo_file_size - off;

        if (n > LOGO_CHUNK)
            n = LOGO_CHUNK;
        if (sd_file_read(g_sd_buf, n) != 0)
            return -1;
        if (w25qxx_read(LOGO_FLASH_ADDR + off, g_flash_buf, n) != 0)
            return -1;
        if (memcmp(g_sd_buf, g_flash_buf, n) != 0)
            return 0;
        off += n;
    }
    return 1;
}

static int logo_flash_write(void)
{
    u32 off = 0;

    if (sd_file_rewind() != 0)
        return -1;

    while (off < g_sd_logo_file_size) {
        u32 n = g_sd_logo_file_size - off;

        if (n > LOGO_CHUNK)
            n = LOGO_CHUNK;
        if (sd_file_read(g_sd_buf, n) != 0)
            return -1;
        if (w25qxx_write(LOGO_FLASH_ADDR + off, g_sd_buf, n) != 0)
            return -1;
        off += n;
    }
    return 0;
}

static int logo_flash_update(void)
{
    int same = logo_flash_same();

    if (same < 0)
        return -1;
    if (same) {
        printf("LOGO: Flash logo current\r\n");
        return 0;
    }

    printf("LOGO: writing %u bytes to Flash @0x%06X\r\n",
           (unsigned)g_sd_logo_file_size, (unsigned)LOGO_FLASH_ADDR);
    if (logo_flash_write() != 0) {
        printf("LOGO: Flash write failed\r\n");
        return -1;
    }
    if (logo_flash_same() != 1) {
        printf("LOGO: Flash verify failed\r\n");
        return -1;
    }
    printf("LOGO: Flash logo updated\r\n");
    return 0;
}

static int logo_flash_load(void)
{
    u8 hdr[20];
    u16 w;
    u16 h;
    u32 data_size;

    if (w25qxx_read(LOGO_FLASH_ADDR, hdr, sizeof(hdr)) != 0)
        return -1;
    if (logo_parse(hdr, LOGO_FLASH_MAX, &w, &h, &data_size) != 0)
        return -1;

    g_logo_w = w;
    g_logo_h = h;
    g_logo_size = data_size;
    printf("LOGO: Flash %ux%u %u bytes\r\n", w, h, (unsigned)data_size);
    return 0;
}

int lcd_logo_load(void)
{
    int flash_ok;

    g_logo_size = 0;

    flash_ok = (w25qxx_init() == 0);
    if (!flash_ok)
        printf("LOGO: Flash init failed, probing SD anyway\r\n");

    if (sd_mount() == 0 && logo_sd_validate() == 0) {
        printf("LOGO: SD logo found\r\n");
        if (flash_ok)
            (void)logo_flash_update();
        sd_file_close();
    } else {
        printf("LOGO: no SD logo\r\n");
        sd_file_close();
    }

    if (!flash_ok)
        return -1;

    if (logo_flash_load() != 0) {
        printf("LOGO: no valid logo in Flash\r\n");
        return -1;
    }
    return 0;
}

int lcd_logo_display(void)
{
    u32 off = 0;

    if (g_logo_size == 0 || g_logo_w == 0 || g_logo_h == 0)
        return -1;

    if (lcd_init_pwr(1) != 0) {
        printf("LOGO: LCD init failed\r\n");
        return -1;
    }

    lcd_image_begin(0, 0, g_logo_w, g_logo_h);
    while (off < g_logo_size) {
        u32 n = g_logo_size - off;

        if (n > LOGO_CHUNK)
            n = LOGO_CHUNK;
        if (w25qxx_read(LOGO_FLASH_ADDR + 20u + off, g_sd_buf, n) != 0) {
            printf("LOGO: Flash read failed\r\n");
            return -1;
        }
        lcd_image_data(g_sd_buf, n);
        off += n;
    }

    printf("LOGO: done\r\n");
    return 0;
}
