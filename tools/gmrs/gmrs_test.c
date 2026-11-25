/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/cfg/gmrs.c, GMRS's channels (the owner's licence, 2026-11-25), against the rules written out here
 *  (47 CFR 95.1763, 95.1767, 95.1773): the 22 channels' frequencies, bandwidths and power limits, the repeater pairs
 *  of 15-22 (input = output + 5 MHz), the names B and G show, the scan's order (every channel once, by frequency), the
 *  lookups' tolerance, the 38 privacy codes (each one of ctcss.c's tones) and the 104 DCS codes. And receive only:
 *  every channel and input refused by every TX check there is (ham_tx_check through a transverter with the switch
 *  on, tx_ranges_check with each profile, tx_ranges_sdr_check, the daemon's own amateur bands never reaching them),
 *  with GMRS's own reason, and the 70 cm band's top still sent on.
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
    receive_only();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
