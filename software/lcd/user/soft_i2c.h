/*
 * Bit-banged I2C for the W600 dev board.
 *
 * The bus is selected at runtime so the same code drives the XL9555
 * (PB09/PB12) and the touch controller (PB13/PB14).
 */
#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include "board.h"

void si2c_set_speed(u8 dly);
void si2c_set_pins(enum tls_io_name scl, enum tls_io_name sda);
int si2c_scl_level(void);
int si2c_sda_level(void);

int si2c_probe(u8 addr);
int si2c_write_regs(u8 addr, u8 reg, const u8 *buf, u8 len);
int si2c_read_regs(u8 addr, u8 reg, u8 *buf, u8 len);
int si2c_read_regs_stop(u8 addr, u8 reg, u8 *buf, u8 len);

void si2c_bus_recover(void);
void si2c_pin_check(const char *tag);
void si2c_scan(u8 lo, u8 hi, const char *tag);

#endif /* SOFT_I2C_H */
