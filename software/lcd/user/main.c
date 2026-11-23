/*
 * W600 module dev board LCD demo.
 *
 * Panel: ILI9341 240x320 SPI, 16-pin FPC
 *   SPI_CLK  PA01        LCD_CS   PB11
 *   SPI_MOSI PB18        LCD_DC   XL9555 P0_3
 *   LCD_RST  XL9555 P0_0 LCD_PWR  XL9555 P0_2
 *   XL9555 SCL PB09 / SDA PB12, address 0x24
 *   Touch: I2C PB13/PB14, TP_RST XL9555 P0_1, TP_INT PB10 (polling)
 *
 * The drawing state matches the STM32 P024C135 demo: text, full-screen
 * colors, color bars, grayscale, then handwriting with a white brush and a
 * Clear button in the bottom-right corner.
 */
#include "board.h"
#include "lcd.h"
#include "logo.h"
#include "touch.h"
#include "xl9555.h"
#include "lcd_demo.h"
#include "drivers_test.h"
#include "spi_bus.h"
#include "w25qxx.h"
#ifdef BAD_APPLE
#include "badapple.h"
#endif
#ifdef BOARD_CHECK
#include "board_check.h"
#endif

#if defined(BOARD_CHECK) && (defined(LCD_TEST) || defined(DRIVER_TEST))
#error "BOARD_CHECK is exclusive with LCD_TEST and DRIVER_TEST"
#endif


/*
 * The prebuilt SDK libs (libapp.a) were compiled against newlib 2.1, which
 * used __ctype_ptr__. Newer newlib only exports _ctype_, so provide the
 * pointer the old HTTP string helpers reference.
 */
extern const char _ctype_[];
const char *__ctype_ptr__ = &_ctype_[1];

/*
 * 8 KB stack: the SD/FAT path was reliable at this size. Do not shrink it
 * for the half-frame logo buffer; the W600 RAM region still fits.
 */
#define USER_TASK_STK_SIZE  2048
#define USER_TASK_PRIO      32

static u32 user_task_stk[USER_TASK_STK_SIZE];

#ifdef LCD_TEST
static void lcd_test_loop(void)
{
    printf("LCDTEST: start (re-init after each PWR change)\r\n");

    for (;;) {
        if (lcd_init_pwr(0) == 0) {
            lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, LCD_RED);
            lcd_text(20, 140, "PWR LOW", LCD_WHITE, LCD_RED, 2);
            printf("LCDTEST: PWR low, red\r\n");
        } else {
            printf("LCDTEST: PWR low init failed\r\n");
        }
        DELAY_MS(3000);

        if (lcd_init_pwr(1) == 0) {
            lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, LCD_GREEN);
            lcd_text(20, 140, "PWR HIGH", LCD_WHITE, LCD_GREEN, 2);
            printf("LCDTEST: PWR high, green\r\n");
        } else {
            printf("LCDTEST: PWR high init failed\r\n");
        }
        DELAY_MS(3000);
    }
}
#endif

static void lcd_demo_task(void *arg)
{
    int ret;
    int has_logo = 0;

    (void)arg;

#ifdef FLASH_ONLY
    for (;;) {
        printf("FLASHONLY: probe\r\n");
        (void)w25qxx_init();
        DELAY_MS(2000);
    }
#endif

    ret = lcd_init();
    if (ret != 0) {
        printf("LCD init failed: %d (check XL9555 on PB9/PB12, 0x24)\r\n", ret);
#ifdef BOARD_CHECK
        printf("BOARD: continuing UART tests without LCD\r\n");
#else
        for (;;) {
            DELAY_MS(1000);
            printf("LCD init failed: %d\r\n", ret);
            xl9555_diag();
        }
#endif
    }

#if !defined(LCD_TEST) && !defined(DRIVER_TEST) && !defined(BOARD_CHECK)
    /*
     * SD init must come after lcd_init(): the earlier working sequence
     * had XL9555/LCD_PWR and the shared pins already settled.
     */
    has_logo = (lcd_logo_load() == 0);
#endif

#ifdef LCD_TEST
    lcd_test_loop();
#endif

#ifdef DRIVER_TEST
    drivers_test();
#endif

#ifdef BOARD_CHECK
    board_check_run();
#endif

#if !defined(LCD_TEST) && !defined(DRIVER_TEST) && !defined(BOARD_CHECK)
    if (has_logo)
        lcd_logo_display();
    DELAY_MS(3000);
#endif

#ifdef BAD_APPLE
    /* Streams /BADAPPLE.BIN until a key is pressed, then falls through. */
    (void)badapple_run();
#endif

#if !defined(LCD_TEST) && !defined(DRIVER_TEST) && !defined(BOARD_CHECK)
    if (touch_init() != 0)
        printf("TOUCH: demo continues without touch\r\n");

    lcd_demo_run();
#endif
}

void pre_gpio_config(void)
{
    /*
     * Called from wm_gpio_config() before the SPI/flash drivers start.
     * Keep both chip selects deasserted while the bus is still floating.
     */
    tls_gpio_cfg(WM_IO_PB_11, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_FLOATING);
    tls_gpio_write(WM_IO_PB_11, 1);   /* LCD_CS */
    tls_gpio_cfg(WM_IO_PB_15, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    tls_gpio_write(WM_IO_PB_15, 1);   /* W25Q64 CS */

    /*
     * PB15 is the W600 default hspi_cs.  Move the hardware CS mux to the
     * unexposed PB00 so the host SPI controller cannot drive PB15 while
     * the LCD is using the shared CLK/MOSI lines.
     */
    wm_spi_cs_config(WM_IO_PB_00);

    /*
     * SD_CS must be high while the card powers up, otherwise it can enter
     * native SD mode instead of SPI mode (observed as CMD0 -> 00).
     */
    tls_gpio_cfg(WM_IO_PB_16, WM_GPIO_DIR_OUTPUT, WM_GPIO_ATTR_PULLHIGH);
    tls_gpio_write(WM_IO_PB_16, 1);   /* SD_CS */
}

void UserMain(void)
{
    printf("\r\nW600 LCD demo, compile @%s %s\r\n", __DATE__, __TIME__);
    printf("Panel ILI9341 240x320; SPI PA1/PB18, CS PB11, ctrl XL9555@0x24\r\n");

    spi_bus_init();

    tls_os_task_create(NULL,
            "lcd",
            lcd_demo_task,
            (void *)0,
            (void *)user_task_stk,
            USER_TASK_STK_SIZE * sizeof(u32),
            USER_TASK_PRIO,
            0);

    /*
     * Keep the startup task here instead of returning into
     * tls_sys_auto_mode_run(): the demo never touches SPI flash while the
     * LCD owns the shared SPI bus.
     */
    for (;;)
        tls_os_time_delay(0x10000000);
}
