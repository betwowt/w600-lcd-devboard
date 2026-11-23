/*
 * Four push buttons on the XL9555 expander.
 *
 *   KEY0..KEY3 = XL9555 P0_4..P0_7, 10 kOhm pull-ups, switch to GND.
 *   Pressed reads 0 on the expander input port.
 */
#ifndef KEY_H
#define KEY_H

#include "board.h"

#define KEY_COUNT       4
#define KEY_DEBOUNCE_MS 20u

/* Ensure P0_4..P0_7 are inputs; reset the debounce state. */
void key_init(void);

/* Raw read: bit 0..3 = KEY0..KEY3, 1 = pressed.  No debounce. */
u8 key_read_raw(void);

/* Debounced read.  Call every 10-20 ms; same bit layout as key_read_raw(). */
u8 key_scan(void);

/* Debounced state of one key; index 0..3. */
int key_pressed(u8 index);

#endif /* KEY_H */
