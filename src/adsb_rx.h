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

/* The receivers listened on: "MICROADSB FW 0E", "GNS 5890 + RTL-SDR R820T"; "" none */
void adsb_rx_name(char *name, size_t n);

/* Bytes read since the start */
uint64_t adsb_rx_bytes(void);

/* An RTL-SDR lent to the Sentry's spectrum (on) or given to AIRSPACE (off): lent, dump1090 is stopped and not started
 * again until it comes back. The serial receivers (GNS 5890, microADSB) go on, and so does a spare RTL-SDR (below) */
void adsb_rx_lend(bool on);

/* The kinds of receiver AIRSPACE listens on, one at a time */
typedef enum {
    ADSB_KIND_NONE,
    ADSB_KIND_GNS,          /* GNS 5890 */
    ADSB_KIND_MICRO,        /* microADSB */
    ADSB_KIND_RTL,          /* an RTL-SDR: the Sentry's SDR, or a spare one beside a HackRF or a bladeRF */
    ADSB_KIND_HACKRF,       /* the Sentry's SDR */
    ADSB_KIND_BLADERF,      /* the Sentry's SDR (with no HackRF) */
    ADSB_KIND_N
} adsb_kind_t;

/* Those AIRSPACE can listen on now, a bit (1 << kind) each: the serial receivers on USB; the Sentry's SDR (the one the
 * daemon holds: B's choice, else a HackRF, a bladeRF, an RTL-SDR); an RTL-SDR beside a HackRF or a bladeRF it holds, a
 * spare: dump1090 opens it itself, so B keeps its SDR, and it listens with AIRSPACE closed as the serial ones do. Only
 * what tunes 1090 MHz. From the thread that looks at USB each second: 0 for the first second */
unsigned adsb_rx_choices(void);

/* The kinds on USB, a bit each, whether they can be listened on or not (B's SDRs among them) */
unsigned adsb_rx_present(void);

/* An RTL-SDR on USB is a spare (the daemon holds a HackRF or a bladeRF beside it) */
bool adsb_rx_rtl_spare(void);

/* Its name: "GNS 5890", "MICROADSB", "RTL-SDR", "HACKRF", "BLADERF"; short: "GNS", "MICRO", "RTL", ...; "" NONE */
const char *adsb_rx_kind_name(adsb_kind_t k);
const char *adsb_rx_kind_short(adsb_kind_t k);

/* What to listen on, a bit each kind: one, or a serial one and an SDR at once (BOTH: both feed the aircraft); the thread
 * changes over within a second. 0, or none of them there: the first serial one and the first SDR (a spare RTL-SDR,
 * else the Sentry's) */
void adsb_rx_choose(unsigned kinds);

/* What is listened on (or being opened) now, a bit each; and what will be, as things are on USB */
unsigned adsb_rx_kinds_now(void);
unsigned adsb_rx_kinds_next(void);

/* An RTL-SDR on USB, whoever has it. These three read what a thread of their own sees each second (no sysfs on the
 * caller's thread: a stuck USB host never hangs the GUI); false or 0 for the first second */
bool adsb_rx_sdr_present(void);

/* The SDR's kind on USB: "RTL-SDR", "HACKRF", "BLADERF"; "" none */
const char *adsb_rx_sdr_kind(void);

/* A receiver of its own on USB (GNS 5890, microADSB), AIRSPACE open or not */
bool adsb_rx_receiver_present(void);

/* The RTL-SDR's USB identity (bus * 1000 + device number), 0 none: another number once it was unplugged and plugged
 * in again (rtl_spectrum.c waits for that before it opens a stick that sent nothing) */
int adsb_rx_sdr_id(void);

/* How many times that thread has looked at USB: 0, the three above say nothing yet */
int adsb_rx_usb_looks(void);
