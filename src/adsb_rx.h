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
    ADSB_RX_LENT,           /* an RTL-SDR, the Sentry's while AIRSPACE's screen is closed: dump1090 stopped */
} adsb_rx_state_t;

/* Started once, it listens till the GUI ends, the screen open or not; the stick may come and go */
void adsb_rx_start(void);

/* The state; dev gets the serial port ("/dev/ttyACM0") when there is one */
adsb_rx_state_t adsb_rx_state(char *dev, size_t n);

/* The receiver found: "MICROADSB FW 0E", "GNS 5890"; "" none */
void adsb_rx_name(char *name, size_t n);

/* Bytes read since the start */
uint64_t adsb_rx_bytes(void);

/* An RTL-SDR lent to the Sentry's spectrum (on) or given to AIRSPACE (off): lent, dump1090 is stopped and not started
 * again until it comes back. The serial receivers (GNS 5890, microADSB) go on */
void adsb_rx_lend(bool on);

/* An RTL-SDR on USB, whoever has it. These three read what a thread of their own sees each second (no sysfs on the
 * caller's thread: a stuck USB host never hangs the GUI); false or 0 for the first second */
bool adsb_rx_sdr_present(void);

/* A receiver of its own on USB (GNS 5890, microADSB), AIRSPACE open or not */
bool adsb_rx_receiver_present(void);

/* The RTL-SDR's USB identity (bus * 1000 + device number), 0 none: another number once it was unplugged and plugged
 * in again (rtl_spectrum.c waits for that before it opens a stick that sent nothing) */
int adsb_rx_sdr_id(void);

/* How many times that thread has looked at USB: 0, the three above say nothing yet */
int adsb_rx_usb_looks(void);
