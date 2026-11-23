/*
 * Touch controller on the LCD FPC.
 *
 * TP_RST  = XL9555 P0_1
 * TP_INT  = PB10 (not used, polling)
 * I2C_SCL = PB13, I2C_SDA = PB14
 */
#ifndef TOUCH_H
#define TOUCH_H

#include "board.h"

int touch_init(void);
/* 0 = no touch, 1 = touched, -1 = I2C error */
int touch_read(u16 *x, u16 *y);

#endif /* TOUCH_H */
