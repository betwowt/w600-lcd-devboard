/*
 * Board validation demo: LCD + W25Q64 + TF card + Wi-Fi scan.
 *
 * Build with -DBOARD_CHECK.  The LCD shows one line per subsystem and a
 * PASS/FAIL summary; the UART prints the full result.  Flash and SD use the
 * shared SPI bus through the queued arbiter in spi_bus.c, so no other
 * device is selected while a transaction is in flight.
 */
#include <string.h>

#include "board.h"
#include "lcd.h"
#include "logo.h"
#include "w25qxx.h"
#include "board_check.h"
#include "wm_wifi.h"

#ifdef BOARD_CHECK

#define SCAN_BUF_SIZE   4096u
#define SCAN_TIMEOUT_MS 20000u

static u8 g_scan_buf[SCAN_BUF_SIZE] __attribute__((aligned(4)));
static volatile u8 g_scan_done;

static void scan_done_cb(void)
{
    g_scan_done = 1;
}

static int flash_test(char *msg, u32 msg_len)
{
    u8 id[3] = {0, 0, 0};
    u8 wr[16];
    u8 rd[16];
    u32 addr = W25QXX_CAPACITY - W25QXX_SECTOR_SIZE;
    int i;

    if (w25qxx_init() != 0) {
        snprintf(msg, msg_len, "FAIL (no ID)");
        return -1;
    }
    if (w25qxx_read_id(id) != 0) {
        snprintf(msg, msg_len, "FAIL (ID read)");
        return -1;
    }

    for (i = 0; i < (int)sizeof(wr); i++)
        wr[i] = (u8)(0xA5u ^ (u8)i);

    if (w25qxx_erase_sector(addr) != 0) {
        snprintf(msg, msg_len, "FAIL (erase)");
        return -1;
    }
    if (w25qxx_read(addr, rd, sizeof(rd)) != 0) {
        snprintf(msg, msg_len, "FAIL (erase read)");
        return -1;
    }
    for (i = 0; i < (int)sizeof(rd); i++) {
        if (rd[i] != 0xFF) {
            snprintf(msg, msg_len, "FAIL (not erased)");
            return -1;
        }
    }

    if (w25qxx_page_program(addr, wr, sizeof(wr)) != 0) {
        snprintf(msg, msg_len, "FAIL (program)");
        return -1;
    }
    if (w25qxx_read(addr, rd, sizeof(rd)) != 0 ||
        memcmp(wr, rd, sizeof(wr)) != 0) {
        snprintf(msg, msg_len, "FAIL (verify)");
        return -1;
    }

    snprintf(msg, msg_len, "OK EF%02X%02X%02X",
             (unsigned)id[0], (unsigned)id[1], (unsigned)id[2]);
    return 0;
}

static int sd_test(char *msg, u32 msg_len)
{
    int ret = sd_card_probe(msg, msg_len);

    if (ret != 0)
        return ret;
    return 0;
}

static int wifi_test(char *msg, u32 msg_len, char *top, u32 top_len)
{
    struct tls_scan_bss_t *scan;
    struct tls_bss_info_t *best[3];
    u32 start;
    u32 count;
    int i;
    int tries;

    g_scan_done = 0;
    tls_wifi_scan_result_cb_register(scan_done_cb);

    for (tries = 0; tries < 3; tries++) {
        if (tls_wifi_scan() == WM_SUCCESS)
            break;
        DELAY_MS(300);
    }
    if (tries == 3) {
        tls_wifi_scan_result_cb_register(NULL);
        snprintf(msg, msg_len, "FAIL (scan start)");
        snprintf(top, top_len, " ");
        return -1;
    }

    start = tls_os_get_time();
    while (!g_scan_done &&
           (u32)((tls_os_get_time() - start) * 2u) < SCAN_TIMEOUT_MS)
        DELAY_MS(50);

    if (!g_scan_done) {
        tls_wifi_scan_result_cb_register(NULL);
        snprintf(msg, msg_len, "FAIL (timeout)");
        snprintf(top, top_len, " ");
        return -1;
    }

    if (tls_wifi_get_scan_rslt(g_scan_buf, sizeof(g_scan_buf)) != WM_SUCCESS) {
        tls_wifi_scan_result_cb_register(NULL);
        snprintf(msg, msg_len, "FAIL (result)");
        snprintf(top, top_len, " ");
        return -1;
    }
    tls_wifi_scan_result_cb_register(NULL);

    scan = (struct tls_scan_bss_t *)g_scan_buf;
    count = scan->count;
    {
        u32 max_count = (SCAN_BUF_SIZE - 8u) /
                        (u32)sizeof(struct tls_bss_info_t);

        if (count > max_count)
            count = max_count;
    }

    best[0] = best[1] = best[2] = NULL;
    for (i = 0; i < (int)count; i++) {
        struct tls_bss_info_t *b = &scan->bss[i];
        int k;

        for (k = 0; k < 3; k++) {
            if (!best[k] || (s8)b->rssi > (s8)best[k]->rssi) {
                int m;

                for (m = 2; m > k; m--)
                    best[m] = best[m - 1];
                best[k] = b;
                break;
            }
        }
    }

    snprintf(msg, msg_len, "OK %u AP", (unsigned)count);
    if (best[0]) {
        char ssid[33];
        u32 n = best[0]->ssid_len;

        if (n > 32)
            n = 32;
        memcpy(ssid, best[0]->ssid, n);
        ssid[n] = '\0';
        snprintf(top, top_len, "%s %ddBm", ssid, (int)(s8)best[0]->rssi);
    } else {
        snprintf(top, top_len, "no AP in range");
    }

    printf("BOARD: Wi-Fi scan count=%u top=%s\r\n", (unsigned)count, top);
    return 0;
}

static void draw_line(u16 y, const char *label, const char *value, int ok)
{
    u16 color = (ok == 0) ? LCD_GREEN : LCD_RED;

    lcd_text(8, y, label, LCD_WHITE, LCD_BLACK, 1);
    lcd_text(8, (u16)(y + 16), value, color, LCD_BLACK, 1);
}

static void draw_status(const char *flash_msg, int flash_ok,
                        const char *sd_msg, int sd_ok,
                        const char *wifi_msg, int wifi_ok,
                        const char *top)
{
    lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, LCD_BLACK);
    lcd_text(8, 8, "W600 BOARD CHECK", LCD_WHITE, LCD_BLACK, 2);
    lcd_text(8, 40, "LCD: OK", LCD_GREEN, LCD_BLACK, 1);

    draw_line(64, "FLASH", flash_msg, flash_ok);
    draw_line(104, "TF CARD", sd_msg, sd_ok);
    draw_line(144, "WI-FI", wifi_msg, wifi_ok);

    lcd_text(8, 192, "Strongest AP:", LCD_WHITE, LCD_BLACK, 1);
    lcd_text(8, 208, top, LCD_CYAN, LCD_BLACK, 1);

    if (flash_ok == 0 && sd_ok == 0 && wifi_ok == 0) {
        lcd_text(8, 252, "PASS  3/3", LCD_GREEN, LCD_BLACK, 2);
    } else {
        lcd_text(8, 252, "CHECK FAILURES", LCD_RED, LCD_BLACK, 2);
    }
    lcd_text(8, 292, "UART: detailed log", LCD_GRAY, LCD_BLACK, 1);
}

void board_check_run(void)
{
    char flash_msg[40] = "not run";
    char sd_msg[40] = "not run";
    char wifi_msg[40] = "not run";
    char top[48] = " ";
    int flash_ok = -1;
    int sd_ok = -1;
    int wifi_ok = -1;

    printf("\r\nBOARD: validation start\r\n");

    /*
     * Flash and SD are one-shot: the flash test erases one sector, and the
     * card is expected to be seated at power-up.
     */
    flash_ok = flash_test(flash_msg, sizeof(flash_msg));
    sd_ok = sd_test(sd_msg, sizeof(sd_msg));

    printf("BOARD: flash=%s SD=%s\r\n", flash_msg, sd_msg);

    draw_status(flash_msg, flash_ok, sd_msg, sd_ok,
                "scanning...", -1, " ");

    for (;;) {
        wifi_ok = wifi_test(wifi_msg, sizeof(wifi_msg), top, sizeof(top));
        draw_status(flash_msg, flash_ok, sd_msg, sd_ok,
                    wifi_msg, wifi_ok, top);
        DELAY_MS(5000);
    }
}

#endif /* BOARD_CHECK */
