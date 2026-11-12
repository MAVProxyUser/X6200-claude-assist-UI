/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Wide Scan: the X6200 base's whole spectrum, 384 kHz around the dial whatever the zoom
 *  (radio_psd_wide(), dsp_psd_raw()). Its middle 360 kHz (the base's filter rolls off in the last 12 kHz
 *  each side) as a trace over a waterfall, the signals that stand out of the noise around them marked
 *  and named: AM broadcast channel, CB channel, FM station, time signal, shortwave broadcaster (with the
 *  EiBi list's station on air now), FT8, ham band.
 *
 *  The MFK goes from target to target, its press (or Tune) takes the radio there in the mode the target
 *  wants (AM on 10 kHz steps in the AM band, the CB channel in AM, WFM on FM...); a touch on the picture
 *  picks the nearest target. The main knob tunes as ever and the picture follows the dial. Auto is a
 *  sentry: every few seconds the strongest target not visited lately, within the 360 kHz seen when it
 *  was turned on, until the knob is turned.
 *
 *  The picture's bins: bin n/2 is the dial (the base's FFT shifted, its DC in the middle), checked on the
 *  night AM band where the stations sit on their 10 kHz channels.
 */

#include "app_wide.h"

#include "band_plan.h"
#include "broadcast_db.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/settings_manager_api.h"
#include "dsp.h"
#include "events.h"
#include "gui_styles.h"
#include "keyboard.h"
#include "main_screen.h"
#include "msg_box.h"
#include "radio.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const lv_font_t pixel_16, pixel_20, pixel_28;

#define VIEW_HZ     360000
#define IMG_W       768
#define IMG_X       14
#define HZ_PX       ((double)VIEW_HZ / IMG_W)
#define HEAD_H      30
#define SPEC_H      84
#define SCALE_H     18
#define FOOT_H      50
#define MAX_BINS    1024
#define FLOOR_HALF  16          /* bins each side for the local floor: +-12 kHz at 750 Hz */
#define TARGET_DB   7.0f        /* over the local floor */
#define TARGET_SEP  6           /* bins between two targets, the stronger kept: an AM carrier's sidebands */
#define MAX_TARGETS 24
#define LABELS      12
#define TICKS       9
#define AUTO_MS     8000
#define VISITED     6

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static void tune_cb(button_data_t *b);
static void auto_cb(button_data_t *b);
static void pre_cb(button_data_t *b);
static void avg_cb(button_data_t *b);
static const char *auto_label(void);
static const char *pre_label(void);
static const char *avg_label(void);

static button_data_t btn_tune = {.type = BTN_TEXT, .label = "Tune", .press = tune_cb};
static button_data_t btn_auto = {.type = BTN_TEXT_FN, .label_fn = auto_label, .press = auto_cb};
static button_data_t btn_pre  = {.type = BTN_TEXT_FN, .label_fn = pre_label, .press = pre_cb};
static button_data_t btn_avg  = {.type = BTN_TEXT_FN, .label_fn = avg_label, .press = avg_cb};

static buttons_page_t page = {{&btn_tune, &btn_auto, &btn_pre, &btn_avg}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .rotary_cb    = rotary_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_wide = &dialog;

typedef struct {
    int32_t hz;
    float   db, con;
} target_t;

typedef struct {
    char    label[12];      /* over the mark */
    char    name[72];       /* in the readout */
    int32_t tune_hz;
    int     mode;           /* radio_mode_t, -1: the radio's own */
} ident_t;

/* The picture */
static lv_img_dsc_t *frame;
static lv_obj_t     *img, *cursor_obj, *dial_obj;
static lv_obj_t     *head_l, *range_l, *stat_l, *big_l, *name_l;
static lv_obj_t     *tick_l[TICKS], *tgt_l[LABELS];
static lv_timer_t   *timer;
static int           img_h, wf_y0;
static bool          wide_ok;

/* The spectrum: power average, its dB, the noise around each bin */
static float    raw[MAX_BINS], avg_lin[MAX_BINS], row_lin[MAX_BINS], avg_db[MAX_BINS], floor_db[MAX_BINS];
static uint16_t nbins;
static uint32_t span_hz;
static double   bin_hz;
static int64_t  centre;         /* of the average, Hz; 0: none yet */
static double   wf_centre;      /* of the waterfall's columns */
static uint32_t last_seq;
static int      settle, row_frames, frames_in;
static float    floor_med, top_db = -60, bot_db = -130;
static bool     slow_avg, tx_now;

static target_t tg[MAX_TARGETS];
static int      ntg;

static int32_t  cursor_hz;
static bool     auto_on;
static int64_t  auto_anchor;
static uint64_t auto_next;
static int32_t  visited[VISITED];
static int      visited_i;

/* Frequencies */

static int64_t dial_centre(void) {
    return (int64_t)cparam_i_get(cfg_fg_freq) - param_i_get(cfg_band_if_shift) + cparam_i_get(cfg_mode_lo_offset);
}

static double px_of(int64_t hz) {
    return (hz - wf_centre) / HZ_PX + IMG_W / 2.0;
}

static int64_t hz_of_px(double x) {
    return (int64_t)llround(centre + (x - IMG_W / 2.0) * HZ_PX);
}

static int bin_of(int64_t hz) {
    return (int)lround((hz - centre) / bin_hz) + nbins / 2;
}

static uint32_t argb(theme_color_t c) {
    return theme_hex(c) | 0xff000000;
}

/* What a signal at hz is, where to tune for it and in what mode */

static const int32_t cb_khz[40] = {
    26965, 26975, 26985, 27005, 27015, 27025, 27035, 27055, 27065, 27075, 27085, 27105, 27115, 27125,
    27135, 27155, 27165, 27175, 27185, 27205, 27215, 27225, 27255, 27235, 27245, 27265, 27275, 27285,
    27295, 27305, 27315, 27325, 27335, 27345, 27355, 27365, 27375, 27385, 27395, 27405};

static const struct { int32_t lo, hi; bool r2_ham; const char *m; } sw_bc[] = {
    {2300, 2495, false, "120 m"}, {3200, 3400, false, "90 m"}, {3900, 4000, true, "75 m"},
    {4750, 5060, false, "60 m"}, {5900, 6200, false, "49 m"}, {7200, 7450, true, "41 m"},
    {9400, 9900, false, "31 m"}, {11600, 12100, false, "25 m"}, {13570, 13870, false, "22 m"},
    {15100, 15800, false, "19 m"}, {17480, 17900, false, "16 m"}, {18900, 19020, false, "15 m"},
    {21450, 21850, false, "13 m"}, {25670, 26100, false, "11 m"}};

static const struct { int32_t lo, hi; const char *name; } ham[] = {
    {1800, 2000, "160 m"}, {3500, 4000, "80 m"}, {5330, 5410, "60 m"}, {7000, 7300, "40 m"},
    {10100, 10150, "30 m"}, {14000, 14350, "20 m"}, {18068, 18168, "17 m"}, {21000, 21450, "15 m"},
    {24890, 24990, "12 m"}, {28000, 29700, "10 m"}, {50000, 54000, "6 m"}};

static const int32_t ft8_khz[] = {1840, 3573, 5357, 7074, 10136, 14074, 18100, 21074, 24915, 28074, 50313};

static const struct { int32_t hz; const char *name; } time_st[] = {
    {2500000, "WWV/WWVH 2.5 MHz"}, {5000000, "WWV/WWVH 5 MHz"}, {10000000, "WWV/WWVH 10 MHz"},
    {15000000, "WWV/WWVH 15 MHz"}, {20000000, "WWV 20 MHz"}, {25000000, "WWV 25 MHz"},
    {3330000, "CHU Canada 3.330"}, {7850000, "CHU Canada 7.850"}, {14670000, "CHU Canada 14.670"}};

/* The AM band's channels: 10 kHz apart in the Americas, 9 elsewhere. The band plan's region is not always the
 * radio's (a plan never chosen is Region 1's), so the carriers say: one within 1.5 kHz of one grid's channel and
 * not of the other's is a vote for its grid (the picture's bins are 750 Hz); a frequency both or neither fit
 * takes the grid with the votes, else the plan's */
static int mw_votes10, mw_votes9;

static int32_t mw_step(int64_t hz, bool r2) {
    int64_t e10 = llabs(hz - llround(hz / 10000.0) * 10000);
    int64_t e9  = llabs(hz - llround(hz / 9000.0) * 9000);

    if (mw_votes10 >= mw_votes9 + 3) return 10000;      /* the stations heard settled it */
    if (mw_votes9 >= mw_votes10 + 3) return 9000;
    if (e10 <= 1500 && e9 > 1500) return 10000;
    if (e9 <= 1500 && e10 > 1500) return 9000;
    if (mw_votes10 != mw_votes9) return mw_votes10 > mw_votes9 ? 10000 : 9000;
    return r2 ? 10000 : 9000;
}

static void mw_vote(int64_t hz) {
    if (hz < 525000 || hz > 1715000) return;
    int64_t e10 = llabs(hz - llround(hz / 10000.0) * 10000);
    int64_t e9  = llabs(hz - llround(hz / 9000.0) * 9000);

    if (e10 <= 1500 && e9 > 1500 && mw_votes10 < 1000) mw_votes10++;
    if (e9 <= 1500 && e10 > 1500 && mw_votes9 < 1000) mw_votes9++;
}

static void identify(int64_t hz, ident_t *id) {
    int32_t khz    = (int32_t)(hz / 1000);
    bool    r2     = band_plan_region() == 2;
    bool    wfm    = radio_caps() & HW_CAP_SAM_WFM;

    id->tune_hz = (int32_t)(llround(hz / 100.0) * 100);
    id->mode    = -1;
    snprintf(id->label, sizeof(id->label), "%d", khz);
    snprintf(id->name, sizeof(id->name), "UNKNOWN SIGNAL");

    for (size_t i = 0; i < sizeof(time_st) / sizeof(time_st[0]); i++) {
        if (llabs(hz - time_st[i].hz) <= 1500) {
            id->tune_hz = time_st[i].hz;
            id->mode    = radio_mode_am;
            snprintf(id->label, sizeof(id->label), "TIME");
            snprintf(id->name, sizeof(id->name), "TIME SIGNAL  %s", time_st[i].name);
            return;
        }
    }
    if (hz >= 516000 && hz <= 520000) {
        id->tune_hz = 518000;
        id->mode    = radio_mode_usb;
        snprintf(id->label, sizeof(id->label), "NAVTEX");
        snprintf(id->name, sizeof(id->name), "NAVTEX  518 KHZ (SHIP WEATHER)");
        return;
    }
    if (hz >= 525000 && hz <= 1715000) {
        int32_t step = mw_step(hz, r2);
        int32_t ch   = (int32_t)(llround((double)hz / step) * step);

        id->tune_hz = ch;
        id->mode    = radio_mode_am;
        snprintf(id->label, sizeof(id->label), "%d", ch / 1000);
        snprintf(id->name, sizeof(id->name), "AM BROADCAST  %d KHZ", ch / 1000);
        return;
    }
    if (hz >= 26960000 && hz <= 27410000) {
        int best = 0;

        for (int i = 1; i < 40; i++) {
            if (llabs(hz - cb_khz[i] * 1000LL) < llabs(hz - cb_khz[best] * 1000LL)) best = i;
        }
        if (llabs(hz - cb_khz[best] * 1000LL) <= 3000) {
            id->tune_hz = cb_khz[best] * 1000;
            id->mode    = radio_mode_am;
            snprintf(id->label, sizeof(id->label), "CH%d", best + 1);
            snprintf(id->name, sizeof(id->name), "CB CHANNEL %d  %s", best + 1,
                     best + 1 == 9 ? "(EMERGENCY)" : best + 1 == 19 ? "(HIGHWAY)" : "");
            return;
        }
    }
    if (hz >= 87500000 && hz <= 108000000) {
        int32_t ch = (int32_t)(llround(hz / 100000.0) * 100000);

        id->tune_hz = ch;
        id->mode    = wfm ? radio_mode_wfm : radio_mode_nfm;
        snprintf(id->label, sizeof(id->label), "%d.%d", ch / 1000000, (ch / 100000) % 10);
        snprintf(id->name, sizeof(id->name), "FM BROADCAST  %d.%d MHZ", ch / 1000000, (ch / 100000) % 10);
        return;
    }
    for (size_t i = 0; i < sizeof(ft8_khz) / sizeof(ft8_khz[0]); i++) {
        if (hz >= ft8_khz[i] * 1000LL && hz <= ft8_khz[i] * 1000LL + 3200) {
            id->tune_hz = ft8_khz[i] * 1000;
            id->mode    = radio_mode_usb_dig;
            snprintf(id->label, sizeof(id->label), "FT8");
            snprintf(id->name, sizeof(id->name), "FT8  %d.%03d MHZ (DIGITAL, OPEN THE FT8 APP)",
                     ft8_khz[i] / 1000, ft8_khz[i] % 1000);
            return;
        }
    }
    for (size_t i = 0; i < sizeof(sw_bc) / sizeof(sw_bc[0]); i++) {
        if (khz < sw_bc[i].lo || khz > sw_bc[i].hi) continue;
        /* the Americas: 75 m broadcasting and 7.2-7.3 MHz are the hams' phone bands */
        if (r2 && sw_bc[i].r2_ham && ((khz >= 3900 && khz < 4000) || (khz >= 7200 && khz < 7300))) continue;
        int32_t           ch = (int32_t)(llround(hz / 5000.0) * 5000);
        broadcast_match_t m[2];
        size_t            n = broadcast_db_find_matches(ch, 2500, m, 2);

        id->tune_hz = ch;
        id->mode    = radio_mode_am;
        snprintf(id->label, sizeof(id->label), "%d", ch / 1000);
        if (n > 0)
            snprintf(id->name, sizeof(id->name), "SHORTWAVE %d  %.40s", ch / 1000, m[0].station);
        else
            snprintf(id->name, sizeof(id->name), "SHORTWAVE BROADCAST  %s BAND  %d KHZ", sw_bc[i].m, ch / 1000);
        return;
    }
    for (size_t i = 0; i < sizeof(ham) / sizeof(ham[0]); i++) {
        if (khz >= ham[i].lo && khz <= ham[i].hi) {
            const char *how;

            if (band_plan_is_cw((int32_t)hz)) {
                id->mode = radio_mode_cw;
                how      = "CW";
            } else {
                id->mode = hz < 10000000 && !(khz >= 5330 && khz <= 5410) ? radio_mode_lsb : radio_mode_usb;
                how      = id->mode == radio_mode_lsb ? "VOICE, LSB" : "VOICE, USB";
            }
            snprintf(id->name, sizeof(id->name), "AMATEUR %s  %s", ham[i].name, how);
            return;
        }
    }
}

/* The noise around each bin: the median of the bins +-FLOOR_HALF */

static float median(float *v, int n) {
    int k = n / 2, lo = 0, hi = n - 1;

    while (lo < hi) {
        float p = v[k];
        int   i = lo, j = hi;

        while (i <= j) {
            while (v[i] < p) i++;
            while (v[j] > p) j--;
            if (i <= j) {
                float t = v[i];
                v[i++]  = v[j];
                v[j--]  = t;
            }
        }
        if (j < k) lo = i;
        if (k < i) hi = j;
    }
    return v[k];
}

/* FM broadcast (and anything 200 kHz wide) would be its own floor within +-12 kHz: there the floor is the
 * picture's, the targets 100 kHz apart and placed at their middle */
static bool broad;

static void find_floor(void) {
    float w[2 * FLOOR_HALF + 1], all[MAX_BINS];

    broad = centre > 87000000 && centre < 108500000;
    for (int i = 0; i < nbins && !broad; i++) {
        int lo = i - FLOOR_HALF < 0 ? 0 : i - FLOOR_HALF;
        int hi = i + FLOOR_HALF >= nbins ? nbins - 1 : i + FLOOR_HALF;
        int n  = hi - lo + 1;

        memcpy(w, avg_db + lo, n * sizeof(float));
        floor_db[i] = median(w, n);
    }
    int b0 = bin_of(centre - VIEW_HZ / 2), b1 = bin_of(centre + VIEW_HZ / 2), n = 0;

    for (int i = b0 < 0 ? 0 : b0; i <= b1 && i < nbins; i++) all[n++] = avg_db[i];
    if (n) floor_med = median(all, n);
    if (broad)
        for (int i = 0; i < nbins; i++) floor_db[i] = floor_med;
}

/* The bins around a peak within 6 dB of it (8 for a broad one): its width, and its middle by power */
static int peak_extent(int i, float drop, double *mid) {
    int    lo = i, hi = i;
    double s = 0, sw = 0;

    while (lo > 0 && avg_db[lo - 1] > avg_db[i] - drop) lo--;
    while (hi < nbins - 1 && avg_db[hi + 1] > avg_db[i] - drop) hi++;
    for (int k = lo; k <= hi; k++) {
        s += avg_lin[k] * k;
        sw += avg_lin[k];
    }
    *mid = sw > 0 ? s / sw : i;
    return hi - lo + 1;
}

static int cmp_con(const void *a, const void *b) {
    float d = ((const target_t *)b)->con - ((const target_t *)a)->con;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void find_targets(void) {
    target_t c[MAX_BINS / 2];
    int      nc = 0;
    int      b0 = bin_of(centre - VIEW_HZ / 2) + 1, b1 = bin_of(centre + VIEW_HZ / 2) - 1;

    if (b0 < 1) b0 = 1;
    if (b1 > nbins - 2) b1 = nbins - 2;
    for (int i = b0; i <= b1 && nc < MAX_BINS / 2; i++) {
        float con = avg_db[i] - floor_db[i];

        if (con < TARGET_DB || avg_db[i] < avg_db[i - 1] || avg_db[i] <= avg_db[i + 1]) continue;
        float  a = avg_db[i - 1], b = avg_db[i], d = avg_db[i + 1], den = a - 2 * b + d;
        double at = i + (den != 0 ? 0.5f * (a - d) / den : 0), mid;

        if (broad) {
            peak_extent(i, 8, &mid);
            at = mid;
        } else if (peak_extent(i, 6, &mid) > 16) {
            continue;                       /* wider than 12 kHz at 6 dB down: a hump of noise, not a station */
        }
        c[nc++] = (target_t){.hz = (int32_t)llround(centre + (at - nbins / 2) * bin_hz), .db = b, .con = con};
    }
    qsort(c, nc, sizeof(c[0]), cmp_con);
    ntg = 0;
    double sep = broad ? 100000 : TARGET_SEP * bin_hz;
    for (int i = 0; i < nc && ntg < MAX_TARGETS; i++) {
        bool near = false;

        for (int j = 0; j < ntg && !near; j++) near = llabs((int64_t)c[i].hz - tg[j].hz) < sep;
        if (!near) {
            tg[ntg++] = c[i];
            if (c[i].con >= 12) mw_vote(c[i].hz);
        }
    }
}

/* Drawing */

static void draw_spectrum(void) {
    uint32_t *px   = (uint32_t *)frame->data;
    uint32_t  bg   = argb(TC_bg), grid = argb(TC_line), fill = argb(TC_accent_soft), line = argb(TC_accent);
    uint32_t  mark = argb(TC_spot), tick = argb(TC_text_dim);
    float     top = -200, want_top, want_bot;
    int       prev_y = -1;

    for (int i = bin_of(centre - VIEW_HZ / 2); i <= bin_of(centre + VIEW_HZ / 2); i++)
        if (i >= 0 && i < nbins && avg_db[i] > top) top = avg_db[i];
    want_top = top + 4;
    want_bot = floor_med - 8;
    if (want_top < want_bot + 30) want_top = want_bot + 30;
    top_db += (want_top - top_db) * (want_top > top_db ? 0.5f : 0.08f);
    bot_db += (want_bot - bot_db) * 0.1f;

    for (int y = 0; y < SPEC_H + SCALE_H; y++) {
        uint32_t *r = px + y * IMG_W;
        for (int x = 0; x < IMG_W; x++) r[x] = bg;
    }
    /* every 10 dB */
    for (int d = (int)ceilf(bot_db / 10) * 10; d < top_db; d += 10) {
        int y = SPEC_H - 1 - (int)((d - bot_db) / (top_db - bot_db) * (SPEC_H - 1));
        if (y >= 0 && y < SPEC_H)
            for (int x = 0; x < IMG_W; x += 3) px[y * IMG_W + x] = grid;
    }
    for (int x = 0; x < IMG_W; x++) {
        int   b = bin_of(hz_of_px(x + 0.5));
        float v = b >= 0 && b < nbins ? avg_db[b] : bot_db;
        int   y = SPEC_H - 1 - (int)((v - bot_db) / (top_db - bot_db) * (SPEC_H - 1));

        if (y < 0) y = 0;
        if (y > SPEC_H - 1) y = SPEC_H - 1;
        for (int yy = y + 1; yy < SPEC_H; yy++) px[yy * IMG_W + x] = fill;
        int y0 = prev_y < 0 ? y : prev_y < y ? prev_y : y, y1 = prev_y < 0 ? y : prev_y > y ? prev_y : y;
        for (int yy = y0; yy <= y1; yy++) px[yy * IMG_W + x] = line;
        prev_y = y;
    }
    /* the targets: a tick over each */
    for (int i = 0; i < ntg; i++) {
        int x = (int)px_of(tg[i].hz), y = SPEC_H - 1 - (int)((tg[i].db - bot_db) / (top_db - bot_db) * (SPEC_H - 1));

        if (x < 1 || x >= IMG_W - 1) continue;
        for (int yy = y - 9; yy <= y - 3; yy++)
            if (yy >= 0) px[yy * IMG_W + x - 1] = px[yy * IMG_W + x] = px[yy * IMG_W + x + 1] = mark;
    }
    /* the scale: a tick every 50 kHz, a small one every 10 */
    int64_t f0 = (centre - VIEW_HZ / 2) / 10000 * 10000;
    for (int64_t f = f0; f <= centre + VIEW_HZ / 2; f += 10000) {
        int x = (int)lround(px_of(f));
        int h = f % 50000 == 0 ? 6 : 3;

        if (x < 0 || x >= IMG_W) continue;
        for (int yy = SPEC_H; yy < SPEC_H + h; yy++) px[yy * IMG_W + x] = tick;
    }
}

static uint32_t wf_px(float db) {
    int v = (int)((db - (floor_med - 4)) * 255 / 42);

    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (theme_shape.mono) return (v > 110 ? theme_shape.ink : theme_shape.paper) | 0xff000000;
    return wf_palette[v] | 0xff000000;
}

static void wf_clear(void) {
    uint32_t *px = (uint32_t *)frame->data;
    uint32_t  c  = wf_px(-999);

    for (int i = wf_y0 * IMG_W; i < img_h * IMG_W; i++) px[i] = c;
}

/* The waterfall's columns follow the dial: what it showed moves aside */
static void wf_shift(int dx) {
    uint32_t *px = (uint32_t *)frame->data;
    uint32_t  c  = wf_px(-999);

    if (abs(dx) >= IMG_W) {
        wf_clear();
        return;
    }
    for (int y = wf_y0; y < img_h; y++) {
        uint32_t *r = px + y * IMG_W;

        if (dx > 0) {
            memmove(r, r + dx, (IMG_W - dx) * sizeof(uint32_t));
            for (int x = IMG_W - dx; x < IMG_W; x++) r[x] = c;
        } else {
            memmove(r - dx, r, (IMG_W + dx) * sizeof(uint32_t));
            for (int x = 0; x < -dx; x++) r[x] = c;
        }
    }
}

static void wf_row(void) {
    uint32_t *px = (uint32_t *)frame->data;

    memmove(px + (wf_y0 + 1) * IMG_W, px + wf_y0 * IMG_W, (size_t)(img_h - wf_y0 - 1) * IMG_W * sizeof(uint32_t));
    for (int x = 0; x < IMG_W; x++) {
        int   b = bin_of(hz_of_px(x + 0.5));
        float v = b >= 0 && b < nbins ? 10 * log10f(row_lin[b] / row_frames + 1e-20f) : -999;

        px[wf_y0 * IMG_W + x] = wf_px(v);
    }
}

static void fmt_freq(char *s, size_t n, int64_t hz) {
    if (hz < 2000000)
        snprintf(s, n, "%lld", (long long)(hz / 1000));
    else
        snprintf(s, n, "%lld.%03lld", (long long)(hz / 1000000), (long long)((hz / 1000) % 1000));
}

static void show_labels(void) {
    int  placed[LABELS], np = 0;
    char s[24];

    /* the scale's figures every 50 kHz */
    int     t  = 0;
    int64_t f0 = ((centre - VIEW_HZ / 2) / 50000 + 1) * 50000;
    for (int64_t f = f0; f < centre + VIEW_HZ / 2 && t < TICKS; f += 50000) {
        int x = (int)lround(px_of(f));

        if (x < 24 || x > IMG_W - 24) continue;
        fmt_freq(s, sizeof(s), f);
        lv_label_set_text(tick_l[t], s);
        lv_obj_set_pos(tick_l[t], IMG_X + x - lv_txt_get_width(s, strlen(s), &pixel_16, 0, 0) / 2,
                       HEAD_H + SPEC_H + SCALE_H - 18 + 2);
        lv_obj_clear_flag(tick_l[t], LV_OBJ_FLAG_HIDDEN);
        t++;
    }
    for (; t < TICKS; t++) lv_obj_add_flag(tick_l[t], LV_OBJ_FLAG_HIDDEN);

    /* the strongest targets named, none over another */
    int l = 0;
    for (int i = 0; i < ntg && l < LABELS; i++) {
        int    x = (int)px_of(tg[i].hz);
        bool   clash = false;
        ident_t id;

        if (x < 16 || x > IMG_W - 16) continue;
        for (int j = 0; j < np && !clash; j++) clash = abs(placed[j] - x) < 46;
        if (clash) continue;
        identify(tg[i].hz, &id);
        lv_label_set_text(tgt_l[l], id.label);
        int w = lv_txt_get_width(id.label, strlen(id.label), &pixel_16, 0, 0);
        int y = SPEC_H - 1 - (int)((tg[i].db - bot_db) / (top_db - bot_db) * (SPEC_H - 1)) - 26;

        if (y < 0) y = 0;
        lv_obj_set_pos(tgt_l[l], IMG_X + x - w / 2, HEAD_H + y);
        lv_obj_clear_flag(tgt_l[l], LV_OBJ_FLAG_HIDDEN);
        placed[np++] = x;
        l++;
    }
    for (; l < LABELS; l++) lv_obj_add_flag(tgt_l[l], LV_OBJ_FLAG_HIDDEN);
}

static void show_cursor(void) {
    double  x = px_of(cursor_hz);
    ident_t id;
    char    f[24];
    int     b = bin_of(cursor_hz);
    float   con = -99;

    if (x < 0 || x >= IMG_W) {
        lv_obj_add_flag(cursor_obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(cursor_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(cursor_obj, IMG_X + (int)x - 1);
    }
    for (int i = b - 1; i <= b + 1; i++)
        if (i >= 0 && i < nbins && avg_db[i] - floor_db[i] > con) con = avg_db[i] - floor_db[i];
    identify(cursor_hz, &id);
    snprintf(f, sizeof(f), "%d.%03d.%d", id.tune_hz / 1000000, (id.tune_hz / 1000) % 1000, (id.tune_hz % 1000) / 100);
    lv_label_set_text_fmt(big_l, "> %s", f);
    if (con > 3)
        lv_label_set_text_fmt(name_l, "%s\nSIGNAL +%.0f DB OVER THE NOISE", id.name, con);
    else
        lv_label_set_text_fmt(name_l, "%s\nNOTHING THERE NOW", id.name);
}

static void show_status(void) {
    char a[24], b[24];

    fmt_freq(a, sizeof(a), centre - VIEW_HZ / 2);
    fmt_freq(b, sizeof(b), centre + VIEW_HZ / 2);
    lv_label_set_text_fmt(range_l, "%s - %s %s", a, b, centre + VIEW_HZ / 2 < 2000000 ? "KHZ" : "MHZ");
    if (tx_now)
        lv_label_set_text(stat_l, "TX: PAUSED");
    else
        lv_label_set_text_fmt(stat_l, "TARGETS %02d%s%s", ntg, cparam_i_get(cfg_cur_pre) ? "  PRE" : "",
                              auto_on ? "  AUTO" : "");
}

/* Tuning */

static void tune_to(int32_t hz, bool quiet) {
    ident_t id;

    identify(hz, &id);
    if (!cfg_is_valid_hw_freq(id.tune_hz)) {
        if (!quiet) msg_update_text_fmt("The radio can't tune %d.%03d MHz", id.tune_hz / 1000000, (id.tune_hz / 1000) % 1000);
        return;
    }
    if (main_screen_freq_locked()) {
        if (!quiet) msg_update_text_fmt("The dial is locked");
        auto_on = false;
        buttons_refresh(&btn_auto);
        return;
    }
    main_screen_set_freq(id.tune_hz);
    if (id.mode >= 0 && !main_screen_mode_locked()) cparam_i_set(cfg_cur_mode, id.mode);
    cursor_hz = id.tune_hz;
}

static void auto_step(void) {
    uint64_t now = lv_tick_get();

    if (!auto_on || now < auto_next || ntg == 0) return;
    auto_next = now + AUTO_MS;
    for (int i = 0; i < ntg; i++) {
        bool seen = false;

        if (llabs(tg[i].hz - auto_anchor) > VIEW_HZ / 2) continue;
        for (int j = 0; j < VISITED && !seen; j++) seen = visited[j] && labs(visited[j] - tg[i].hz) < 2500;
        if (seen) continue;
        visited[visited_i] = tg[i].hz;
        visited_i          = (visited_i + 1) % VISITED;
        tune_to(tg[i].hz, true);
        return;
    }
    memset(visited, 0, sizeof(visited));     /* all seen lately: round again */
}

static void move_cursor(int dir) {
    int     best = -1;
    int64_t from = cursor_hz;

    if (!centre) return;
    for (int i = 0; i < ntg; i++) {
        int64_t d = (int64_t)(tg[i].hz - from) * dir;
        if (d > bin_hz && (best < 0 || d < (int64_t)(tg[best].hz - from) * dir)) best = i;
    }
    cursor_hz = best >= 0 ? tg[best].hz : (int32_t)(from + dir * 5000);
    if (llabs(cursor_hz - centre) > VIEW_HZ / 2) cursor_hz = (int32_t)(centre + dir * VIEW_HZ / 2);
    show_cursor();
}

/* The base's packets */

static void timer_cb(lv_timer_t *t) {
    uint16_t n;
    uint32_t span, seq;
    bool     tx;

    (void)t;
    if (!dsp_psd_raw(raw, MAX_BINS, &n, &span, &tx, &seq) || seq == last_seq) return;
    last_seq = seq;
    tx_now   = tx;
    if (tx) {
        show_status();
        return;
    }
    if (span < 300000 || n < 64) {           /* not yet the wide one */
        lv_label_set_text(stat_l, "WAITING FOR THE BASE");
        return;
    }

    int64_t c = dial_centre();

    if (n != nbins || span != span_hz || !centre) {
        nbins   = n;
        span_hz = span;
        bin_hz  = (double)span / n;
        centre  = c;
        wf_centre = c;
        frames_in = row_frames = 0;
        cursor_hz = (int32_t)c;
        wf_clear();
        settle = 2;
        return;
    }
    if (c != centre) {
        int db = (int)lround((c - centre) / bin_hz);

        if (llabs(c - centre) < VIEW_HZ / 2 && db != 0) {
            if (db > 0) {
                memmove(avg_lin, avg_lin + db, (nbins - db) * sizeof(float));
                memmove(row_lin, row_lin + db, (nbins - db) * sizeof(float));
                for (int i = nbins - db; i < nbins; i++) avg_lin[i] = avg_lin[nbins - db - 1], row_lin[i] = 0;
            } else {
                memmove(avg_lin - db, avg_lin, (nbins + db) * sizeof(float));
                memmove(row_lin - db, row_lin, (nbins + db) * sizeof(float));
                for (int i = 0; i < -db; i++) avg_lin[i] = avg_lin[-db], row_lin[i] = 0;
            }
        } else if (llabs(c - centre) >= VIEW_HZ / 2) {
            frames_in = row_frames = 0;
        }
        centre = c;
        if (llabs(cursor_hz - c) > VIEW_HZ / 2) cursor_hz = (int32_t)c;     /* off the picture: to the dial */
        int dx = (int)lround((c - wf_centre) / HZ_PX);
        wf_shift(dx);
        wf_centre += dx * HZ_PX;
        settle = 2;                         /* the next packets may be of the old frequency */
        return;
    }
    if (settle > 0) {
        settle--;
        return;
    }

    float a = slow_avg ? 0.88f : 0.6f;
    for (int i = 0; i < nbins; i++) {
        float lin = powf(10.0f, raw[i] * 0.1f);

        avg_lin[i] = frames_in ? avg_lin[i] * a + lin * (1 - a) : lin;
        row_lin[i] = row_frames ? row_lin[i] + lin : lin;
        avg_db[i]  = 10 * log10f(avg_lin[i] + 1e-20f);
    }
    frames_in++;
    row_frames++;
    find_floor();
    find_targets();
    if (row_frames >= 2) {
        wf_row();
        row_frames = 0;
    }
    draw_spectrum();
    lv_obj_invalidate(img);
    show_labels();
    show_cursor();
    show_status();
    auto_step();
}

/* Controls */

static const char *auto_label(void) {
    return auto_on ? "Auto:\nOn" : "Auto:\nOff";
}

static const char *pre_label(void) {
    return cparam_i_get(cfg_cur_pre) ? "Pre:\nOn" : "Pre:\nOff";
}

static const char *avg_label(void) {
    return slow_avg ? "Average:\nSlow" : "Average:\nFast";
}

static void tune_cb(button_data_t *b) {
    (void)b;
    if (centre) tune_to(cursor_hz, false);
}

static void auto_cb(button_data_t *b) {
    (void)b;
    auto_on = !auto_on;
    if (auto_on) {
        auto_anchor = centre ? centre : dial_centre();
        auto_next   = 0;
        memset(visited, 0, sizeof(visited));
    }
    buttons_refresh(&btn_auto);
    show_status();
}

static void pre_cb(button_data_t *b) {
    (void)b;
    cparam_i_set(cfg_cur_pre, !cparam_i_get(cfg_cur_pre));
    buttons_refresh(&btn_pre);
}

static void avg_cb(button_data_t *b) {
    (void)b;
    slow_avg = !slow_avg;
    buttons_refresh(&btn_avg);
}

static void rotary_cb(int32_t diff) {
    (void)diff;
    if (auto_on) {                          /* the knob takes the radio back */
        auto_on = false;
        buttons_refresh(&btn_auto);
    }
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_LEFT:
            move_cursor(-1);
            break;
        case LV_KEY_RIGHT:
            move_cursor(1);
            break;
        case LV_KEY_ENTER:
            if (centre) tune_to(cursor_hz, false);
            break;
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

/* MFK press: the encoder sends a click, not ENTER */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER && centre) tune_to(cursor_hz, false);
}

/* A touch on the picture: the nearest target within 12 px, else the place itself */
static void touch_cb(lv_event_t *e) {
    lv_point_t p;
    lv_area_t  a;

    (void)e;
    if (!centre || !lv_indev_get_act()) return;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_obj_get_coords(img, &a);
    double  x    = p.x - a.x1;
    int64_t hz   = hz_of_px(x);
    int     best = -1;

    for (int i = 0; i < ntg; i++) {
        double d = fabs(px_of(tg[i].hz) - x);
        if (d <= 12 && (best < 0 || d < fabs(px_of(tg[best].hz) - x))) best = i;
    }
    cursor_hz = best >= 0 ? tg[best].hz : (int32_t)hz;
    show_cursor();
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y,
                      const char *s) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, s);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    buttons_load_page(&page);
    lv_obj_update_layout(dialog.obj);

    int h = lv_obj_get_height(dialog.obj);

    img_h = h - HEAD_H - FOOT_H;
    if (img_h > SPEC_H + SCALE_H + 200) img_h = SPEC_H + SCALE_H + 200;
    wf_y0 = SPEC_H + SCALE_H;

    head_l  = text(dialog.obj, &pixel_20, TC(text_head), IMG_X, 5, "WIDE SCAN // 384 KHZ");
    range_l = text(dialog.obj, &pixel_20, TC(text_value), 240, 5, "");
    stat_l  = text(dialog.obj, &pixel_20, TC(accent), 520, 5, "STARTING");

    frame = lv_img_buf_alloc(IMG_W, img_h, LV_IMG_CF_TRUE_COLOR);
    img   = lv_img_create(dialog.obj);
    lv_img_set_src(img, frame);
    lv_obj_set_pos(img, IMG_X, HEAD_H);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(img, touch_cb, LV_EVENT_PRESSING, NULL);
    {
        uint32_t *px = (uint32_t *)frame->data;
        uint32_t  bg = argb(TC_bg);
        for (int i = 0; i < wf_y0 * IMG_W; i++) px[i] = bg;
    }
    nbins  = 0;
    centre = 0;
    wf_clear();

    dial_obj = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(dial_obj);
    lv_obj_set_size(dial_obj, 1, img_h);
    lv_obj_set_pos(dial_obj, IMG_X + IMG_W / 2, HEAD_H);
    lv_obj_set_style_bg_color(dial_obj, TC(vfo_line), 0);
    lv_obj_set_style_bg_opa(dial_obj, LV_OPA_50, 0);
    lv_obj_clear_flag(dial_obj, LV_OBJ_FLAG_CLICKABLE);

    cursor_obj = lv_obj_create(dialog.obj);
    lv_obj_remove_style_all(cursor_obj);
    lv_obj_set_size(cursor_obj, 2, img_h);
    lv_obj_set_pos(cursor_obj, IMG_X + IMG_W / 2 - 1, HEAD_H);
    lv_obj_set_style_bg_color(cursor_obj, TC(finder_cursor), 0);
    lv_obj_set_style_bg_opa(cursor_obj, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cursor_obj, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < TICKS; i++) {
        tick_l[i] = text(dialog.obj, &pixel_16, TC(text_dim), 0, 0, "");
        lv_obj_add_flag(tick_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < LABELS; i++) {
        tgt_l[i] = text(dialog.obj, &pixel_16, TC(spot), 0, 0, "");
        lv_obj_add_flag(tgt_l[i], LV_OBJ_FLAG_HIDDEN);
    }

    int fy = HEAD_H + img_h + 4;
    big_l  = text(dialog.obj, &pixel_28, TC(text_value), IMG_X, fy, "> ---");
    name_l = text(dialog.obj, &pixel_16, TC(text), 250, fy, "");

    auto_on = false;
    wide_ok = radio_psd_wide(true);
    if (!wide_ok) {
        lv_label_set_text(stat_l, "NO WIDE SPECTRUM");
        lv_label_set_text(name_l, "THIS RADIO'S BASE SENDS NO SPECTRUM OF ITS OWN:\nWIDE SCAN NEEDS AN X6200");
    } else {
        lv_label_set_text(name_l, "MFK: TARGET TO TARGET   PRESS: TUNE\nTOUCH: PICK   KNOB: TUNE AS EVER");
        timer = lv_timer_create(timer_cb, 60, NULL);
    }

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static void destruct_cb(void) {
    if (timer) {
        lv_timer_del(timer);
        timer = NULL;
    }
    if (wide_ok) radio_psd_wide(false);
    wide_ok = false;
    auto_on = false;
    lv_group_remove_obj(dialog.obj);
    if (frame) {                            /* the image goes with the window, undrawn */
        lv_img_cache_invalidate_src(frame);
        lv_img_buf_free(frame);
        frame = NULL;
    }
    img = cursor_obj = dial_obj = head_l = range_l = stat_l = big_l = name_l = NULL;
    memset(tick_l, 0, sizeof(tick_l));
    memset(tgt_l, 0, sizeof(tgt_l));
}
