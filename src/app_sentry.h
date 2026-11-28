/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Sentry: a listener's receiver on the base's whole spectrum (app_sentry.c)
 */

#pragma once

#include "app_window.h"
#include <stdint.h>
#include "board.h"

/* The pages' control as the screens name it, the Sentry's and AIRSPACE's and the direction finder's: the X6200's MFK
 * inner ring; the X6100's knob has none, the MFK held and the main dial turned stand in (screen_main.c) */
#ifdef BOARD_DEV_MFK_INNER
#define SENTRY_RING_HINT  "RING: SCREEN"        /* the footers */
#define SENTRY_RING_SHORT "RING"
#define SENTRY_RING_NAME  "THE MFK'S RING"
#else
#define SENTRY_RING_HINT  "MFK+VFO: PAGE"
#define SENTRY_RING_SHORT "MFK HELD + VFO"
#define SENTRY_RING_NAME  "THE MFK HELD AND THE VFO TURNED"
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_sentry;

/* The Sentry opened on a screen of its ring by its letter, "A" to "G"; "H" opens AIRSPACE (app_adsb.c), its eighth */
void app_sentry_open_at(char letter);
/* The page shown, "A" to "G" (SCAN CFG "S"), 0 when the Sentry is not open */
char app_sentry_page(void);
/* What the speaker plays: 0 the radio's own receiver, 1 the SDR at B's channel, 2 both, 3 nothing (the remote's
 * "heard"); and set for now, each page's saved LISTEN left alone (LVGL thread) */
int app_sentry_heard(void);
void app_sentry_set_heard(int h);
/* G's own channel (s.wch Hz, s.wchm the X6200's mode; 0 and -1: none yet, CB 19 then): read from any thread (the
 * remote's "wch"); set with the LVGL thread, G's memory only, the dial not tuned (the remote's "set wch HZ MODE",
 * what the bench puts back); loaded from the GUI's state once it is read (gui_main.c) */
void app_sentry_g_channel(int32_t *hz, int32_t *mode);
void app_sentry_g_channel_set(int32_t hz, int32_t mode);
void app_sentry_g_channel_load(void);
/* B and G on the SDR, for the remote's state (any thread; the LVGL thread publishes it every 50 ms while the Sentry
 * is open, all zero while it is not): B's channel heard (Hz), its preset or band's name, its mode as heard ("NFM",
 * "DMR"), the channel's name ("GMRS 20", "" none), B's F1 scan (0 off, 1 the band, 2 the channel list, 3 EMERGENCY's,
 * 4 GMRS's 22 channels); G's SDR channel (Hz, 0 G on the X6200), its
 * transmitter armed (A/B) and why the SDR may not transmit there ("" it may: w_sdr_refusal's words, "GMRS: ..." on
 * GMRS's channels); an SDR there; F's choice (its item's name, "" off F) and F's GMRS panel: open, its chosen
 * channel (Hz) and the receiver its F1 tunes (0 none, 1 B's SDR, 2 the radio's own: the WR12); G's lamp's word while
 * G is shown ("ON AIR", "LOCKED", "ARMED", "DMR", a digital voice's name, "SIGNAL", "QUIET"; "" off G); D's chosen
 * band ("40 M", "" off D) and E's chosen type ("NOAA ALERTS", "" off E), by their names on the screen */
typedef struct {
    int64_t bhz, ghz, gmhz;
    int32_t bscan, sdr, garm, gmp, gmrx;
    char    bpre[16], bmode[12], bname[16], gno[56], fsel[24], glamp[12], dsel[24], esel[24];
} app_sentry_sdr_t;
void app_sentry_sdr_state(app_sentry_sdr_t *out);
/* Any thread: the rows A's history has drawn for the time on the air (dim, at the rows' pace) since the GUI started:
 * the bench's a-tx-rows counts them over an over */
uint32_t app_sentry_tx_rows(void);
/* F's NIFOG for the remote's "nifog?" (any thread; the LVGL thread publishes it every 50 ms while the Sentry is open,
 * all zero while it is not): open or not, the page (the PDF's numbering), how far down it (px) and the most it goes,
 * and the words as the screen has them: the caption, the line under the page, the legend, the F keys' boxes */
typedef struct {
    int32_t open, page, y, maxy;
    char    cap[160], where[200], legend[160], keys[5][32];
} app_sentry_nifog_t;
void app_sentry_nifog_state(app_sentry_nifog_t *out);
/* The remote's "set bch HZ" (LVGL thread): B's channel at HZ with the narrowest preset it is in, B's scan stopped, as
 * by hand; only while B shows. "set gsdr HZ": G's SDR channel at HZ (one of G's plan's, GMRS's among them), 0 G back
 * on the X6200 at its own channel (s.wch, CB 19 for none); only while G shows with an SDR there and nothing armed or
 * keyed */
void app_sentry_b_channel_set(int64_t hz);
void app_sentry_g_sdr_set(int64_t hz);
/* The remote's "set gmp 1|0" (LVGL thread): F SYSTEM's GMRS chosen on F's list and its panel opened (1), or the panel
 * closed (0); only while F shows */
void app_sentry_gmrs_panel(bool on);
/* F's GHOSTNET page for the remote's "ghostnet?" (any thread; the LVGL thread publishes it every 50 ms while the
 * Sentry is open, all zero while the page is closed): open or not, A on the net now (7.107 MHz USB-D), and the words as
 * the screen has them: its head, TUNE's line and the line under it, the antenna and D's band, the schedule's head and
 * its rows (the JS8 windows on 7.107, the next first), the line on who listens, the keys' line, the legend, the F
 * keys' boxes */
typedef struct {
    int32_t open, on;
    char    head[96], tune[96], how[200], ant[64], band[96], sched_head[80], sched[5][100], watch[200], foot[120];
    char    legend[160], keys[5][32];
} app_sentry_ghostnet_t;
void app_sentry_ghostnet_state(app_sentry_ghostnet_t *out);
/* The remote's "set gnp 1|0" (LVGL thread): F SYSTEM's GHOSTNET chosen on F's list and its page opened (1), or closed
 * (0); only while F shows */
void app_sentry_ghostnet_page(bool on);
/* F's TRIBAL KNOWLEDGE for the remote's "tribal?" (any thread, published every 50 ms while it is open, zero while
 * closed): open, the page (1 the first) of pages, and that page's text as WHAT IT DOES draws it (upper case) */
typedef struct {
    int32_t open, page, pages;
    char    text[2048];
} app_sentry_tribal_t;
void app_sentry_tribal_state(app_sentry_tribal_t *out);
/* The SDR pool's panel (sdr_pool.h), the Sentry opened on F for it when it is not open (APP held on H) */
void app_sentry_open_pool(void);

/* The release and the commit the GUI is built from, "v0.3.12.19 f739c40": in every Sentry title, AIRSPACE's too */
const char *app_sentry_release(void);

#ifdef __cplusplus
}
#endif
