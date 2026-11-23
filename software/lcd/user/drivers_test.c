/*
 * Self-test for the three board drivers: W25Q64 flash, four keys, RGB LED.
 * Build the example with -DDRIVER_TEST to run this instead of the LCD demo.
 */
#include "board.h"
#include "drivers_test.h"
#include "lcd.h"
#include "xl9555.h"
#include "w25qxx.h"
#include "key.h"
#include "led.h"

#ifdef DRIVER_TEST

static void led_sweep(void)
{
    printf("DRV: LED R\r\n");
    led_rgb(1, 0, 0);
    DELAY_MS(700);
    printf("DRV: LED G\r\n");
    led_rgb(0, 1, 0);
    DELAY_MS(700);
    printf("DRV: LED B\r\n");
    led_rgb(0, 0, 1);
    DELAY_MS(700);
    printf("DRV: LED W (R+G+B)\r\n");
    led_rgb(1, 1, 1);
    DELAY_MS(700);
    led_off();
}

void drivers_test(void)
{
    u8 id[3] = {0, 0, 0};
    u8 key;
    u8 last = 0xFE;     /* impossible raw value, forces the first print */
    int flash_ok;

    printf("DRV: start\r\n");

    flash_ok = (w25qxx_init() == 0);
    if (flash_ok && w25qxx_read_id(id) == 0) {
        u8 first[16];

        if (w25qxx_read(0, first, sizeof(first)) == 0) {
            printf("DRV: flash[0..15] %02X %02X %02X %02X ...\r\n",
                   first[0], first[1], first[2], first[3]);
        }
    }

    key_init();
    led_init();
    printf("DRV: keys and RGB ready (KEY0=R, KEY1=G, KEY2=B, KEY3=white)\r\n");
    led_sweep();

    lcd_fill(0, 0, LCD_W - 1, LCD_H - 1, LCD_BLACK);
    lcd_text(10, 10, "Driver test", LCD_WHITE, LCD_BLACK, 2);
    lcd_text(10, 44, flash_ok ? "W25Q64 OK" : "W25Q64 FAIL",
             flash_ok ? LCD_GREEN : LCD_RED, LCD_BLACK, 1);
    lcd_text(10, 66, "KEY0=R KEY1=G", LCD_CYAN, LCD_BLACK, 1);
    lcd_text(10, 86, "KEY2=B KEY3=W", LCD_CYAN, LCD_BLACK, 1);
    lcd_text(10, 120, "Press keys", LCD_YELLOW, LCD_BLACK, 1);

    for (;;) {
        int white;

        key = key_scan();
        if (key != last) {
            u8 in0 = 0;

            (void)xl9555_read_regs(XL9555_REG_IN0, &in0, 1);
            printf("DRV: IN0=0x%02X keys=0x%02X (KEY0=R KEY1=G KEY2=B KEY3=W)\r\n",
                   (unsigned)in0, (unsigned)key);
            last = key;
        }

        white = (key & 0x08) != 0;
        led_rgb((key & 0x01) || white,
                (key & 0x02) || white,
                (key & 0x04) || white);
        DELAY_MS(20);
    }
}

#endif /* DRIVER_TEST */
