/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GMRS's channels (gmrs.h). Sources (2026-11-25): 47 CFR 95.1763 (the channels), 95.1767 (power),
 *  95.1773 (bandwidths), 95.1761 (certified transmitters only); FRS shares the channels (95.563). The privacy codes
 *  are the EIA's 38 CTCSS tones in their usual order (code 22: 141.3 Hz, the one heard with channel 20); the DCS codes
 *  the 104 standard ones, which radios number from 39 up each in its own order.
 */

#include "gmrs.h"

#include <stdio.h>
#include <stdlib.h>

static const gmrs_ch_t chs[GMRS_CHANNELS] = {
    {1, 462562500, 0, 20000, 50, "GMRS 1", NULL, "462 INTERSTITIAL, 5 W"},
    {2, 462587500, 0, 20000, 50, "GMRS 2", NULL, "462 INTERSTITIAL, 5 W"},
    {3, 462612500, 0, 20000, 50, "GMRS 3", NULL, "462 INTERSTITIAL, 5 W"},
    {4, 462637500, 0, 20000, 50, "GMRS 4", NULL, "462 INTERSTITIAL, 5 W"},
    {5, 462662500, 0, 20000, 50, "GMRS 5", NULL, "462 INTERSTITIAL, 5 W"},
    {6, 462687500, 0, 20000, 50, "GMRS 6", NULL, "462 INTERSTITIAL, 5 W"},
    {7, 462712500, 0, 20000, 50, "GMRS 7", NULL, "462 INTERSTITIAL, 5 W"},
    {8, 467562500, 0, 12500, 5, "GMRS 8", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {9, 467587500, 0, 12500, 5, "GMRS 9", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {10, 467612500, 0, 12500, 5, "GMRS 10", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {11, 467637500, 0, 12500, 5, "GMRS 11", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {12, 467662500, 0, 12500, 5, "GMRS 12", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {13, 467687500, 0, 12500, 5, "GMRS 13", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {14, 467712500, 0, 12500, 5, "GMRS 14", NULL, "NARROW 12.5 KHZ, 0.5 W"},
    {15, 462550000, 467550000, 20000, 500, "GMRS 15", "GMRS 15 IN", "RPT OUT, IN 467.550"},
    {16, 462575000, 467575000, 20000, 500, "GMRS 16", "GMRS 16 IN", "RPT OUT, IN 467.575"},
    {17, 462600000, 467600000, 20000, 500, "GMRS 17", "GMRS 17 IN", "RPT OUT, IN 467.600"},
    {18, 462625000, 467625000, 20000, 500, "GMRS 18", "GMRS 18 IN", "RPT OUT, IN 467.625"},
    {19, 462650000, 467650000, 20000, 500, "GMRS 19", "GMRS 19 IN", "RPT OUT, IN 467.650"},
    {20, 462675000, 467675000, 20000, 500, "GMRS 20", "GMRS 20 IN", "RPT OUT, IN 467.675"},
    {21, 462700000, 467700000, 20000, 500, "GMRS 21", "GMRS 21 IN", "RPT OUT, IN 467.700"},
    {22, 462725000, 467725000, 20000, 500, "GMRS 22", "GMRS 22 IN", "RPT OUT, IN 467.725"},
};

/* By frequency: 462.550 (15), 462.5625 (1), ... 462.725 (22), then 467.5625 (8) ... 467.7125 (14) */
static const uint8_t scan_order[GMRS_CHANNELS] = {15, 1, 16, 2, 17, 3, 18, 4, 19, 5, 20, 6, 21, 7, 22,
                                                  8, 9, 10, 11, 12, 13, 14};

static const int16_t ctcss[GMRS_CTCSS_N] = {
    670,  719,  744,  770,  797,  825,  854,  885,  915,  948,  974,  1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
    1318, 1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799, 1862, 1928, 2035, 2107, 2181, 2257, 2336, 2418, 2503,
};

static const int16_t dcs[GMRS_DCS_N] = {
    23,  25,  26,  31,  32,  36,  43,  47,  51,  53,  54,  65,  71,  72,  73,  74,  114, 115, 116, 122, 125,
    131, 132, 134, 143, 145, 152, 155, 156, 162, 165, 172, 174, 205, 212, 223, 225, 226, 243, 244, 245, 246,
    251, 252, 255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325, 331, 332, 343, 346, 351, 356, 364, 365,
    371, 411, 412, 413, 423, 431, 432, 445, 446, 452, 454, 455, 462, 464, 465, 466, 503, 506, 516, 523, 526,
    532, 546, 565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754,
};

const gmrs_ch_t *gmrs_ch(int ch) {
    return ch >= 1 && ch <= GMRS_CHANNELS ? &chs[ch - 1] : NULL;
}

int gmrs_find(int64_t hz, int32_t tol, bool *input) {
    if (input) *input = false;
    for (int i = 0; i < GMRS_CHANNELS; i++) {
        if (llabs(hz - chs[i].hz) <= tol) return chs[i].ch;
        if (chs[i].in_hz && llabs(hz - chs[i].in_hz) <= tol) {
            if (input) *input = true;
            return chs[i].ch;
        }
    }
    return 0;
}

const char *gmrs_name(int64_t hz, int32_t tol) {
    bool in;
    int  ch = gmrs_find(hz, tol, &in);

    return ch ? (in ? chs[ch - 1].in_name : chs[ch - 1].name) : NULL;
}

int gmrs_scan_ch(int i) {
    return i >= 0 && i < GMRS_CHANNELS ? scan_order[i] : 0;
}

int gmrs_scan_next(int from) {
    return from < 0 ? 0 : (from + 1) % GMRS_CHANNELS;
}

int gmrs_row_ch(int r, bool *input) {
    if (input) *input = r >= GMRS_CHANNELS && r < GMRS_ROWS;
    if (r < 0 || r >= GMRS_ROWS) return 0;
    return r < GMRS_CHANNELS ? r + 1 : 15 + r - GMRS_CHANNELS;
}

int64_t gmrs_row_hz(int r) {
    bool             in;
    const gmrs_ch_t *c = gmrs_ch(gmrs_row_ch(r, &in));

    return c ? (in ? c->in_hz : c->hz) : 0;
}

const char *gmrs_num(int ch) {
    static const char *const num[GMRS_CHANNELS] = {"1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",  "9",  "10", "11",
                                                   "12", "13", "14", "15", "16", "17", "18", "19", "20", "21", "22"};

    return ch >= 1 && ch <= GMRS_CHANNELS ? num[ch - 1] : "";
}

void gmrs_tone_row(int r, char *t, size_t n) {
    size_t at = 0;

    if (!n) return;
    t[0] = 0;
    if (r == 0) {
        snprintf(t, n, "PRIVACY CODES: CTCSS HZ (22 = 141.3, HEARD WITH CHANNEL 20)");
    } else if (r >= 1 && r <= 10) {
        for (int k = (r - 1) * 4 + 1; k <= (r - 1) * 4 + 4 && k <= GMRS_CTCSS_N && at < n; k++)
            at += (size_t)snprintf(t + at, n - at, "%s%2d %3d.%d", k > (r - 1) * 4 + 1 ? "    " : "", k,
                                   gmrs_ctcss(k) / 10, gmrs_ctcss(k) % 10);
    } else if (r == 11) {
        snprintf(t, n, "DCS CODES (RADIOS NUMBER THEM FROM 39 UP, EACH ITS OWN WAY)");
    } else if (r >= 12 && r < GMRS_TONE_ROWS) {
        for (int k = (r - 12) * 12; k < (r - 11) * 12 && k < GMRS_DCS_N && at < n; k++)
            at += (size_t)snprintf(t + at, n - at, "%s%03d", k > (r - 12) * 12 ? " " : "", gmrs_dcs(k));
    }
}

int gmrs_ctcss(int n) {
    return n >= 1 && n <= GMRS_CTCSS_N ? ctcss[n - 1] : 0;
}

int gmrs_dcs(int i) {
    return i >= 0 && i < GMRS_DCS_N ? dcs[i] : 0;
}
