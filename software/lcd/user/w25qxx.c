/*
 * W25Q64 (8 MB) SPI NOR flash on the W600 module dev board.
 *
 *   CS   PB15   (shared SPI flash chip select, active low)
 *   CLK  PA01
 *   MOSI PB18
 *   MISO PB17
 *
 * Why GPIO bit-bang: the W25Q64JVSIQ answers the ID command correctly on
 * all four bit-banged GPIO lines (EF 40 17), while the W600 host-SPI
 * receive path returns FF in modes 0/1/2/3 and for 0x9F/0x90.  The host
 * controller is still used for the LCD (TX) and by the SD fallback; the
 * Flash is kept on the proven GPIO path.
 *
 * CLK/MOSI/MISO are shared with the LCD and TF card.  Every bus operation
 * ends with lcd_spi_resume() restoring the LCD's host-SPI setup.
 */
#include <string.h>

#include "board.h"
#include "lcd.h"
#include "w25qxx.h"
#include "spi_bus.h"

#define W25QXX_CS       WM_IO_PB_15
#define W25QXX_CLK      WM_IO_PA_01
#define W25QXX_MOSI     WM_IO_PB_18
#define W25QXX_MISO     WM_IO_PB_17

#define CMD_WRITE_ENABLE    0x06
#define CMD_READ_STATUS1    0x05
#define CMD_READ_DATA       0x03
#define CMD_PAGE_PROGRAM    0x02
#define CMD_SECTOR_ERASE    0x20
#define CMD_JEDEC_ID        0x9F
#define CMD_MANUF_DEV_ID    0x90

#define W25QXX_ID_HZ        1000000u
#define W25QXX_ID_SLOW_HZ   512000u
#define W25QXX_READ_HZ      8000000u
#define W25QXX_WRITE_HZ     4000000u

static u32 g_id;
static u32 g_id_hz = W25QXX_ID_HZ;
static int g_host_read_ok;

static void flash_bb_pins(u32 hz)
{
    (void)spi_bus_set_clock(SPI_DEV_FLASH, hz);

    tls_gpio_cfg(W25QXX_CS, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    (void)spi_bus_cs_high(SPI_DEV_FLASH);
    tls_gpio_cfg(W25QXX_CLK, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(W25QXX_CLK, 0);
    tls_gpio_cfg(W25QXX_MOSI, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(W25QXX_MOSI, 1);
    tls_gpio_cfg(W25QXX_MISO, WM_GPIO_DIR_INPUT, WM_GPIO_ATTR_PULLHIGH);
}

static void flash_bb_cs(int low)
{
    /* Callers use 0 = assert (CS low), 1 = deassert (CS high). */
    if (low)
        (void)spi_bus_cs_high(SPI_DEV_FLASH);
    else
        (void)spi_bus_cs_low(SPI_DEV_FLASH);
}

static u8 flash_bb_xfer(u8 out)
{
    u8 in = 0;
    int i;

    for (i = 7; i >= 0; i--) {
        tls_gpio_write(W25QXX_MOSI, (out >> i) & 1);
        spi_bus_bb_delay();
        tls_gpio_write(W25QXX_CLK, 1);
        spi_bus_bb_delay();
        in = (u8)((in << 1) | (tls_gpio_read(W25QXX_MISO) ? 1 : 0));
        tls_gpio_write(W25QXX_CLK, 0);
        spi_bus_bb_delay();
    }
    return in;
}

static void flash_bb_read(u8 *buf, u32 len)
{
    while (len--)
        *buf++ = flash_bb_xfer(0xFF);
}

static void flash_bb_write(const u8 *buf, u32 len)
{
    while (len--)
        (void)flash_bb_xfer(*buf++);
}

static void flash_bb_cmd(u8 cmd)
{
    flash_bb_cs(0);
    (void)flash_bb_xfer(cmd);
    flash_bb_cs(1);
}

static void flash_bb_addr_cmd(u8 cmd, u32 addr)
{
    flash_bb_cs(0);
    (void)flash_bb_xfer(cmd);
    (void)flash_bb_xfer((u8)(addr >> 16));
    (void)flash_bb_xfer((u8)(addr >> 8));
    (void)flash_bb_xfer((u8)addr);
}

static int range_ok(u32 addr, u32 len)
{
    if (len == 0)
        return 1;
    if (addr >= W25QXX_CAPACITY)
        return 0;
    return len <= W25QXX_CAPACITY - addr;
}

static int flash_id_known(u32 id)
{
    return id == W25QXX_JEDEC_ID ||   /* W25Q64 JEDEC */
           id == 0xEF4018u ||         /* W25Q128 JEDEC */
           id == 0xEF1600u ||         /* W25Q64 0x90 */
           id == 0xEF1700u;           /* W25Q128 0x90 */
}

static void flash_host_setup(void);

static int flash_host_cmd(u8 cmd)
{
    int ret;

    flash_host_setup();
    flash_bb_cs(0);
    ret = tls_spi_write(&cmd, 1);
    flash_bb_cs(1);
    return ret;
}

static int flash_host_page(u32 addr, const void *buf, u16 len)
{
    u8 hdr[4];
    int ret;

    hdr[0] = CMD_PAGE_PROGRAM;
    hdr[1] = (u8)(addr >> 16);
    hdr[2] = (u8)(addr >> 8);
    hdr[3] = (u8)addr;

    flash_host_setup();
    flash_bb_cs(0);
    ret = tls_spi_write_with_cmd(hdr, sizeof(hdr), (const u8 *)buf, len);
    flash_bb_cs(1);
    return ret;
}

static int flash_host_erase(u32 addr)
{
    u8 hdr[4];
    int ret;

    hdr[0] = CMD_SECTOR_ERASE;
    hdr[1] = (u8)(addr >> 16);
    hdr[2] = (u8)(addr >> 8);
    hdr[3] = (u8)addr;

    flash_host_setup();
    flash_bb_cs(0);
    ret = tls_spi_write(hdr, sizeof(hdr));
    flash_bb_cs(1);
    return ret;
}

static int w25qxx_read_id_locked(u8 id[3])
{
    u8 mfg[2];

    if (!id)
        return -1;

    flash_bb_pins(g_id_hz);
    flash_bb_cs(0);
    DELAY_MS(1);                    /* CS low well before the first edge */
    (void)flash_bb_xfer(CMD_JEDEC_ID);
    id[0] = flash_bb_xfer(0xFF);
    id[1] = flash_bb_xfer(0xFF);
    id[2] = flash_bb_xfer(0xFF);
    flash_bb_cs(1);

    g_id = ((u32)id[0] << 16) | ((u32)id[1] << 8) | id[2];
    printf("FLASH: 0x9F BB = %02X %02X %02X\r\n",
           (unsigned)id[0], (unsigned)id[1], (unsigned)id[2]);
    if (flash_id_known(g_id))
        return 0;

    flash_bb_cs(0);
    (void)flash_bb_xfer(CMD_MANUF_DEV_ID);
    (void)flash_bb_xfer(0);
    (void)flash_bb_xfer(0);
    (void)flash_bb_xfer(0);
    mfg[0] = flash_bb_xfer(0xFF);
    mfg[1] = flash_bb_xfer(0xFF);
    flash_bb_cs(1);

    id[0] = mfg[0];
    id[1] = mfg[1];
    id[2] = 0;
    g_id = ((u32)mfg[0] << 16) | ((u32)mfg[1] << 8);
    printf("FLASH: 0x90 BB = %02X %02X\r\n",
           (unsigned)mfg[0], (unsigned)mfg[1]);
    return 0;
}

static int w25qxx_read_id_op(void *arg)
{
    return w25qxx_read_id_locked((u8 *)arg);
}

int w25qxx_read_id(u8 id[3])
{
    return spi_bus_run(SPI_DEV_FLASH, w25qxx_read_id_op, id);
}

u32 w25qxx_id(void)
{
    return g_id;
}

static void flash_host_setup(void)
{
    wm_spi_ck_config(W25QXX_CLK);
    wm_spi_do_config(W25QXX_MOSI);
    wm_spi_di_config(W25QXX_MISO);
    wm_spi_cs_config(WM_IO_PB_00);
    tls_spi_trans_type(SPI_DMA_TRANSFER);
    (void)tls_spi_setup(TLS_SPI_MODE_3, TLS_SPI_CS_LOW, 20000000u);
}

static int flash_host_read_status(u8 *st)
{
    u8 cmd = CMD_READ_STATUS1;

    flash_host_setup();
    flash_bb_cs(0);
    if (tls_spi_read_with_cmd(&cmd, 1, st, 1) != 0) {
        flash_bb_cs(1);
        return -1;
    }
    flash_bb_cs(1);
    return 0;
}

static int flash_host_read(u32 addr, void *buf, u32 len)
{
    u8 hdr[4];

    if (len > SPI_DMA_BUF_MAX_SIZE)
        return -1;

    hdr[0] = CMD_READ_DATA;
    hdr[1] = (u8)(addr >> 16);
    hdr[2] = (u8)(addr >> 8);
    hdr[3] = (u8)addr;

    flash_host_setup();
    flash_bb_cs(0);
    if (tls_spi_read_with_cmd(hdr, sizeof(hdr), (u8 *)buf, len) != 0) {
        flash_bb_cs(1);
        return -1;
    }
    flash_bb_cs(1);
    return 0;
}

int w25qxx_init(void)
{
    u8 id[3] = {0, 0, 0};
    int ret;

    ret = w25qxx_read_id(id);
    if (ret != 0 || !flash_id_known(g_id)) {
        printf("FLASH: retry at %u Hz\r\n", (unsigned)W25QXX_ID_SLOW_HZ);
        g_id_hz = W25QXX_ID_SLOW_HZ;
        ret = w25qxx_read_id(id);
    }

    if (ret != 0 || !flash_id_known(g_id)) {
        printf("FLASH: W25Q64/W25Q128 not found (want EF 40 17/18)\r\n");
        printf("FLASH: check U1 supply, PB15 CS and PB17 MISO\r\n");
        return -1;
    }

    {
        u8 bb_st;
        u8 dma_st = 0xFF;

        flash_bb_pins(W25QXX_ID_HZ);
        flash_bb_cs(0);
        (void)flash_bb_xfer(CMD_READ_STATUS1);
        bb_st = flash_bb_xfer(0);
        flash_bb_cs(1);

        g_host_read_ok = 0;
        if (flash_host_read_status(&dma_st) == 0 && bb_st == dma_st)
            g_host_read_ok = 1;
        printf("FLASH: host DMA %s%s\r\n",
               g_host_read_ok ? "OK " : "off",
               g_host_read_ok ? "(20 MHz)" : "(BB fallback)");
    }

    printf("FLASH: W25Q ready, %u bytes\r\n", (unsigned)W25QXX_CAPACITY);
    printf("FLASH: clocks id/read/write = %u/%u/%u Hz\r\n",
           (unsigned)W25QXX_ID_HZ, (unsigned)W25QXX_READ_HZ,
           (unsigned)W25QXX_WRITE_HZ);
    return 0;
}

static int w25qxx_wait_ready_locked(u32 timeout_ms)
{
    u32 start = tls_os_get_time();

    flash_bb_pins(W25QXX_ID_HZ);

    for (;;) {
        u8 st;

        flash_bb_cs(0);
        (void)flash_bb_xfer(CMD_READ_STATUS1);
        st = flash_bb_xfer(0xFF);
        flash_bb_cs(1);

        if ((st & 0x01u) == 0)
            return 0;
        if ((tls_os_get_time() - start) * 2u > timeout_ms) {
            printf("FLASH: busy timeout (%u ms)\r\n", (unsigned)timeout_ms);
            return -1;
        }
        DELAY_MS(2);
    }
}

static int w25qxx_wait_ready_op(void *arg)
{
    return w25qxx_wait_ready_locked((u32)(u32)arg);
}

int w25qxx_wait_ready(u32 timeout_ms)
{
    return spi_bus_run(SPI_DEV_FLASH, w25qxx_wait_ready_op,
                       (void *)(u32)timeout_ms);
}

static int w25qxx_read_locked(u32 addr, void *buf, u32 len)
{
    if (!buf || !range_ok(addr, len))
        return -1;
    if (len == 0)
        return 0;

    if (g_host_read_ok) {
        if (flash_host_read(addr, buf, len) == 0)
            return 0;
        g_host_read_ok = 0;
        printf("FLASH: host read failed, falling back to BB\r\n");
    }

    flash_bb_pins(W25QXX_READ_HZ);
    flash_bb_addr_cmd(CMD_READ_DATA, addr);
    flash_bb_read((u8 *)buf, len);
    flash_bb_cs(1);
    return 0;
}

struct w25qxx_buf_arg {
    u32 addr;
    void *buf;
    u32 len;
};

static int w25qxx_read_op(void *arg)
{
    struct w25qxx_buf_arg *a = (struct w25qxx_buf_arg *)arg;

    return w25qxx_read_locked(a->addr, a->buf, a->len);
}

int w25qxx_read(u32 addr, void *buf, u32 len)
{
    struct w25qxx_buf_arg a = {addr, buf, len};

    return spi_bus_run(SPI_DEV_FLASH, w25qxx_read_op, &a);
}

static int w25qxx_page_program_locked(u32 addr, const void *buf, u16 len)
{
    if (!buf || len == 0 || len > W25QXX_PAGE_SIZE)
        return -1;
    if (!range_ok(addr, len) ||
        (addr & (W25QXX_PAGE_SIZE - 1u)) + len > W25QXX_PAGE_SIZE)
        return -1;

    if (g_host_read_ok) {
        if (flash_host_cmd(CMD_WRITE_ENABLE) == 0 &&
            flash_host_page(addr, buf, len) == 0)
            return w25qxx_wait_ready_locked(100u);
        g_host_read_ok = 0;
        printf("FLASH: host write failed, falling back to BB\r\n");
    }

    flash_bb_pins(W25QXX_WRITE_HZ);
    flash_bb_cmd(CMD_WRITE_ENABLE);
    flash_bb_addr_cmd(CMD_PAGE_PROGRAM, addr);
    flash_bb_write((const u8 *)buf, len);
    flash_bb_cs(1);

    return w25qxx_wait_ready_locked(100u);
}

struct w25qxx_page_arg {
    u32 addr;
    const void *buf;
    u16 len;
};

static int w25qxx_page_program_op(void *arg)
{
    struct w25qxx_page_arg *a = (struct w25qxx_page_arg *)arg;

    return w25qxx_page_program_locked(a->addr, a->buf, a->len);
}

int w25qxx_page_program(u32 addr, const void *buf, u16 len)
{
    struct w25qxx_page_arg a = {addr, buf, len};

    return spi_bus_run(SPI_DEV_FLASH, w25qxx_page_program_op, &a);
}

static int w25qxx_erase_sector_locked(u32 addr)
{
    if (addr >= W25QXX_CAPACITY)
        return -1;
    addr &= ~(W25QXX_SECTOR_SIZE - 1u);

    if (g_host_read_ok) {
        if (flash_host_cmd(CMD_WRITE_ENABLE) == 0 &&
            flash_host_erase(addr) == 0)
            return w25qxx_wait_ready_locked(2000u);
        g_host_read_ok = 0;
        printf("FLASH: host erase failed, falling back to BB\r\n");
    }

    flash_bb_pins(W25QXX_WRITE_HZ);
    flash_bb_cmd(CMD_WRITE_ENABLE);
    flash_bb_addr_cmd(CMD_SECTOR_ERASE, addr);
    flash_bb_cs(1);

    return w25qxx_wait_ready_locked(2000u);
}

static int w25qxx_erase_sector_op(void *arg)
{
    return w25qxx_erase_sector_locked((u32)(u32)arg);
}

int w25qxx_erase_sector(u32 addr)
{
    return spi_bus_run(SPI_DEV_FLASH, w25qxx_erase_sector_op,
                       (void *)(u32)addr);
}

static int w25qxx_write_locked(u32 addr, const void *buf, u32 len)
{
    const u8 *p = (const u8 *)buf;
    u32 end;
    u32 s;

    if (!buf || !range_ok(addr, len))
        return -1;
    if (len == 0)
        return 0;

    end = addr + len;

    for (s = addr & ~(W25QXX_SECTOR_SIZE - 1u); s < end;
         s += W25QXX_SECTOR_SIZE) {
        if (w25qxx_erase_sector_locked(s) != 0)
            return -1;
    }

    while (addr < end) {
        u32 page_room = W25QXX_PAGE_SIZE - (addr & (W25QXX_PAGE_SIZE - 1u));
        u32 n = end - addr;

        if (n > page_room)
            n = page_room;
        if (w25qxx_page_program_locked(addr, p, (u16)n) != 0)
            return -1;
        addr += n;
        p += n;
    }
    return 0;
}

static int w25qxx_write_op(void *arg)
{
    struct w25qxx_buf_arg *a = (struct w25qxx_buf_arg *)arg;

    return w25qxx_write_locked(a->addr, a->buf, a->len);
}

int w25qxx_write(u32 addr, const void *buf, u32 len)
{
    struct w25qxx_buf_arg a = {addr, buf, len};

    return spi_bus_run(SPI_DEV_FLASH, w25qxx_write_op, &a);
}
