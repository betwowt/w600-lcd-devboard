/*
 * Minimal SDK headers for this example.
 *
 * Do not include "wm_include.h": it pulls the lwIP socket headers, whose
 * in_addr_t/timeval/select definitions collide with newer newlib versions.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdio.h>
#include <stdlib.h>

#include "wm_type_def.h"
#include "wm_gpio.h"
#include "wm_gpio_afsel.h"
#include "wm_hostspi.h"
#include "wm_osal.h"

/* configTICK_RATE_HZ is 500 in this SDK, so one tick is 2 ms. */
#define DELAY_MS(ms) tls_os_time_delay((ms) / 2)

#endif /* BOARD_H */
