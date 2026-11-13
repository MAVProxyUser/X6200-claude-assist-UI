/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An RTL-SDR stick's spectrum for the Sentry's F VHF/UHF screen (rtl_spectrum.c): /usr/bin/x6x00-rtlspec
 *  (tools/rtlspec) runs while it is wanted and its rows are kept here, the newest one. AIRSPACE lends it the stick
 *  (adsb_rx_lend) meanwhile and gets it back after.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RTL_SPEC_OFF,           /* not wanted */
    RTL_SPEC_NO_HELPER,     /* no /usr/bin/x6x00-rtlspec on this image */
    RTL_SPEC_NO_STICK,      /* no RTL-SDR on USB */
    RTL_SPEC_BUSY,          /* the stick, held by another program for now */
    RTL_SPEC_OPENING,       /* started, its first row not here yet */
    RTL_SPEC_ON,            /* rows coming */
} rtl_spec_state_t;

#define RTL_SPEC_MIN_HZ  24000000LL
#define RTL_SPEC_MAX_HZ  1766000000LL

/* Wanted (on, at hz with gain tenths of dB, -1 the tuner's AGC) or not: the helper starts or ends in the background */
void rtl_spectrum_want(bool on, int64_t hz, int gain);

/* While on: another frequency (kept within RTL_SPEC_MIN_HZ..MAX_HZ), another gain */
void rtl_spectrum_tune(int64_t hz);
void rtl_spectrum_gain(int tenths);

rtl_spec_state_t rtl_spectrum_state(void);

/* The tuner the helper found ("R820T"), "" before */
void rtl_spectrum_tuner(char *s, size_t n);

/* The newest row: up to cap dB values from the lowest frequency, its sample rate (the span) and centre. False when
 * there is none, or none newer than *seq (which it updates). fresh: the first since a retune */
bool rtl_spectrum_frame(float *db, uint16_t cap, uint16_t *n, uint32_t *rate, int64_t *centre, uint32_t *seq,
                        bool *fresh);
