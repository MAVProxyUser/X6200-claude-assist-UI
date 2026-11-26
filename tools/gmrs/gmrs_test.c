/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/gmrs.c, GMRS's channels (the owner's licence, 2026-11-25), against the rules written out here
 *  (47 CFR 95.1763, 95.1767, 95.1773): the 22 channels' frequencies, bandwidths and power limits, the repeater pairs
 *  of 15-22 (input = output + 5 MHz), the names B and G show, the scan's order (every channel once, by frequency), the
 *  lookups' tolerance, the 38 privacy codes (each one of ctcss.c's tones) and the 104 DCS codes. And receive only:
 *  every channel and input refused by every TX check there is (ham_tx_check through a transverter with the switch
 *  on, tx_ranges_check with each profile, tx_ranges_sdr_check, the daemon's own amateur bands never reaching them),
 *  with GMRS's own reason, and the 70 cm band's top still sent on. And what the Sentry draws from it: the scan's next
 *  place (B's and A's GMRS scans), F's GMRS panel's 30 rows, G's channel numbers, the tones' rows.
 */

#include "cfg/gmrs.h"
#include "cfg/ham_bands.h"
#include "cfg/tx_ranges.h"
#include "ctcss.h"

#include <stdio.h>
#include <string.h>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* The channels as the rules give them, written out, not read back from the table */
static int32_t want_hz(int ch) {
    if (ch <= 7) return 462562500 + (ch - 1) * 25000;       /* 462 MHz interstitial */
    if (ch <= 14) return 467562500 + (ch - 8) * 25000;      /* 467 MHz interstitial */
    return 462550000 + (ch - 15) * 25000;                   /* 462 MHz main */
}

static int32_t want_in(int ch) {
    return ch >= 15 ? want_hz(ch) + 5000000 : 0;
}

static void channels(void) {
    char name[16], in_name[16];

    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) {
        const gmrs_ch_t *c = gmrs_ch(ch);

        check(c != NULL, "channel there", ch);
        if (!c) continue;
        check(c->ch == ch, "its number", ch);
        check(c->hz == want_hz(ch), "its frequency", ch);
        check(c->hz % 12500 == 0, "on the 12.5 kHz grid (B's step)", ch);
        check(c->bw_hz == (ch >= 8 && ch <= 14 ? 12500 : 20000), "its bandwidth (narrow on 8-14)", ch);
        check(c->w10 == (ch <= 7 ? 50 : ch <= 14 ? 5 : 500), "its power limit", ch);
        check(c->in_hz == (ch >= 15 ? c->hz + GMRS_RPT_SHIFT : 0), "its repeater input (15-22, +5 MHz)", ch);
        snprintf(name, sizeof(name), "GMRS %d", ch);
        snprintf(in_name, sizeof(in_name), "GMRS %d IN", ch);
        check(c->name && !strcmp(c->name, name), "its name", ch);
        check(ch >= 15 ? c->in_name && !strcmp(c->in_name, in_name) : c->in_name == NULL, "its input's name", ch);
        check(c->note && *c->note && strlen(c->note) <= 24, "its note, short (G's list row)", ch);
        for (int k = 1; k < ch; k++) {          /* no two on one frequency, an input on none of the channels */
            const gmrs_ch_t *o = gmrs_ch(k);

            check(o->hz != c->hz, "frequencies apart", ch);
            check(!c->in_hz || o->hz != c->in_hz, "an input on no channel", ch);
        }
    }
    check(gmrs_ch(0) == NULL && gmrs_ch(23) == NULL, "none past 1-22", 0);
}

static void lookups(void) {
    bool in;

    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) {
        const gmrs_ch_t *c = gmrs_ch(ch);

        check(gmrs_find(c->hz, 0, &in) == ch && !in, "found on its frequency", ch);
        check(gmrs_find(c->hz + 3000, 3000, &in) == ch && !in, "found 3 kHz off with 3 kHz", ch);
        check(gmrs_find(c->hz + 3001, 3000, NULL) == 0, "not 3001 Hz off with 3 kHz", ch);
        check(!strcmp(gmrs_name(c->hz, 3000), c->name), "named", ch);
        if (c->in_hz) {
            check(gmrs_find(c->in_hz, 3000, &in) == ch && in, "its input found as an input", ch);
            check(!strcmp(gmrs_name(c->in_hz - 2000, 3000), c->in_name), "its input named", ch);
        }
    }
    check(gmrs_find(462600000 - 6250, 3000, NULL) == 0, "nothing between 17 and 3", 462593750);
    check(gmrs_name(446000000, 3000) == NULL, "70 cm's calling channel no GMRS", 446000000);
    check(gmrs_name(467537500, 3000) == NULL, "under 467.550 none", 467537500);
}

static void scan_order(void) {
    int seen[GMRS_CHANNELS + 1] = {0}, prev = 0;

    for (int i = 0; i < GMRS_CHANNELS; i++) {
        int              ch = gmrs_scan_ch(i);
        const gmrs_ch_t *c  = gmrs_ch(ch);

        check(c != NULL, "the scan's channel", i);
        if (!c) continue;
        seen[ch]++;
        check(c->hz > prev, "the scan by frequency", ch);
        prev = c->hz;
    }
    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) check(seen[ch] == 1, "each channel scanned once", ch);
    check(gmrs_scan_ch(-1) == 0 && gmrs_scan_ch(GMRS_CHANNELS) == 0, "none past the scan's ends", 0);
}

static void tones(void) {
    static const int want[GMRS_CTCSS_N] = {
        670,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148, 1188, 1230,
        1273, 1318, 1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799, 1862, 1928, 2035, 2107, 2181, 2257, 2336,
        2418, 2503};

    for (int n = 1; n <= GMRS_CTCSS_N; n++) {
        check(gmrs_ctcss(n) == want[n - 1], "privacy code's tone", n);
        check(ctcss_valid(gmrs_ctcss(n)) == gmrs_ctcss(n), "one of ctcss.c's tones", n);
    }
    check(gmrs_ctcss(22) == 1413, "code 22: 141.3 Hz (channel 20's)", 22);
    check(gmrs_ctcss(0) == 0 && gmrs_ctcss(39) == 0, "none past 1-38", 0);
    for (int i = 0; i < GMRS_DCS_N; i++) {
        int d = gmrs_dcs(i);

        check(d > 0 && d <= 777 && d % 10 <= 7 && d / 10 % 10 <= 7 && d / 100 <= 7, "DCS code octal", d);
        check(i == 0 || d > gmrs_dcs(i - 1), "DCS codes sorted, none twice", d);
    }
    check(gmrs_dcs(0) == 23 && gmrs_dcs(GMRS_DCS_N - 1) == 754, "DCS 023 to 754", 0);
    check(gmrs_dcs(GMRS_DCS_N) == 0, "none past the 104", 0);
}

/* The scans' next place: -1 the first, each of the 22 once a round, round again */
static void scan_next(void) {
    int at = -1, seen[GMRS_CHANNELS + 1] = {0};

    check(gmrs_scan_next(-1) == 0, "the scan starts at its first", 0);
    for (int k = 0; k < GMRS_CHANNELS; k++) {
        at = gmrs_scan_next(at);
        check(at == k, "the scan's places in turn", at);
        seen[gmrs_scan_ch(at)]++;
    }
    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) check(seen[ch] == 1, "a round: each channel once", ch);
    check(gmrs_scan_next(GMRS_CHANNELS - 1) == 0, "round again after the last", 0);
    check(gmrs_scan_next(57) == 58 % GMRS_CHANNELS, "a place left from another list: on from there", 57);
}

/* F SYSTEM's GMRS panel: 1-22 by number, then the inputs of 15-22; G's names "1".."22" */
static void panel(void) {
    bool in;
    char want[8];

    check(GMRS_ROWS == 30, "the panel's rows", GMRS_ROWS);
    for (int r = 0; r < GMRS_ROWS; r++) {
        int              ch = gmrs_row_ch(r, &in);
        const gmrs_ch_t *c  = gmrs_ch(ch);

        check(c != NULL, "a row's channel", r);
        if (!c) continue;
        check(ch == (r < GMRS_CHANNELS ? r + 1 : r - GMRS_CHANNELS + 15), "the rows' order", r);
        check(in == (r >= GMRS_CHANNELS), "the inputs after the channels", r);
        check(gmrs_row_hz(r) == (in ? c->in_hz : c->hz), "a row's frequency", r);
        check(gmrs_row_hz(r) == (in ? want_in(ch) : want_hz(ch)), "a row's frequency as the rules give it", r);
    }
    check(gmrs_row_ch(-1, &in) == 0 && !in && gmrs_row_hz(-1) == 0, "no row before the first", -1);
    check(gmrs_row_ch(GMRS_ROWS, &in) == 0 && !in && gmrs_row_hz(GMRS_ROWS) == 0, "no row past the last", GMRS_ROWS);
    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) {
        snprintf(want, sizeof(want), "%d", ch);
        check(!strcmp(gmrs_num(ch), want), "G's channel name", ch);
    }
    check(!strcmp(gmrs_num(0), "") && !strcmp(gmrs_num(23), ""), "no name past 1-22", 0);
}

/* The tones' rows as F's panel lists them: every privacy code and every DCS code once, in order, each row short enough
 * for the panel (60 characters) */
static void tone_rows(void) {
    char t[128], want[48];    /* room for any int, or GCC 11's -Wformat-truncation stops the build (CI) */
    int  code = 1, dcs = 0;

    for (int r = 0; r < GMRS_TONE_ROWS; r++) {
        gmrs_tone_row(r, t, sizeof(t));
        check(t[0] != 0, "a tone row there", r);
        check(strlen(t) <= 60, "a tone row fits the panel", (long)strlen(t));
        if (r == 0) check(strstr(t, "141.3") != NULL, "the heading names channel 20's tone", r);
        if (r == 11) check(strstr(t, "DCS") != NULL, "the DCS heading", r);
        if (r >= 1 && r <= 10) {
            for (const char *p = t; code <= GMRS_CTCSS_N;) {
                snprintf(want, sizeof(want), "%2d %3d.%d", code, gmrs_ctcss(code) / 10, gmrs_ctcss(code) % 10);
                if (!(p = strstr(p, want))) break;
                p += strlen(want);
                code++;
            }
        }
        if (r >= 12) {
            for (const char *p = t; dcs < GMRS_DCS_N;) {
                snprintf(want, sizeof(want), "%03d", gmrs_dcs(dcs));
                if (!(p = strstr(p, want))) break;
                p += strlen(want);
                dcs++;
            }
        }
    }
    check(code == GMRS_CTCSS_N + 1, "every privacy code in the rows, in order", code - 1);
    check(dcs == GMRS_DCS_N, "every DCS code in the rows, in order", dcs);
    gmrs_tone_row(GMRS_TONE_ROWS, t, sizeof(t));
    check(t[0] == 0, "nothing past the last tone row", GMRS_TONE_ROWS);
    gmrs_tone_row(-1, t, sizeof(t));
    check(t[0] == 0, "nothing before the first tone row", -1);
    gmrs_tone_row(1, t, 8);
    check(strlen(t) < 8, "a short buffer kept", (long)strlen(t));
}

/* Receive only: whatever the checks are asked, nothing on GMRS */
static void no_tx_at(int32_t hz, int ch) {
    check(ham_gmrs(hz), "in ham_gmrs's spans", ch);
    check(ham_tx_check(hz, true, true, true) == HAM_TX_GMRS, "through a transverter, the switch on: GMRS", ch);
    check(ham_tx_check(hz, false, true, true) == HAM_TX_RX_ONLY, "no hardware there: receive only", ch);
    for (int p = TX_RANGES_BANDS; p <= TX_RANGES_OWN; p++) {
        check(tx_ranges_sdr_check((tx_ranges_t)p, hz) == HAM_TX_GMRS, "the SDR: GMRS", ch);
        check(tx_ranges_check((tx_ranges_t)p, hz, true, 434000000, true) != HAM_TX_OK, "a transverter: refused", ch);
    }
    check(!tx_ranges_sdr_band(hz, NULL, NULL), "in none of the SDR's amateur bands", ch);
}

static void receive_only(void) {
    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) {
        const gmrs_ch_t *c = gmrs_ch(ch);

        no_tx_at(c->hz, ch);
        no_tx_at(c->hz - c->bw_hz / 2, ch);         /* the channel's edges too */
        no_tx_at(c->hz + c->bw_hz / 2, ch);
        if (c->in_hz) {
            no_tx_at(c->in_hz, ch);
            no_tx_at(c->in_hz + c->bw_hz / 2, ch);
        }
    }
    check(tx_ranges_why(HAM_TX_GMRS) && strstr(tx_ranges_why(HAM_TX_GMRS), "GMRS"), "the refusal names GMRS", 0);
    check(!ham_gmrs(462537500 - 1) && !ham_gmrs(467737500 + 1) && !ham_gmrs(465000000), "GMRS's spans no wider", 0);
    check(tx_ranges_sdr_check(TX_RANGES_BANDS, 450000000) == HAM_TX_OK, "70 cm's top still sent on", 450000000);
    check(ham_tx_check(14200000, true, false, false) == HAM_TX_OK, "20 m still sent on", 14200000);
    check(ham_tx_check(436000000, true, true, false) == HAM_TX_OK, "a 70 cm transverter still sends", 436000000);
}

int main(void) {
    channels();
    lookups();
    scan_order();
    tones();
    scan_next();
    panel();
    tone_rows();
    receive_only();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
