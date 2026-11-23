/*
 * Shared SPI bus: one command queue, one device owning the bus at a time.
 *
 * Devices on PA01/PB18/PB17:
 *   LCD   CS PB11   (hardware SPI, 20 MHz)
 *   Flash CS PB15   (GPIO bit-bang)
 *   TF    CS PB16   (GPIO bit-bang)
 *
 * Drivers submit a transaction to the queue; a dedicated bus task executes
 * transactions in FIFO order.  Chip selects are driven only by
 * spi_bus_cs_low()/spi_bus_cs_high(), which enforce:
 *   - only the current transaction's device may drive its CS;
 *   - at most one CS may be low.
 * The worker force-deselects on every exit and restores the master-SPI mux,
 * so a bit-bang transaction can never leak into the next LCD transaction.
 */
#ifndef SPI_BUS_H
#define SPI_BUS_H

#include "board.h"

typedef enum {
    SPI_DEV_NONE = 0,
    SPI_DEV_LCD,
    SPI_DEV_FLASH,
    SPI_DEV_SD
} spi_dev_t;

void spi_bus_init(void);

/*
 * Submit fn(arg) to the bus queue and wait for it to finish.
 * fn runs in the bus task; it may call spi_bus_cs_low/high().
 * Returns fn's return value, or -1 if the queue is unavailable/full.
 */
int spi_bus_run(spi_dev_t dev, int (*fn)(void *arg), void *arg);

/* Called only from inside a queued transaction. */
int spi_bus_cs_low(spi_dev_t dev);
int spi_bus_cs_high(spi_dev_t dev);

/*
 * Per-device clock.  LCD reprograms the master SPI; Flash/SD select the
 * bit-bang half-period that meets their timing.  Must run in the device's
 * transaction (owner check).
 */
int spi_bus_set_clock(spi_dev_t dev, u32 hz);

/* Delay used by bit-bang drivers, derived from the current device clock. */
void spi_bus_bb_delay(void);

#endif /* SPI_BUS_H */
