/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the time from WWV and WWVH (NIST, 2.5 to 25 MHz in AM), decoded from the receiver's audio (wwv.c):
 *  the second ticks (5 ms of 1000 Hz from WWV, 1200 Hz from WWVH) give the second, the 100 Hz subcarrier's BCD code
 *  the minute (#261, #275). Each second the 100 Hz starts 30 ms in and drops at 200 ms (a 0), 500 ms (a 1) or 800 ms
 *  (a marker, at :09 :19 ... :59); second :00 has none. A frame is the 60 seconds of a minute: the year, the day of
 *  the year, the hour and the minute that began at its :00, DUT1, DST.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wwv_s wwv_t;

typedef struct {
    bool     locked;            /* the second ticks found: the seconds below count */
    int      station;           /* 1 WWV (1000 Hz ticks), 2 WWVH (1200 Hz), 0 none */
    float    tick_db;           /* the ticks over the rest of the second, dB */
    float    code_db;           /* the 100 Hz on over off, dB (15 sent) */
    int      second;            /* the second of the minute now, -1 not known yet */
    char     symbols[61];       /* this minute's so far, at their seconds: 0 1 M (marker) - (no 100 Hz) ? (unclear) */
    /* The last frame decoded whole */
    bool     have_time;
    int      year, yday, hour, minute;      /* the minute that began at its :00, UTC; year 2000-2099 */
    float    dut1;              /* UT1 - UTC, s */
    bool     dst1, dst2, leap_warning;
    double   minute_t;          /* when its :00 tick came, in the time base wwv_feed's t0 is in */
    int      frames_ok;         /* frames in a row, each one minute after the one before */
    uint32_t frames, frames_bad;
} wwv_info_t;

/* rate: the audio's sample rate (11025 in the apps) */
wwv_t *wwv_new(float rate);
void   wwv_free(wwv_t *w);
void   wwv_reset(wwv_t *w);                 /* another frequency: start again */

/* Audio in; t0: when x[0] came, in the caller's time base (s, monotonic). True when wwv_info changed */
bool wwv_feed(wwv_t *w, const float *x, size_t n, double t0);
void wwv_info(const wwv_t *w, wwv_info_t *out);

/* The decoded minute's start as seconds since the epoch (UTC), from year, yday, hour, minute */
int64_t wwv_minute_utc(const wwv_info_t *in);

#ifdef __cplusplus
}
#endif
