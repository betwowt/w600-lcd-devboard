/*
 * RGB LED on the XL9555 expander.
 *
 *   LED_R = P1_0, LED_G = P1_1, LED_B = P1_2, 1 kOhm series resistors.
 *   LED1 common anode is tied to +3.3 V, so the pins are active low.
 *   The expander has no PWM; each channel is on/off only.
 */
#ifndef LED_H
#define LED_H

#include "board.h"

typedef enum {
    LED_OFF = 0,
    LED_RED = 1,
    LED_GREEN = 2,
    LED_YELLOW = 3,
    LED_BLUE = 4,
    LED_MAGENTA = 5,
    LED_CYAN = 6,
    LED_WHITE = 7
} led_color_t;

/* Configure P1_0..P1_2 as outputs and turn all channels off. */
void led_init(void);

/* Any non-zero argument turns that channel on. */
void led_rgb(int r, int g, int b);

void led_set(led_color_t color);
void led_off(void);

#endif /* LED_H */
