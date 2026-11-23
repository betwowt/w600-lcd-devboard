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
#include "spi_bus.h"
#include "w25qxx.h"

#define SD_CS_PIN       WM_IO_PB_16
#define SD_CS_LOW()     ((void)spi_bus_cs_low(SPI_DEV_SD))
#define SD_CS_HIGH()    ((void)spi_bus_cs_high(SPI_DEV_SD))

#define SD_BLOCK_SIZE   512u
#define SD_TOKEN_START  0xFE
#define FAT_INVALID     0xFFFFFFFFu

#define LOGO_FLASH_ADDR 0x700000u
#define LOGO_FLASH_MAX  0x40000u
#define LOGO_CHUNK      4096u

static u8 g_sd_crc[2] __attribute__((aligned(4)));

static u8 g_sec[SD_BLOCK_SIZE] __attribute__((aligned(4)));
static u8 g_fat_sec[SD_BLOCK_SIZE] __attribute__((aligned(4)));
static u8 g_sd_buf[LOGO_CHUNK] __attribute__((aligned(4)));
static u8 g_flash_buf[LOGO_CHUNK] __attribute__((aligned(4)));
static u16 g_logo_w;
static u16 g_logo_h;
static u32 g_logo_size;
static u32 g_sd_logo_file_size;

static u32 g_fat_sec_lba;
static int g_fat_sec_valid;

static int g_fat32;
static u8 g_sec_per_clus;
static u32 g_fat_start;
static u32 g_data_start;
static u32 g_root_dir_lba;
static u32 g_root_clus;
static u16 g_root_entries;
static u32 g_total_clusters;
static int g_sd_byte_addr;

static u32 g_file_cluster;
static u32 g_file_size;
static u32 g_sd_file_start;
static u32 g_file_left;
static u32 g_file_clus_lba;
static u32 g_file_sec_idx;
static u32 g_file_off;
static int g_file_sec_valid;

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---- SPI / SD --------------------------------------------------------- */

/*
 * The W600 master-SPI controller reads the card's R1 as 0xC0 (bit
 * alignment/pipeline issue), while the same card answers perfectly to a
 * GPIO bit-bang at mode 0. Keep the hardware path as the fast path, and
 * fall back to bit-bang when CMD0 is not 0x01.
 */
static int g_sd_bb;

#define SD_INIT_HZ  400000u
#define SD_DATA_HZ  1000000u

static void bb_delay(void)
{
    spi_bus_bb_delay();
}

static void sd_bb_pins(void)
{
    tls_gpio_cfg(SD_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    (void)spi_bus_cs_high(SPI_DEV_SD);
    tls_gpio_cfg(WM_IO_PA_01, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(WM_IO_PA_01, 0);
    tls_gpio_cfg(WM_IO_PB_18, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(WM_IO_PB_18, 1);
    tls_gpio_cfg(WM_IO_PB_17, WM_GPIO_DIR_INPUT, WM_GPIO_ATTR_PULLHIGH);
}

static u8 bb_xfer(u8 out)
{
    u8 in = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        tls_gpio_write(WM_IO_PB_18, (out >> i) & 1);
        bb_delay();
        tls_gpio_write(WM_IO_PA_01, 1);
        bb_delay();
        in = (u8)((in << 1) | (tls_gpio_read(WM_IO_PB_17) ? 1 : 0));
        tls_gpio_write(WM_IO_PA_01, 0);
        bb_delay();
    }
    return in;
}

static u8 spi_xfer(u8 out)
{
    u8 in = 0xFF;

    if (g_sd_bb)
        return bb_xfer(out);
    tls_spi_read(&in, 1);
    return in;
}

static void spi_tx(const u8 *buf, u32 len)
{
    u32 i;

    if (g_sd_bb) {
        for (i = 0; i < len; i++)
            bb_xfer(buf[i]);
        return;
    }
    if (len)
        tls_spi_write(buf, len);
}

static void spi_rx(u8 *buf, u32 len)
{
    u32 i;

    if (g_sd_bb) {
        for (i = 0; i < len; i++)
            buf[i] = bb_xfer(0xFF);
        return;
    }
    for (i = 0; i < len; i++)
        tls_spi_read(&buf[i], 1);
}

static void sd_deselect(void)
{
    SD_CS_HIGH();
    spi_xfer(0xFF);
}

/*
 * Send an SD command and return R1. Eight clocks before selecting and after
 * deselecting match the W601 FATFS reference. keep=1 leaves CS low for the
 * R3/R7 payload (CMD8/CMD58); the caller must deselect afterwards.
 */
static u8 sd_cmd(u8 cmd, u32 arg, u8 crc, int keep)
{
    static const u8 pre[3] = {0xFF, 0xFF, 0xFF};
    u8 buf[6];
    u8 r = 0xFF;
    int i;

    buf[0] = (u8)(0x40 | cmd);
    buf[1] = (u8)(arg >> 24);
    buf[2] = (u8)(arg >> 16);
    buf[3] = (u8)(arg >> 8);
    buf[4] = (u8)arg;
    buf[5] = crc;

    if (g_sd_bb)
        sd_bb_pins();
    SD_CS_HIGH();
    spi_tx(pre, sizeof(pre));
    SD_CS_LOW();
    spi_tx(buf, sizeof(buf));
    for (i = 0; i < 200; i++) {
        r = spi_xfer(0xFF);
        if ((r & 0x80u) == 0)
            break;
    }
    if (!keep)
        sd_deselect();
    return r;
}

static int sd_read_block_locked(u32 lba, u8 *buf)
{
    (void)spi_bus_set_clock(SPI_DEV_SD, SD_DATA_HZ);
    u8 r;
    u32 i;

    if (g_sd_byte_addr)
        lba *= SD_BLOCK_SIZE;

    /*
     * CMD17 must keep CS low: the card streams the data token and block
     * right after R1. The W601 reference never deselects here.
     */
    r = sd_cmd(17, lba, 0x01, 1);
    if (r != 0x00) {
        sd_deselect();
        return -1;
    }

    for (i = 0; i < 4000u; i++) {
        r = spi_xfer(0xFF);
        if (r == SD_TOKEN_START)
            break;
    }
    if (r != SD_TOKEN_START) {
        sd_deselect();
        return -1;
    }

    spi_rx(buf, SD_BLOCK_SIZE);
    spi_rx(g_sd_crc, sizeof(g_sd_crc));
    sd_deselect();
    return 0;
}

struct sd_block_arg {
    u32 lba;
    u8 *buf;
};

static int sd_read_block_op(void *arg)
{
    struct sd_block_arg *a = (struct sd_block_arg *)arg;

    return sd_read_block_locked(a->lba, a->buf);
}

static int sd_read_block(u32 lba, u8 *buf)
{
    struct sd_block_arg a = {lba, buf};

    return spi_bus_run(SPI_DEV_SD, sd_read_block_op, &a);
}

static int sd_init_locked(void)
{
    u8 r;
    u8 r7[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    u8 r55;
    u32 i;
    int v2 = 0;
    int ok = -1;

    /*
     * Always use GPIO SPI.  The W600 controller's RX path misreads the
     * card's R1, and its TX/RX transaction does not reliably drive MOSI
     * high during the card's power-up clocks.  Bit-bang works on this
     * board (SDDIAG CMD0=01), so do not probe hardware first.
     */
    g_sd_bb = 1;
    (void)spi_bus_set_clock(SPI_DEV_SD, SD_INIT_HZ);
    printf("SD: clock init %u Hz, data %u Hz\r\n",
           (unsigned)SD_INIT_HZ, (unsigned)SD_DATA_HZ);
    sd_bb_pins();

    /*
     * 74+ clocks with CS high, then let the card finish its own power-up
     * init. A busy card holds DAT0 low (R1 reads 0x00), so give it enough
     * time and retries; native SD power-up can exceed 100 ms.
     */
    for (i = 0; i < 10; i++)
        spi_xfer(0xFF);
    DELAY_MS(500);

    for (i = 0; i < 50; i++) {
        r = sd_cmd(0, 0, 0x95, 0);          /* CMD0: GO_IDLE_STATE */
#ifdef SD_DIAG
        printf("SD: CMD0 try %u -> %02X\r\n", (unsigned)i, r);
#endif
        if (r == 0x01)
            break;
        DELAY_MS(10);
    }
    if (r != 0x01)
        goto out;

    /* CMD8 is only used to detect a v2 card; its R7 echo is not needed. */
    r = sd_cmd(8, 0x1AA, 0x87, 1);
    if (r == 0x01) {
        v2 = 1;
        spi_rx(r7, sizeof(r7));
    }
    sd_deselect();
#ifdef SD_DIAG
    printf("SD: CMD8 -> %02X echo=%02X %02X %02X %02X v2=%d\r\n",
           r, r7[0], r7[1], r7[2], r7[3], v2);
#endif
    if (r != 0x01 && r != 0x05)
        goto out;

    /* ACMD41 until the card leaves the idle state. */
    for (i = 0; i < 200; i++) {
        r55 = sd_cmd(55, 0, 0x65, 0);       /* APP_CMD must return 0x01 */
#ifdef SD_DIAG
        if (r55 != 0x01)
            printf("SD: CMD55 -> %02X\r\n", r55);
#endif
        if (r55 != 0x01)
            goto out;
        r = sd_cmd(41, v2 ? 0x40000000u : 0u, 0x77, 0);
        if (r == 0x00)
            break;
        if (r != 0x01)
            goto out;
        if ((i % 20) == 19) {
#ifdef SD_DIAG
            printf("SD: ACMD41 still idle after %u tries\r\n",
                   (unsigned)(i + 1));
#endif
        }
        DELAY_MS(10);
    }
#ifdef SD_DIAG
    printf("SD: ACMD41 try %u -> %02X\r\n", (unsigned)i, r);
#endif
    if (r != 0x00)
        goto out;

    (void)spi_bus_set_clock(SPI_DEV_SD, SD_DATA_HZ);

    /* CMD58: read OCR, determine byte vs. block addressing. */
    r = sd_cmd(58, 0, 0xFD, 1);
    if (r != 0x00) {
        sd_deselect();
        goto out;
    }
    spi_rx(r7, sizeof(r7));
    g_sd_byte_addr = (r7[0] & 0x40) ? 0 : 1;
    sd_deselect();
#ifdef SD_DIAG
    printf("SD: CMD58 -> %02X OCR=%02X %02X %02X %02X byte_addr=%d\r\n",
           r, r7[0], r7[1], r7[2], r7[3], g_sd_byte_addr);
#endif

    r = sd_cmd(16, SD_BLOCK_SIZE, 0x01, 0); /* CMD16: set block length */
    if (r != 0x00)
        goto out;

    ok = 0;

out:
    if (ok != 0)
        SD_CS_HIGH();
    /*
     * Shared bus: put CLK/MOSI/MISO back on the master SPI controller.
     * sd_cmd() re-enters bit-bang on the next SD transaction.
     */
    (void)lcd_spi_resume();
    return ok;
}

static int sd_init_op(void *arg)
{
    (void)arg;
    return sd_init_locked();
}

static int sd_init(void)
{
    return spi_bus_run(SPI_DEV_SD, sd_init_op, NULL);
}

/* ---- FAT ------------------------------------------------------------- */

static int fat_mount(void)
{
    u32 part = 0;
    u32 total;
    u32 fat_size;
    u32 root_dir_sectors;
    u32 nfats;
    u32 reserved;
    u8 *bs;
    int i;

    if (sd_read_block(0, g_sec) != 0)
        return -1;
    if (rd16(g_sec + 510) != 0xAA55)
        return -1;

    bs = g_sec;
    if (memcmp(g_sec + 82, "FAT32", 5) == 0) {
        part = 0;                            /* superfloppy */
    } else {
        for (i = 0; i < 4; i++) {
            u8 *e = g_sec + 446 + i * 16;
            u8 type = e[4];

            if (type == 0x0B || type == 0x0C || type == 0x06 || type == 0x0E) {
                part = rd32(e + 8);
                break;
            }
        }
        if (part == 0)
            return -1;
        if (sd_read_block(part, g_sec) != 0)
            return -1;
        if (rd16(g_sec + 510) != 0xAA55)
            return -1;
        bs = g_sec;
    }

    if (memcmp(bs + 82, "FAT32", 5) == 0) {
        g_fat32 = 1;
    } else if (memcmp(bs + 54, "FAT16", 5) == 0) {
        g_fat32 = 0;
    } else {
        return -1;
    }

    if (rd16(bs + 11) != SD_BLOCK_SIZE)
        return -1;

    g_sec_per_clus = bs[13];
    if (g_sec_per_clus == 0)
        return -1;

    reserved = rd16(bs + 14);
    nfats = bs[16];
    g_root_entries = rd16(bs + 17);
    total = rd16(bs + 19);
    if (total == 0)
        total = rd32(bs + 32);
    fat_size = rd16(bs + 22);
    if (fat_size == 0)
        fat_size = rd32(bs + 36);
    if (nfats == 0 || total == 0 || fat_size == 0)
        return -1;

    root_dir_sectors = ((u32)g_root_entries * 32u + SD_BLOCK_SIZE - 1u) /
                       SD_BLOCK_SIZE;
    g_fat_start = part + reserved;
    g_root_dir_lba = g_fat_start + nfats * fat_size;
    g_data_start = g_root_dir_lba + root_dir_sectors;
    g_root_clus = g_fat32 ? rd32(bs + 44) : 0;
    g_total_clusters = (total - reserved - nfats * fat_size -
                        root_dir_sectors) / g_sec_per_clus;
    g_fat_sec_valid = 0;

    if (g_total_clusters == 0 || (g_fat32 && g_root_clus < 2))
        return -1;

    printf("LOGO: FAT%d, %u clusters x %u sectors\r\n",
           g_fat32 ? 32 : 16,
           (unsigned)g_total_clusters,
           (unsigned)g_sec_per_clus);
    return 0;
}

int sd_card_probe(char *msg, u32 msg_len)
{
    u32 kb;

    if (!msg || msg_len == 0)
        return -1;

    if (sd_init() != 0) {
        snprintf(msg, msg_len, "init failed");
        return -1;
    }
    if (fat_mount() != 0) {
        snprintf(msg, msg_len, "mount failed");
        return -2;
    }

    kb = g_total_clusters * g_sec_per_clus / 2u;   /* 512-byte sectors */
    snprintf(msg, msg_len, "FAT%d %uKB", g_fat32 ? 32 : 16, (unsigned)kb);
    return 0;
}

static u32 fat_next(u32 cluster)
{
    u32 off;
    u32 lba;
    u32 idx;
    u32 v;

    if (g_fat32) {
        if (cluster < 2 || cluster > g_total_clusters + 1)
            return FAT_INVALID;
        off = cluster * 4u;
    } else {
        if (cluster < 2 || cluster > g_total_clusters + 1)
            return FAT_INVALID;
        off = cluster * 2u;
    }

    lba = g_fat_start + off / SD_BLOCK_SIZE;
    idx = off % SD_BLOCK_SIZE;

    if (!g_fat_sec_valid || g_fat_sec_lba != lba) {
        if (sd_read_block(lba, g_fat_sec) != 0)
            return FAT_INVALID;
        g_fat_sec_lba = lba;
        g_fat_sec_valid = 1;
    }

    v = g_fat32 ? (rd32(g_fat_sec + idx) & 0x0FFFFFFFu)
                : (u32)rd16(g_fat_sec + idx);

    if (v == 0)
        return FAT_INVALID;
    if (g_fat32) {
        if (v >= 0x0FFFFFF8u)
            return FAT_INVALID;
    } else {
        if (v >= 0xFFF8u)
            return FAT_INVALID;
    }
    return v;
}

static int logo_entry(const u8 *e, u32 *cluster, u32 *size)
{
    u8 attr = e[11];

    if (e[0] == 0x00)
        return -2;                           /* end of directory */
    if (e[0] == 0xE5)
        return 0;                            /* deleted */
    if (attr == 0x0F || (attr & 0x08) || (attr & 0x10))
        return 0;                            /* LFN, volume, directory */

    if (memcmp(e, "LOGO    BIN", 11) != 0)
        return 0;

    *cluster = g_fat32 ? (((u32)rd16(e + 20) << 16) | rd16(e + 26))
                       : (u32)rd16(e + 26);
    *size = rd32(e + 28);
    return 1;
}

static int logo_find(void)
{
    u32 cluster;
    u32 size;
    u32 guard = 0;
    u32 s;
    int i;

    if (g_fat32) {
        cluster = g_root_clus;
        while (cluster >= 2 && guard++ < g_total_clusters + 2u) {
            u32 base = g_data_start +
                       (cluster - 2u) * g_sec_per_clus;

            for (s = 0; s < g_sec_per_clus; s++) {
                if (sd_read_block(base + s, g_sec) != 0)
                    return -1;
                for (i = 0; i < (int)SD_BLOCK_SIZE; i += 32) {
                    int r = logo_entry(g_sec + i, &cluster, &size);

                    if (r == 1) {
                        g_file_cluster = cluster;
                        g_file_size = size;
                        return 0;
                    }
                    if (r == -2)
                        return -1;
                }
            }
            cluster = fat_next(cluster);
            if (cluster == FAT_INVALID)
                break;
        }
        return -1;
    }

    {
        u32 root_sectors = ((u32)g_root_entries * 32u +
                            SD_BLOCK_SIZE - 1u) / SD_BLOCK_SIZE;

        for (s = 0; s < root_sectors; s++) {
            if (sd_read_block(g_root_dir_lba + s, g_sec) != 0)
                return -1;
            for (i = 0; i < (int)SD_BLOCK_SIZE; i += 32) {
                int r = logo_entry(g_sec + i, &cluster, &size);

                if (r == 1) {
                    g_file_cluster = cluster;
                    g_file_size = size;
                    return 0;
                }
                if (r == -2)
                    return -1;
            }
        }
    }
    return -1;
}

static int logo_stream_start(u32 cluster, u32 size)
{
    if (cluster < 2 || size == 0)
        return -1;

    g_file_cluster = cluster;
    g_file_size = size;
    g_file_left = size;
    g_file_clus_lba = g_data_start + (cluster - 2u) * g_sec_per_clus;
    g_file_sec_idx = 0;
    g_file_off = 0;
    g_file_sec_valid = 0;
    return 0;
}

static int logo_read(u8 *buf, u32 len)
{
    if (len > g_file_left)
        return -1;

    while (len) {
        u32 take;

        if (!g_file_sec_valid) {
            u32 lba = g_file_clus_lba + g_file_sec_idx;

            if (sd_read_block(lba, g_sec) != 0)
                return -1;
            g_file_sec_valid = 1;
        }

        take = SD_BLOCK_SIZE - g_file_off;
        if (take > len)
            take = len;
        memcpy(buf, g_sec + g_file_off, take);

        buf += take;
        len -= take;
        g_file_off += take;
        g_file_left -= take;

        if (g_file_off == SD_BLOCK_SIZE) {
            g_file_off = 0;
            g_file_sec_valid = 0;
            g_file_sec_idx++;
            if (g_file_sec_idx >= g_sec_per_clus) {
                u32 next;

                if (g_file_left == 0)
                    break;                   /* no next cluster needed */
                next = fat_next(g_file_cluster);
                if (next == FAT_INVALID)
                    return -1;
                g_file_cluster = next;
                g_file_clus_lba = g_data_start +
                                  (next - 2u) * g_sec_per_clus;
                g_file_sec_idx = 0;
            }
        }
    }
    return 0;
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

    if (logo_find() != 0)
        return -1;
    g_sd_file_start = g_file_cluster;
    if (g_file_size < sizeof(hdr) ||
        logo_stream_start(g_sd_file_start, g_file_size) != 0)
        return -1;
    if (logo_read(hdr, sizeof(hdr)) != 0)
        return -1;
    if (logo_parse(hdr, g_file_size, &w, &h, &data_size) != 0)
        return -1;

    g_sd_logo_file_size = 20u + data_size;
    printf("LOGO: SD /logo.bin %ux%u %u bytes\r\n",
           w, h, (unsigned)data_size);
    return 0;
}

static int logo_flash_same(void)
{
    u32 off = 0;

    if (logo_stream_start(g_sd_file_start, g_file_size) != 0)
        return -1;

    while (off < g_sd_logo_file_size) {
        u32 n = g_sd_logo_file_size - off;

        if (n > LOGO_CHUNK)
            n = LOGO_CHUNK;
        if (logo_read(g_sd_buf, n) != 0)
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

    if (logo_stream_start(g_sd_file_start, g_file_size) != 0)
        return -1;

    while (off < g_sd_logo_file_size) {
        u32 n = g_sd_logo_file_size - off;

        if (n > LOGO_CHUNK)
            n = LOGO_CHUNK;
        if (logo_read(g_sd_buf, n) != 0)
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

    if (sd_init() == 0 && fat_mount() == 0 && logo_sd_validate() == 0) {
        printf("LOGO: SD logo found\r\n");
        if (flash_ok)
            (void)logo_flash_update();
    } else {
        printf("LOGO: no SD logo\r\n");
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
