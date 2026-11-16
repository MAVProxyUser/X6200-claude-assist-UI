/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Watch channels (rtl_watch.c): the spare RTL-SDRs of a KrakenSDR (or any on a hub of their own power)
 *  each on a channel of the user's, squelched, their audio on the speaker while they are open
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_WATCH_N 4

typedef struct {
    int64_t hz;
    int     mode;       /* 1 NFM, 2 AM, 3 WFM (rtl_spectrum_listen's) */
    int     place;      /* RTL<n> it is on, -1 none free */
    bool    up;         /* its daemon streaming */
    bool    open;       /* its squelch open (with its hang) */
    int     over_db;    /* its level over the noise, dB */
} rtl_watch_info_t;

/* Started once (the saved channels, BOARD_DATA "/rtl_watch") */
void rtl_watch_init(void);

/* hz watched (mode, squelch dB over the noise, gain in the RTL's tenths) or no longer: 1 added, 0 removed, -1 none
 * added (no free RTL of its own power, or RTL_WATCH_N already) */
int rtl_watch_toggle(int64_t hz, int mode, int sql_db, int gain);
bool rtl_watch_has(int64_t hz);

int  rtl_watch_count(void);
bool rtl_watch_get(int i, rtl_watch_info_t *out);
/* RTLs free for another watch channel now */
int  rtl_watch_free(void);

/* The player (rtl_spectrum.c): any channel open, or a moment ago (its hang): the speaker wanted; their audio added
 * into buf, n samples at 44.1 kHz, each faded in and out with its squelch */
bool rtl_watch_active(void);
void rtl_watch_mix(int16_t *buf, int n);

#ifdef __cplusplus
}
#endif
