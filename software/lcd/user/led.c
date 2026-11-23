#include "board.h"
#include "xl9555.h"
#include "led.h"

#define LED_MASK (XL9555_P1_LED_R | XL9555_P1_LED_G | XL9555_P1_LED_B)

void led_init(void)
{
    /* Output latch high = all channels off (common anode, active low). */
    xl9555_p1_patch(LED_MASK, LED_MASK);
}

void led_rgb(int r, int g, int b)
{
    u8 v = LED_MASK;

    if (r)
        v &= (u8)~XL9555_P1_LED_R;
    if (g)
        v &= (u8)~XL9555_P1_LED_G;
    if (b)
        v &= (u8)~XL9555_P1_LED_B;

    xl9555_p1_patch(LED_MASK, v);
}

void led_set(led_color_t color)
{
    led_rgb((color & LED_RED) != 0,
            (color & LED_GREEN) != 0,
            (color & LED_BLUE) != 0);
}

void led_off(void)
{
    led_rgb(0, 0, 0);
}
