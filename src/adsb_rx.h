/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - ADS-B receiver: a GNS 5890 (04d8:f8e8) or a microADSB (04d8:000a, sprut's adsbPIC) on USB, read by a
 *  thread of its own into adsb.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ADSB_RX_OFF,            /* not started */
    ADSB_RX_NO_STICK,       /* no receiver on USB */
    ADSB_RX_NO_DRIVER,      /* the stick, but no cdc_acm in the kernel */
    ADSB_RX_OPENING,        /* the stick, its serial port not there yet */
    ADSB_RX_ON,             /* reading */
} adsb_rx_state_t;

/* Started once, it listens till the GUI ends, the screen open or not; the stick may come and go */
void adsb_rx_start(void);

/* The state; dev gets the serial port ("/dev/ttyACM0") when there is one */
adsb_rx_state_t adsb_rx_state(char *dev, size_t n);

/* The receiver found: "MICROADSB FW 0E", "GNS 5890"; "" none */
void adsb_rx_name(char *name, size_t n);

/* Bytes read since the start */
uint64_t adsb_rx_bytes(void);
