/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Page I's link to the direction finder, /usr/bin/x6x00-df (tools/rtlspec/df.c; the wire format in
 *  tools/rtlspec/df_proto.h). Started when page I opens, told to end ("Q") when it closes; a thread here connects to
 *  it, keeps the newest of each of its messages and sends the GUI's settings again to a daemon started anew.
 *  While page I is open the SDR pool gives it the whole KrakenSDR (SDR_OWN_DF: AIRSPACE's spares and the watch
 *  channels let go of its sticks) and B, if it was on one of them, moves to another SDR or is lent (its daemon ended
 *  till page I closes).
 */

#pragma once

#include "../tools/rtlspec/df_proto.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DF_RX_HIST 128          /* DoA spectra kept for the bearing waterfall */

/* What page I draws, copied out under the lock */
typedef struct {
    bool        connected;      /* the daemon answers */
    bool        have_st, have_do, have_sp;
    df_st_msg_t st;
    df_do_msg_t dd;             /* the newest estimate */
    df_sp_msg_t sp;
    uint32_t    do_n;           /* estimates taken since page I opened (the history's newest is hist[(do_n-1) % H]) */
    uint32_t    st_ms;          /* when DFST last came (lv_tick-like ms), 0 never */
} df_snap_t;

/* A KrakenSDR on USB (three or more of serials 1000-1004), or the test source chosen (df_rx_synth): page I on the
 * ring */
bool df_rx_present(void);

/* The test source (x6x00-df -T: a synthetic transmitter, no sticks) instead of the Kraken: a setting kept by page I,
 * or the file /mnt/df_synth */
bool df_rx_synth(void);
void df_rx_set_synth(bool on);

/* Page I opened / closed: the daemon started or ended, the pool's DF owner wanted or not, B moved or lent and back */
void df_rx_open(void);
void df_rx_close(void);
/* What B does now because of page I: 0 nothing, 1 moved to another SDR, 2 lent (no SDR for B till page I closes) */
int  df_rx_b_lent(void);

/* A command to the daemon (df_proto.h), kept and said again to a daemon started anew (all but "c" and "Q") */
void df_rx_cmd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The newest of everything; false while there is no daemon */
bool df_rx_snapshot(df_snap_t *s);
/* The DoA spectrum of estimate number n (0 .. do_n - 1), if still kept: 360 bytes, false if gone */
bool df_rx_hist(uint32_t n, uint8_t *p, uint8_t *gate);

#ifdef __cplusplus
}
#endif
