/*
 * Minimal SD card + FAT16/FAT32 reader for the shared SPI bus.
 *
 * The W600 master-SPI controller reads the card's R1 as 0xC0 (bit
 * alignment/pipeline issue), while the same card answers perfectly to a
 * GPIO bit-bang at mode 0, so the whole path is bit-banged.
 *
 * The bit-bang writes the W600 GPIO data registers directly: the SDK's
 * tls_gpio_write() takes a critical section and masks the pin on every
 * call, which caps the card clock at a few hundred kHz.  Direct stores
 * reach several MHz and are what makes real-time frame streaming from
 * the TF card possible.  Data CRCs are enabled (CMD59) and every block
 * is verified; the clock is probed down from 4 MHz until the card passes.
 *
 * The W25Qxx and LCD drivers still use the SDK API and keep the shared
 * bus interlock in spi_bus.c; this file only runs inside an SD bus job.
 */
#include <string.h>

#include "board.h"
#include "lcd.h"
#include "sd_fs.h"
#include "spi_bus.h"

#define SD_CS_PIN       WM_IO_PB_16
#define SD_CS_LOW()     ((void)spi_bus_cs_low(SPI_DEV_SD))
#define SD_CS_HIGH()    ((void)spi_bus_cs_high(SPI_DEV_SD))

#define SD_TOKEN_START  0xFE
#define FAT_INVALID     0xFFFFFFFFu

/* W600 GPIO data registers (see wm_regs.h, GPIOA/B base). */
#define SD_PA_DATA      (*(volatile u32 *)0x40010C00u)
#define SD_PB_DATA      (*(volatile u32 *)0x40011200u)
#define SD_PA_DATA_EN   (*(volatile u32 *)0x40010C04u)
#define SD_PB_DATA_EN   (*(volatile u32 *)0x40011204u)
#define SD_PA_CLK       (1u << 1)       /* PA01 */
#define SD_PB_MOSI      (1u << 18)      /* PB18 */
#define SD_PB_MISO      (1u << 17)      /* PB17 */

#define SD_INIT_HZ      400000u
#define SD_HZ_COUNT     7

/*
 * Target clocks, fastest first.  The bit-bang tops out around 2 MHz on an
 * 80 MHz core, so the 8 MHz entry really means "no delay loop"; the CRC
 * probe decides what the card and wiring accept.
 */
static const u32 g_sd_hz_table[SD_HZ_COUNT] = {
    8000000u, 6000000u, 4000000u, 3000000u, 2000000u, 1000000u, 400000u
};

static u8 g_crc[2] __attribute__((aligned(4)));
static u8 g_sec[SD_BLOCK_SIZE] __attribute__((aligned(4)));
static u8 g_fat_sec[SD_BLOCK_SIZE] __attribute__((aligned(4)));

static u32 g_bb_loops;
static int g_sd_ready;
static int g_fs_mounted;
static int g_crc_on;
static int g_sd_hz_idx = SD_HZ_COUNT - 1;

static int g_fat32;
static u8 g_sec_per_clus;
static u32 g_fat_start;
static u32 g_data_start;
static u32 g_root_dir_lba;
static u32 g_root_clus;
static u16 g_root_entries;
static u32 g_total_clusters;
static int g_sd_byte_addr;

static u32 g_fat_sec_lba;
static int g_fat_sec_valid;

static u32 g_file_first_cluster;
static u32 g_file_total_size;
static u32 g_file_cluster;
static u32 g_file_left;
static u32 g_file_clus_lba;
static u32 g_file_sec_idx;
static u32 g_file_off;
static int g_file_sec_valid;
static int g_file_open;

static u16 rd16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---- SPI bit-bang ----------------------------------------------------- */

static void sd_bb_pins(void)
{
    tls_gpio_cfg(SD_CS_PIN, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    (void)spi_bus_cs_high(SPI_DEV_SD);
    tls_gpio_cfg(WM_IO_PA_01, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(WM_IO_PA_01, 0);
    tls_gpio_cfg(WM_IO_PB_18, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(WM_IO_PB_18, 1);
    tls_gpio_cfg(WM_IO_PB_17, WM_GPIO_DIR_INPUT, WM_GPIO_ATTR_PULLHIGH);

    /*
     * tls_gpio_read/write() raise the per-pin DATA_EN bit around every
     * access; keep it asserted for our three pins so the hot loop can use
     * plain register stores/loads.
     */
    SD_PA_DATA_EN |= SD_PA_CLK;
    SD_PB_DATA_EN |= SD_PB_MOSI | SD_PB_MISO;
}

static u8 bb_xfer(u8 out)
{
    u32 delay = g_bb_loops;
    u32 pb = SD_PB_DATA & ~SD_PB_MOSI;
    u8 in = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        if (out & (1u << i))
            pb |= SD_PB_MOSI;
        else
            pb &= ~SD_PB_MOSI;
        SD_PB_DATA = pb;

        if (delay) {
            u32 n = delay;

            while (n--)
                __asm__ __volatile__("nop");
        }

        SD_PA_DATA |= SD_PA_CLK;
        if (delay) {
            u32 n = delay;

            while (n--)
                __asm__ __volatile__("nop");
        }
        __asm__ __volatile__("nop");
        __asm__ __volatile__("nop");
        in = (u8)((in << 1) | ((SD_PB_DATA & SD_PB_MISO) ? 1u : 0u));
        SD_PA_DATA &= ~SD_PA_CLK;

        if (delay) {
            u32 n = delay;

            while (n--)
                __asm__ __volatile__("nop");
        }
    }
    return in;
}

static void spi_tx(const u8 *buf, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++)
        (void)bb_xfer(buf[i]);
}

static void spi_rx(u8 *buf, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++)
        buf[i] = bb_xfer(0xFF);
}

static void sd_deselect(void)
{
    SD_CS_HIGH();
    (void)bb_xfer(0xFF);
}

/* CRC7 over start bit, command index and four argument bytes. */
static u8 sd_cmd_crc(u8 cmd, u32 arg)
{
    u8 buf[5];
    u8 crc = 0;
    int i;
    int b;

    buf[0] = (u8)(0x40 | cmd);
    buf[1] = (u8)(arg >> 24);
    buf[2] = (u8)(arg >> 16);
    buf[3] = (u8)(arg >> 8);
    buf[4] = (u8)arg;

    for (i = 0; i < 5; i++) {
        for (b = 7; b >= 0; b--) {
            u8 bit = (u8)((buf[i] >> b) & 1u);
            u8 top = (u8)((crc >> 6) & 1u);

            crc = (u8)(crc << 1);
            if (top ^ bit)
                crc ^= 0x09u;
        }
    }
    return (u8)((crc << 1) | 1u);
}

/*
 * Send an SD command and return R1.  Eight clocks before selecting and
 * after deselecting match the W601 FATFS reference.  keep=1 leaves CS low
 * for the R3/R7 payload (CMD8/CMD58); the caller must deselect afterwards.
 * The CRC is always computed so the code also works if a card checks it.
 */
static u8 sd_cmd(u8 cmd, u32 arg, int keep)
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
    buf[5] = sd_cmd_crc(cmd, arg);

    sd_bb_pins();
    SD_CS_HIGH();
    spi_tx(pre, sizeof(pre));
    SD_CS_LOW();
    spi_tx(buf, sizeof(buf));
    for (i = 0; i < 400; i++) {
        r = bb_xfer(0xFF);
        if ((r & 0x80u) == 0)
            break;
    }
    if (!keep)
        sd_deselect();
    return r;
}

/* ---- SD data blocks --------------------------------------------------- */

static u16 sd_crc16(const u8 *p, u32 len)
{
    static const u16 tab[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
        0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF
    };
    u16 crc = 0;

    while (len--) {
        crc = (u16)((crc << 4) ^ tab[((crc >> 12) ^ (*p >> 4)) & 0xFu]);
        crc = (u16)((crc << 4) ^ tab[((crc >> 12) ^ (*p & 0xFu)) & 0xFu]);
        p++;
    }
    return crc;
}

/* SD keeps its own half-period table; the flash driver's stays untouched. */
static u32 sd_loops_for_hz(u32 hz)
{
    if (hz <= 512000u)
        return 80u;
    if (hz <= 1000000u)
        return 40u;
    if (hz <= 2000000u)
        return 12u;
    if (hz <= 3000000u)
        return 8u;
    if (hz <= 4000000u)
        return 4u;
    if (hz <= 6000000u)
        return 2u;
    return 0u;
}

static void sd_set_clock(int idx)
{
    g_sd_hz_idx = idx;
    g_bb_loops = sd_loops_for_hz(g_sd_hz_table[idx]);
}

/* One CMD17 attempt. verify=0 skips the CRC check (used by the probe). */
static int sd_read_block_once(u32 lba, u8 *buf, int verify)
{
    u8 r;
    u32 i;

    if (g_sd_byte_addr)
        lba *= SD_BLOCK_SIZE;

    r = sd_cmd(17, lba, 1);
    if (r != 0x00) {
        sd_deselect();
        return -1;
    }

    for (i = 0; i < 4000u; i++) {
        r = bb_xfer(0xFF);
        if (r == SD_TOKEN_START)
            break;
    }
    if (r != SD_TOKEN_START) {
        sd_deselect();
        return -1;
    }

    spi_rx(buf, SD_BLOCK_SIZE);
    spi_rx(g_crc, sizeof(g_crc));
    sd_deselect();

    if (verify && g_crc_on) {
        u16 want = (u16)(((u16)g_crc[0] << 8) | g_crc[1]);

        if (sd_crc16(buf, SD_BLOCK_SIZE) != want)
            return -1;
    }
    return 0;
}

struct sd_block_arg {
    u32 lba;
    u8 *buf;
};

/* Runs inside an SD bus job; may lower the clock if the card misbehaves. */
static int sd_read_block_locked(u32 lba, u8 *buf)
{
    int attempt;
    int ret;

    for (attempt = 0; attempt < 3; attempt++) {
        ret = sd_read_block_once(lba, buf, 1);
        if (ret == 0)
            return 0;
    }

    while (g_sd_hz_idx + 1 < SD_HZ_COUNT) {
        sd_set_clock(g_sd_hz_idx + 1);
        printf("SD: block %u failed, clock -> %u Hz\r\n",
               (unsigned)lba, (unsigned)g_sd_hz_table[g_sd_hz_idx]);
        for (attempt = 0; attempt < 3; attempt++) {
            if (sd_read_block_once(lba, buf, 1) == 0)
                return 0;
        }
    }

    if (g_crc_on) {
        g_crc_on = 0;
        (void)sd_cmd(59, 0, 0);
        printf("SD: CRC keeps failing, CRC off\r\n");
        if (sd_read_block_once(lba, buf, 0) == 0)
            return 0;
    }

    printf("SD: block %u read failed\r\n", (unsigned)lba);
    return -1;
}

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

/*
 * Probe data clocks from fast to slow.  CRC must be on, otherwise a
 * marginal clock can corrupt a logo or a video frame silently.
 */
static void sd_probe_speed(void)
{
    int i;
    int k;

    sd_set_clock(SD_HZ_COUNT - 1);
    if (!g_crc_on)
        return;
    if (sd_read_block_once(0, g_fat_sec, 0) != 0) {
        printf("SD: reference read failed\r\n");
        return;
    }

    for (i = 0; i < SD_HZ_COUNT; i++) {
        sd_set_clock(i);
        for (k = 0; k < 4; k++) {
            if (sd_read_block_once(0, g_sec, 1) != 0)
                break;
            if (memcmp(g_sec, g_fat_sec, SD_BLOCK_SIZE) != 0)
                break;
        }
        if (k == 4) {
            printf("SD: data clock %u Hz, CRC on\r\n",
                   (unsigned)g_sd_hz_table[i]);
            return;
        }
    }

    sd_set_clock(SD_HZ_COUNT - 1);
    if (g_crc_on) {
        g_crc_on = 0;
        (void)sd_cmd(59, 0, 0);
    }
    if (sd_read_block_once(0, g_sec, 0) == 0 &&
        memcmp(g_sec, g_fat_sec, SD_BLOCK_SIZE) == 0) {
        printf("SD: CRC probe failed, CRC off at %u Hz\r\n",
               (unsigned)g_sd_hz_table[SD_HZ_COUNT - 1]);
        return;
    }
    printf("SD: speed probe failed, using %u Hz\r\n",
           (unsigned)g_sd_hz_table[SD_HZ_COUNT - 1]);
}

static int sd_init_locked(void)
{
    u8 r;
    u8 r7[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    u8 r55;
    u32 i;
    int v2 = 0;
    int ok = -1;

    sd_set_clock(SD_HZ_COUNT - 1);
    printf("SD: init clock %u Hz\r\n", (unsigned)SD_INIT_HZ);
    sd_bb_pins();

    /*
     * 74+ clocks with CS high, then let the card finish its own power-up
     * init. A busy card holds DAT0 low (R1 reads 0x00), so give it enough
     * time and retries; native SD power-up can exceed 100 ms.
     */
    for (i = 0; i < 10; i++)
        (void)bb_xfer(0xFF);
    DELAY_MS(500);

    for (i = 0; i < 50; i++) {
        r = sd_cmd(0, 0, 0);          /* CMD0: GO_IDLE_STATE */
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
    r = sd_cmd(8, 0x1AA, 1);
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
        r55 = sd_cmd(55, 0, 0);       /* APP_CMD must return 0x01 */
#ifdef SD_DIAG
        if (r55 != 0x01)
            printf("SD: CMD55 -> %02X\r\n", r55);
#endif
        if (r55 != 0x01)
            goto out;
        r = sd_cmd(41, v2 ? 0x40000000u : 0u, 0);
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

    /* CMD58: read OCR, determine byte vs. block addressing. */
    r = sd_cmd(58, 0, 1);
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

    r = sd_cmd(16, SD_BLOCK_SIZE, 0); /* CMD16: set block length */
    if (r != 0x00)
        goto out;

    /* CMD59: data-block CRCs on (the card computes, we verify). */
    r = sd_cmd(59, 1, 0);
    g_crc_on = (r == 0x00);
    if (!g_crc_on)
        printf("SD: CMD59 rejected, CRC off, fixed 400 kHz data\r\n");

    sd_probe_speed();
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

    printf("SD: FAT%d, %u clusters x %u sectors\r\n",
           g_fat32 ? 32 : 16,
           (unsigned)g_total_clusters,
           (unsigned)g_sec_per_clus);
    return 0;
}

int sd_mount(void)
{
    if (g_fs_mounted)
        return 0;
    if (!g_sd_ready) {
        if (sd_init() != 0)
            return -1;
        g_sd_ready = 1;
    }
    if (fat_mount() != 0)
        return -2;
    g_fs_mounted = 1;
    return 0;
}

u32 sd_card_kb(void)
{
    if (!g_fs_mounted)
        return 0;
    return g_total_clusters * g_sec_per_clus / 2u;   /* 512-byte sectors */
}

int sd_card_probe(char *msg, u32 msg_len)
{
    int ret;

    if (!msg || msg_len == 0)
        return -1;

    ret = sd_mount();
    if (ret == -1) {
        snprintf(msg, msg_len, "init failed");
        return -1;
    }
    if (ret == -2) {
        snprintf(msg, msg_len, "mount failed");
        return -2;
    }

    snprintf(msg, msg_len, "FAT%d %uKB", g_fat32 ? 32 : 16,
             (unsigned)sd_card_kb());
    return 0;
}

static u32 fat_next_locked(u32 cluster)
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
        if (sd_read_block_locked(lba, g_fat_sec) != 0)
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

struct fat_next_arg {
    u32 cluster;
    u32 value;
};

static int fat_next_op(void *arg)
{
    struct fat_next_arg *a = (struct fat_next_arg *)arg;

    a->value = fat_next_locked(a->cluster);
    return 0;
}

static u32 fat_next(u32 cluster)
{
    struct fat_next_arg a = {cluster, FAT_INVALID};

    if (spi_bus_run(SPI_DEV_SD, fat_next_op, &a) != 0)
        return FAT_INVALID;
    return a.value;
}

static int dir_entry(const u8 *e, const char *name11, u32 *cluster, u32 *size)
{
    u8 attr = e[11];

    if (e[0] == 0x00)
        return -2;                           /* end of directory */
    if (e[0] == 0xE5)
        return 0;                            /* deleted */
    if (attr == 0x0F || (attr & 0x08) || (attr & 0x10))
        return 0;                            /* LFN, volume, directory */

    if (memcmp(e, name11, SD_NAME_LEN) != 0)
        return 0;

    *cluster = g_fat32 ? (((u32)rd16(e + 20) << 16) | rd16(e + 26))
                       : (u32)rd16(e + 26);
    *size = rd32(e + 28);
    return 1;
}

static int sd_find(const char name11[SD_NAME_LEN], u32 *cluster_out,
                   u32 *size_out)
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
                    int r = dir_entry(g_sec + i, name11, &cluster, &size);

                    if (r == 1) {
                        *cluster_out = cluster;
                        *size_out = size;
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
                int r = dir_entry(g_sec + i, name11, &cluster, &size);

                if (r == 1) {
                    *cluster_out = cluster;
                    *size_out = size;
                    return 0;
                }
                if (r == -2)
                    return -1;
            }
        }
    }
    return -1;
}

/* ---- File stream ------------------------------------------------------ */

static int stream_start(u32 cluster, u32 size)
{
    if (cluster < 2 || size == 0)
        return -1;

    g_file_cluster = cluster;
    g_file_left = size;
    g_file_clus_lba = g_data_start + (cluster - 2u) * g_sec_per_clus;
    g_file_sec_idx = 0;
    g_file_off = 0;
    g_file_sec_valid = 0;
    return 0;
}

int sd_file_open(const char name11[SD_NAME_LEN], u32 *size)
{
    u32 cluster;
    u32 sz;

    if (!g_fs_mounted)
        return -1;
    if (sd_find(name11, &cluster, &sz) != 0)
        return -1;
    if (stream_start(cluster, sz) != 0)
        return -1;

    g_file_first_cluster = cluster;
    g_file_total_size = sz;
    g_file_open = 1;
    if (size)
        *size = sz;
    return 0;
}

int sd_file_rewind(void)
{
    if (!g_file_open)
        return -1;
    return stream_start(g_file_first_cluster, g_file_total_size);
}

/* Runs inside one SD bus job: a whole frame is one queue/semaphore round. */
static int sd_file_read_locked(u8 *out, u32 len)
{
    while (len) {
        u32 take;

        if (!g_file_sec_valid) {
            u32 lba = g_file_clus_lba + g_file_sec_idx;

            if (sd_read_block_locked(lba, g_sec) != 0)
                return -1;
            g_file_sec_valid = 1;
        }

        take = SD_BLOCK_SIZE - g_file_off;
        if (take > len)
            take = len;
        memcpy(out, g_sec + g_file_off, take);

        out += take;
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
                next = fat_next_locked(g_file_cluster);
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

struct sd_file_read_arg {
    u8 *buf;
    u32 len;
};

static int sd_file_read_op(void *arg)
{
    struct sd_file_read_arg *a = (struct sd_file_read_arg *)arg;

    return sd_file_read_locked(a->buf, a->len);
}

int sd_file_read(void *buf, u32 len)
{
    struct sd_file_read_arg a = {(u8 *)buf, len};

    if (!g_file_open || len > g_file_left)
        return -1;
    return spi_bus_run(SPI_DEV_SD, sd_file_read_op, &a);
}

void sd_file_close(void)
{
    g_file_open = 0;
    g_file_left = 0;
}
