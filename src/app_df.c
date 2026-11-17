/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Page I, the Sentry's direction finder (docs/plans/kraken_df_plan.md, section 3): on the MFK's ring
 *  after H (AIRSPACE) while a KrakenSDR is on USB. The daemon x6x00-df (tools/rtlspec/df.c, through df_rx.c) streams
 *  the Kraken's five receivers, calibrates them with its noise source and works out where a signal comes from; this
 *  screen draws it round the array as it is set out (KrakenRF's Antenna Array Size Calculator's numbering: ANT 0
 *  forward, the rest clockwise).
 *
 *  - The compass (left): the array to scale in the middle, each antenna in the colour of what it is doing (green in
 *    the bearing, amber quiet, red left out or slipped, magenta clipping, dim not calibrated or off, a ring alone when
 *    it is not in the bearing) with a bar of the signal on it; a dashed circle half a wavelength across, so the array
 *    is seen against the band; round it the method's spectrum, the bearing's needle and its +- wedge in the colour
 *    of its fit, the last bearings as dots on the rim, a line's other side dashed.
 *  - The right: the target and the bearing; the array against the band (the calculator's s, its resolution and the
 *    word OK / AMBIGUOUS / COARSE, and the size it would want); each antenna's signal and state; CH0's spectrum with
 *    the target marked over a waterfall of bearings.
 *  - Keys: F1 TUNE (FOLLOW A or B, the band, the frequency, the bandwidth, AUTO; held: AUTO, the strongest signal in
 *    the passband), F2 ARRAY (band, kind, size, heading), F3 METHOD, F4 CAL (now; held: how often), F5 SET (rate,
 *    gain, squelch, antennas left out, the test source). VFO: the target finely, the Kraken following past its
 *    passband's edge; MFK: a channel; the BAND keys: the band presets; the ring: H before, A after.
 *  - FOLLOW (the owner, 2026-11-17: no easy way to tune to the band to DF, nor to follow a radio's tune from another
 *    screen): A takes the X6200's dial, B the frequency B is tuned to, as they change, once they rest half a second
 *    (each retune is a calibration). The VFO, the band keys or a band chosen set it OFF.
 *  Receive only: the Kraken has no transmitter, and the X6200 is not touched (the VFO is this screen's).
 */

#include "app_df.h"

#include "app_sentry.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "df_rx.h"
#include "gui_events.h"
#include "gui_state.h"
#include "keyboard.h"
#include "main_screen.h"
#include "rtl_spectrum.h"
#include "scheduler.h"
#include "sdr_pool.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern const lv_font_t pixel_16, pixel_20, pixel_28;

#define SW        800
#define HEAD_H    44
#define BODY_Y    50
#define KEYS_H    46
#define LEGEND_H  20
#define SC        348           /* the compass, square */
#define SC_X      9
#define SC_Y      (BODY_Y + 3)
#define RX0       366           /* the right-hand column */
#define RW        (SW - 6 - RX0)
#define NANT      5             /* rows in the antennas' box (a Kraken's five) */
#define BAR_W     120
#define ROW_H     16
#define SPEC_W    (RW - 12)
#define SPEC_H    22
#define WF_H      30
#define WF_STEP   3             /* estimates a waterfall row */
#define TRAIL     60            /* bearings kept as dots on the rim */
#define PANEL_W   600
#define PANEL_ROWS 11

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static bool front_key(int key, int state);
static bool rotary_takes(void) { return true; }    /* the VFO is this screen's: the X6200 is not tuned by it */
static void fkey(int k);
static void f1_cb(button_data_t *b) { (void)b; fkey(0); }
static void f2_cb(button_data_t *b) { (void)b; fkey(1); }
static void f3_cb(button_data_t *b) { (void)b; fkey(2); }
static void f4_cb(button_data_t *b) { (void)b; fkey(3); }
static void f5_cb(button_data_t *b) { (void)b; fkey(4); }
static const char *f_label(int i);
static const char *f1_label(void) { return f_label(0); }
static const char *f2_label(void) { return f_label(1); }
static const char *f3_label(void) { return f_label(2); }
static const char *f4_label(void) { return f_label(3); }
static const char *f5_label(void) { return f_label(4); }

static button_data_t btn[5] = {
    {.type = BTN_TEXT_FN, .label_fn = f1_label, .press = f1_cb},
    {.type = BTN_TEXT_FN, .label_fn = f2_label, .press = f2_cb},
    {.type = BTN_TEXT_FN, .label_fn = f3_label, .press = f3_cb},
    {.type = BTN_TEXT_FN, .label_fn = f4_label, .press = f4_cb},
    {.type = BTN_TEXT_FN, .label_fn = f5_label, .press = f5_cb},
};
static buttons_page_t page = {{&btn[0], &btn[1], &btn[2], &btn[3], &btn[4]}};

static dialog_t dialog = {
    .run             = false,
    .construct_cb    = construct_cb,
    .destruct_cb     = destruct_cb,
    .rotary_cb       = rotary_cb,
    .btn_page        = &page,
    .key_cb          = key_cb,
    .front_key_cb    = front_key,
    .rotary_takes_cb = rotary_takes,
};

dialog_t *dialog_df = &dialog;

/* ---- The settings (kept with gui_state: "df.*") ---- */

/* Bands the array is set out for: the target's frequency, its bandwidth, a channel's step (the MFK) */
typedef struct {
    const char *name;
    uint32_t    hz, bw, step;
} band_t;

static const band_t bands[] = {
    {"AIR", 125000000, 6000, 25000},       /* airband AM */
    {"2 M", 146000000, 12500, 12500},
    {"MARINE", 156800000, 12500, 25000},   /* channel 16 */
    {"NOAA", 162550000, 12500, 25000},
    {"1.25 M", 223500000, 12500, 20000},
    {"ISM 433", 433920000, 25000, 25000},
    {"70 CM", 440000000, 12500, 12500},
    {"FRS/GMRS", 462562500, 12500, 25000},
    {"33 CM", 915000000, 25000, 25000},
    {"23 CM", 1270000000, 12500, 25000},
};
#define N_BANDS (int)(sizeof(bands) / sizeof(bands[0]))

typedef struct {
    const char *name;
    bool        ula;
    int         n;
} kind_t;

static const kind_t kinds[] = {{"UCA-5", false, 5}, {"UCA-4", false, 4}, {"UCA-3", false, 3},
                               {"ULA-5", true, 5},  {"ULA-4", true, 4},  {"ULA-3", true, 3},
                               {"CUSTOM", false, 0}};
#define N_KINDS (int)(sizeof(kinds) / sizeof(kinds[0]))
#define K_CUSTOM (N_KINDS - 1)

/* CUSTOM: any layout of up to five antennas, x forward and y left in cm, ANT 0 first, one a line in /mnt/df_array.txt
 * (the web page's Array editor writes it; # starts a comment). Read when CUSTOM is chosen and at each start */
#define ARRAY_FILE "/mnt/df_array.txt"
static double cust_x[DF_PROTO_MAXCH], cust_y[DF_PROTO_MAXCH];     /* metres */
static int    cust_n;

static int cust_load(void) {
    FILE *f = fopen(ARRAY_FILE, "r");
    char  l[128];
    int   n = 0;

    if (!f) return cust_n = 0;
    while (n < NANT && fgets(l, sizeof(l), f)) {
        double x, y;
        char  *h = strchr(l, '#');
        if (h) *h = 0;
        if (sscanf(l, "%lf %lf", &x, &y) == 2 && fabs(x) < 2000 && fabs(y) < 2000) cust_x[n] = x / 100, cust_y[n] = y / 100, n++;
    }
    fclose(f);
    return cust_n = n >= 2 ? n : 0;
}


static const char *const methods[] = {"MUSIC", "BARTLETT", "CAPON", "MEM", "TNA", "ROOT-MUSIC"};
#define N_METHODS (int)(sizeof(methods) / sizeof(methods[0]))
static const uint32_t    rates[] = {250000, 1024000, 2048000, 2400000};
#define N_RATES (int)(sizeof(rates) / sizeof(rates[0]))
static const uint32_t bws[] = {2500, 6000, 12500, 25000, 200000};
#define N_BWS (int)(sizeof(bws) / sizeof(bws[0]))
static const int cal_every[] = {0, 30, 60, 120, 300, 600};
#define N_CALS (int)(sizeof(cal_every) / sizeof(cal_every[0]))
/* the R820T's gains, tenths of a dB */
static const int gains[] = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                            280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
#define N_GAINS (int)(sizeof(gains) / sizeof(gains[0]))

static struct {
    int32_t fc, off;            /* the Kraken's centre, the target's offset from it (Hz) */
    int     bw, band, kind, size_mm, heading, method, rate, gain, sql_ddb, cal, off_mask;
    bool    autot;              /* the target: the strongest signal in the passband */
    int     follow;             /* FOLLOW: 0 off, 1 A (the X6200's dial), 2 B (B's frequency) */
    int     nsrc;               /* the sources to look for: 1-3, 0 AUTO (x6x00-df's k) */
    bool    fb;                 /* forward-backward averaging (a ULA) */
    int     smooth;             /* spatial smoothing's subarrays, 0 off, 2 or 3 (a ULA; the web page sets it) */
    bool    log;                /* the bearing log, /mnt/df_log (x6x00-df's l) */
    uint32_t tg_hz[DF_PROTO_TARGETS];   /* the other targets (x6x00-df's 1-3), Hz, 0 none */
    int     tg_bw[DF_PROTO_TARGETS];    /* their widths (bws[]) */
} cfg;

/* the elements the chosen layout has */
static int kind_n(void) { return cfg.kind == K_CUSTOM ? cust_n : kinds[cfg.kind].n; }

static void cfg_load(void) {
    cfg.band    = gui_state_get("df.band", 6);
    if (cfg.band < -1 || cfg.band >= N_BANDS) cfg.band = 6;
    cfg.rate    = gui_state_get("df.rate", 1);
    if (cfg.rate < 0 || cfg.rate >= N_RATES) cfg.rate = 1;
    cfg.fc      = gui_state_get("df.fc", (int32_t)(440000000 - rates[cfg.rate] / 4));
    cfg.off     = gui_state_get("df.off", (int32_t)(rates[cfg.rate] / 4));
    cfg.bw      = gui_state_get("df.bw", 2);
    if (cfg.bw < 0 || cfg.bw >= N_BWS) cfg.bw = 2;
    cfg.kind    = gui_state_get("df.kind", 0);
    if (cfg.kind < 0 || cfg.kind >= N_KINDS) cfg.kind = 0;
    if (cfg.kind == K_CUSTOM && !cust_load()) cfg.kind = 0;      /* the file gone: the circle */
    cfg.size_mm = gui_state_get("df.size", 290);
    if (cfg.size_mm < 10 || cfg.size_mm > 5000) cfg.size_mm = 290;
    cfg.heading = gui_state_get("df.head", 0) % 360;
    cfg.method  = gui_state_get("df.meth", 0);
    if (cfg.method < 0 || cfg.method >= N_METHODS) cfg.method = 0;
    cfg.gain    = gui_state_get("df.gain", 15);
    if (cfg.gain < 0 || cfg.gain >= N_GAINS) cfg.gain = 15;
    cfg.sql_ddb = gui_state_get("df.sql", 80);
    cfg.cal     = gui_state_get("df.cal", 3);
    if (cfg.cal < 0 || cfg.cal >= N_CALS) cfg.cal = 3;
    cfg.off_mask = gui_state_get("df.mask", 0) & 0xff;
    cfg.autot   = gui_state_get("df.auto", 0) != 0;
    cfg.follow  = gui_state_get("df.follow", 0);
    if (cfg.follow < 0 || cfg.follow > 2) cfg.follow = 0;
    cfg.nsrc    = gui_state_get("df.nsrc", 1);
    if (cfg.nsrc < 0 || cfg.nsrc > 3) cfg.nsrc = 1;
    cfg.fb      = gui_state_get("df.fb", 0) != 0;
    cfg.smooth  = gui_state_get("df.smooth", 0);
    if (cfg.smooth != 2 && cfg.smooth != 3) cfg.smooth = 0;
    cfg.log     = gui_state_get("df.log", 0) != 0;
    for (int i = 0; i < DF_PROTO_TARGETS; i++) {
        char k[16];

        snprintf(k, sizeof(k), "df.tg%d", i + 2);
        int32_t hz = gui_state_get(k, 0);

        cfg.tg_hz[i] = hz >= 24000000 && hz <= 1766000000 ? (uint32_t)hz : 0;
        snprintf(k, sizeof(k), "df.tg%dbw", i + 2);
        cfg.tg_bw[i] = gui_state_get(k, 2);
        if (cfg.tg_bw[i] < 0 || cfg.tg_bw[i] >= N_BWS) cfg.tg_bw[i] = 2;
    }
}

static void cfg_save(void) {
    gui_state_set("df.band", cfg.band);
    gui_state_set("df.rate", cfg.rate);
    gui_state_set("df.fc", cfg.fc);
    gui_state_set("df.off", cfg.off);
    gui_state_set("df.bw", cfg.bw);
    gui_state_set("df.kind", cfg.kind);
    gui_state_set("df.size", cfg.size_mm);
    gui_state_set("df.head", cfg.heading);
    gui_state_set("df.meth", cfg.method);
    gui_state_set("df.gain", cfg.gain);
    gui_state_set("df.sql", cfg.sql_ddb);
    gui_state_set("df.cal", cfg.cal);
    gui_state_set("df.mask", cfg.off_mask);
    gui_state_set("df.auto", cfg.autot);
    gui_state_set("df.follow", cfg.follow);
    gui_state_set("df.nsrc", cfg.nsrc);
    gui_state_set("df.fb", cfg.fb);
    gui_state_set("df.smooth", cfg.smooth);
    gui_state_set("df.log", cfg.log);
    for (int i = 0; i < DF_PROTO_TARGETS; i++) {
        char k[16];

        snprintf(k, sizeof(k), "df.tg%d", i + 2);
        gui_state_set(k, (int32_t)cfg.tg_hz[i]);
        snprintf(k, sizeof(k), "df.tg%dbw", i + 2);
        gui_state_set(k, cfg.tg_bw[i]);
    }
}

static uint32_t target_hz(void) { return (uint32_t)(cfg.fc + cfg.off); }

/* The newest of what the daemon said (df_rx_snap, each refresh) */
/* The heading the bearings take (degrees, ANT 0's compass bearing): gpsd's when x6x00-df has one (a compass behind
 * gpsd, or the course while moving: DFGP), else the one set by hand (F2 ARRAY's HEADING); heading_from() says which */
static df_snap_t sn;
static int heading_now(void) {
    if (sn.connected && sn.have_gp && sn.gp.head_src && sn.gp.head_ddeg >= 0) return (sn.gp.head_ddeg + 5) / 10 % 360;
    return cfg.heading;
}

static const char *heading_from(void) {
    if (sn.connected && sn.have_gp && sn.gp.head_src == 2) return "COMPASS";
    if (sn.connected && sn.have_gp && sn.gp.head_src == 1) return "GPS COURSE";
    return cfg.heading ? "SET" : "";
}

/* What each setting means to the daemon (df_proto.h): kept by df_rx.c and said again to a daemon started anew */
static uint32_t mono_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void send_tgs(void);
static void send_rate(void)   { df_rx_cmd("s %u", rates[cfg.rate]); send_tgs(); }
/* ---- The other targets (x6x00-df's 1-3, page I's TARGET 2-4): each kept by its frequency, so a retune leaves it
 * where it was; one the passband has left is off till the Kraken is back round it ---- */
#define N_TG DF_PROTO_TARGETS
static const uint32_t tg_col[N_TG] = {0x4fc3f7, 0xe066d6, 0xd8d8d8};

/* its offset from the Kraken's centre; false outside the passband (the main target's margins) */
static bool tg_off(int i, int32_t *off) {
    int64_t o   = (int64_t)cfg.tg_hz[i] - cfg.fc;
    int32_t lim = (int32_t)(rates[cfg.rate] / 2) - (int32_t)bws[cfg.tg_bw[i]] / 2 - (int32_t)(rates[cfg.rate] / 20);

    if (!cfg.tg_hz[i] || llabs(o) > lim) return false;
    *off = (int32_t)o;
    return true;
}

static void send_tg(int i) {
    int32_t off;

    if (tg_off(i, &off)) df_rx_cmd("%d %d %u", i + 1, off, bws[cfg.tg_bw[i]]);
    else df_rx_cmd("%d 0 0", i + 1);
}
static void send_tgs(void) {
    for (int i = 0; i < N_TG; i++) send_tg(i);
}

/* the right-hand column's few characters: the bearing and fit, H holding, SLOW its core short, OUT */
static void tg_short(int i, char *s, size_t n);

/* the daemon's newest for target i in snapshot x: NULL while it has none (set, inside the passband, DFTG under 1.5 s
 * old) */
static const df_tg_t *tg_now_in(const df_snap_t *x, int i) {
    if (!cfg.tg_hz[i] || !x->tg_ms || mono_ms() - x->tg_ms > 1500 || !x->tg.tg[i].on) return NULL;
    return &x->tg.tg[i];
}
static const df_tg_t *tg_now(int i) { return tg_now_in(&sn, i); }

/* its bearing to show (tenths, by the heading when there is one), -1 none; hold: the held one */
static int tg_brg(int i, bool *hold) {
    const df_tg_t *t = tg_now(i);

    *hold = false;
    if (!t || t->bearing_ddeg < 0 || (!t->gate && t->age_ds >= 300)) return -1;
    *hold = !t->gate;
    return *heading_from() ? (t->bearing_ddeg + heading_now() * 10) % 3600 : t->bearing_ddeg;
}

static void tg_short(int i, char *s, size_t n) {
    int32_t        off;
    bool           hold;
    int            b = tg_brg(i, &hold);
    const df_tg_t *t = tg_now(i);

    if (!tg_off(i, &off)) snprintf(s, n, "OUT");
    else if (b < 0) snprintf(s, n, "---%s", t && t->done_pct < 90 ? " SLOW" : "");
    else snprintf(s, n, "%03d.%d %d%%%s", b / 10, b % 10, t->fit_pct, hold ? "H" : "");
}

static void tg_text(int i, char *s, size_t n) {
    int32_t        off;
    bool           hold;
    int            b = tg_brg(i, &hold);
    const df_tg_t *t = tg_now(i);

    if (!tg_off(i, &off)) snprintf(s, n, "OUT OF THE PASSBAND");
    else if (b < 0) snprintf(s, n, "BRG ---%s", t && t->done_pct < 90 ? "  CPU SHORT" : "");
    else snprintf(s, n, "BRG %03d.%d%s FIT %d%%%s", b / 10, b % 10, *heading_from() ? " N" : "", t->fit_pct, hold ? " HOLD" : "");
}

static void send_gain(void)   { df_rx_cmd("g %d", gains[cfg.gain]); }
/* when page I last tuned the daemon itself: an IQ program's tuning is not taken over it till x6x00-df has said where
 * it is since (iq_adopt), or a DFST from before page I's own tuning would take it back */
static uint32_t own_tune_ms;
static void send_fc(void)     { own_tune_ms = lv_tick_get(); df_rx_cmd("f %d", cfg.fc); send_tgs(); }
static void send_target(void) { own_tune_ms = lv_tick_get(); df_rx_cmd("t %d %u", cfg.off, bws[cfg.bw]); }
static void send_array(void) {
    if (cfg.kind == K_CUSTOM && cust_n >= 2) {
        char a[200];
        int  l = snprintf(a, sizeof(a), "a custom %d", cust_n);
        for (int m = 0; m < cust_n; m++) l += snprintf(a + l, sizeof(a) - (size_t)l, " %ld %ld", lround(cust_x[m] * 1000), lround(cust_y[m] * 1000));
        df_rx_cmd("%s", a);
        return;
    }
    df_rx_cmd("a %s %d %d", kinds[cfg.kind].ula ? "ula" : "uca", kinds[cfg.kind].n, cfg.size_mm);
}
static void send_method(void) { df_rx_cmd("m %d", cfg.method); }
static void send_sql(void)    { df_rx_cmd("q %d", cfg.sql_ddb); }
static void send_cal(void)    { df_rx_cmd("i %d", cal_every[cfg.cal]); }
static void send_mask(void)   { df_rx_cmd("x %d", cfg.off_mask); }
static void send_nsrc(void)   { df_rx_cmd("k %d", cfg.nsrc); }
static void send_fb(void)     { df_rx_cmd("w %d", cfg.fb); }
static void send_smooth(void) { df_rx_cmd("o %d", cfg.smooth); }
static void send_log(void)    { df_rx_cmd("l %d", cfg.log); df_rx_cmd("h %d", cfg.heading); }
static void send_all(void) {
    send_rate();
    send_gain();
    send_fc();
    send_target();
    send_array();
    send_method();
    send_sql();
    send_cal();
    send_mask();
    send_nsrc();
    send_fb();
    send_smooth();
    send_log();
}

/* The target kept inside the passband, clear of its edges (the filters' skirts) */
static void clamp_target(void) {
    int32_t lim = (int32_t)(rates[cfg.rate] / 2) - (int32_t)bws[cfg.bw] / 2 - (int32_t)(rates[cfg.rate] / 20);

    if (lim < 0) lim = 0;
    if (cfg.off > lim) cfg.off = lim;
    if (cfg.off < -lim) cfg.off = -lim;
}

/* A band chosen: the Kraken a quarter of the rate under its frequency, so the target sits clear of the sticks' DC */
static void band_set(int b) {
    cfg.band = b;
    cfg.off  = (int32_t)(rates[cfg.rate] / 4);
    cfg.fc   = (int32_t)bands[b].hz - cfg.off;
    for (int i = 0; i < N_BWS; i++)
        if (bws[i] == bands[b].bw) cfg.bw = i;
    send_fc();
    send_target();
}

/* The band the target is in now (-1 none): what the calculator's line names */
static int band_of_target(void) {
    uint32_t f = target_hz();
    int      best = -1;
    double   bd   = 1e18;

    for (int i = 0; i < N_BANDS; i++) {
        double d = fabs((double)f - bands[i].hz);
        if (d < bd) bd = d, best = i;
    }
    return bd <= 0.03 * bands[best].hz ? best : -1;
}

/* ---- Who has the Kraken for DF: page I and the web page (cfg_socket df_run); the daemon runs while either does ---- */

enum { DF_USER_PAGE = 1, DF_USER_WEB = 2 };
static int  df_users;
static bool cfg_loaded;

static void send_all(void);
static void clamp_target(void);

static void df_use(int who, bool on) {
    int before = df_users;

    df_users = on ? df_users | who : df_users & ~who;
    if (!before && df_users) {
        if (!cfg_loaded) cfg_load(), cfg_loaded = true;
        df_rx_open();
        clamp_target();
        send_all();
    } else if (before && !df_users) {
        cfg_save();
        df_rx_close();
    }
}

/* ---- Tuning: a frequency anywhere the Kraken goes, and FOLLOW ---- */

#define KRAKEN_LO   24000000LL
#define KRAKEN_HI   1766000000LL

static void msg(const char *t);
static void show_keys(void);

/* The target to hz: inside the passband as it is when hz is clear of its edges and of the sticks' DC in the middle,
 * else the Kraken moved (a quarter of the rate under hz, as a band chosen), which means a calibration. false: out of
 * the Kraken's range */
static bool tune_to(int64_t hz) {
    int32_t lim = (int32_t)(rates[cfg.rate] / 2) - (int32_t)bws[cfg.bw] / 2 - (int32_t)(rates[cfg.rate] / 20);
    int64_t off = hz - cfg.fc;

    if (hz < KRAKEN_LO || hz > KRAKEN_HI) return false;
    if (llabs(off) <= lim && llabs(off) >= (int64_t)bws[cfg.bw]) {
        cfg.off = (int32_t)off;
    } else {
        cfg.off = (int32_t)(rates[cfg.rate] / 4);
        cfg.fc  = (int32_t)(hz - cfg.off);
        send_fc();
    }
    cfg.band = band_of_target();
    send_target();
    return true;
}

/* another target set to hz (0: cleared), its width the main one's when new; false outside the Kraken's range */
static bool tg_set_hz(int i, int64_t hz) {
    if (hz && (hz < KRAKEN_LO || hz > KRAKEN_HI)) return false;
    if (hz && !cfg.tg_hz[i]) cfg.tg_bw[i] = cfg.bw;
    cfg.tg_hz[i] = (uint32_t)hz;
    send_tg(i);
    return true;
}

static const char *const follow_names[] = {"OFF", "A", "B"};

static int64_t follow_hz(void) {
    return cfg.follow == 1 ? (int64_t)cparam_i_get(cfg_fg_freq) : cfg.follow == 2 ? rtl_spectrum_listen_hz() : 0;
}

/* FOLLOW off by the user's own tuning, said once */
static void follow_off(void) {
    if (!cfg.follow) return;
    cfg.follow = 0;
    msg("FOLLOW OFF: TUNED HERE");
}

/* Each 200 ms: the followed radio's frequency taken once it has rested half a second (each retune calibrates) */
static void follow_tick(void) {
    static int64_t  seen, took;
    static uint32_t since;
    static int      said;
    int64_t         hz = follow_hz();
    uint32_t        now = lv_tick_get();

    if (!cfg.follow || hz <= 0) {
        seen = took = 0;
        said = 0;
        return;
    }
    if (hz != seen) {
        seen  = hz;
        since = now;
        return;
    }
    if (hz == took || now - since < 500) return;
    took = hz;
    if (hz == (int64_t)target_hz()) return;
    if (tune_to(hz)) {
        said = 0;
        cfg_save();
        show_keys();
    } else if (said != cfg.follow) {
        char t[96];

        snprintf(t, sizeof(t), "FOLLOW %s: %.3f MHZ IS OUTSIDE THE KRAKEN'S 24-1766 MHZ: WAITING", follow_names[cfg.follow],
                 hz / 1e6);
        msg(t);
        said = cfg.follow;
    }
}

/* ---- The calculator (KrakenRF's Antenna Array Size Calculator) ---- */

typedef struct {
    double lambda, s, res, want_m;      /* wavelength; spacing over it; est. resolution (deg); size for s = 0.5 */
    const char *word;
    int    level;                       /* 0 OK, 1 COARSE, 2 AMBIGUOUS */
} calc_t;

static calc_t calc_at(double f) {
    const kind_t *k = &kinds[cfg.kind];
    double        size = cfg.size_mm / 1000.0;
    calc_t        c;

    c.lambda = 299792458.0 / f;
    double d, ap;
    if (cfg.kind == K_CUSTOM) {                 /* its closest pair and its widest */
        d = 1e9, ap = 0;
        for (int a = 0; a < cust_n; a++)
            for (int b = a + 1; b < cust_n; b++) {
                double r = hypot(cust_x[a] - cust_x[b], cust_y[a] - cust_y[b]);
                if (r < d) d = r;
                if (r > ap) ap = r;
            }
        if (cust_n < 2) d = 0;
        c.want_m = 0;
    } else {
        d  = k->ula ? size : 2 * size * sin(M_PI / k->n);
        ap = k->ula ? (k->n - 1) * size : 2 * size;
        c.want_m = k->ula ? 0.5 * c.lambda : 0.5 * c.lambda / (2 * sin(M_PI / k->n));
    }
    c.s      = d / c.lambda;
    c.res    = ap > 0 ? (180 / M_PI) * (1.22 / (ap / c.lambda)) / 10 : 999;
    if (c.s > 0.5) c.word = "AMBIGUOUS", c.level = 2;
    else if (c.res > 20 || c.s < 0.2) c.word = "COARSE", c.level = 1;
    else c.word = "OK", c.level = 0;
    return c;
}

/* Element k's place, metres (x forward, y left), the calculator's way: a circle's ANT 0 forward, the rest clockwise;
 * a line from ANT 0 on the left, centred */
static void elem(int k, double *x, double *y) {
    const kind_t *kd = &kinds[cfg.kind];
    double        size = cfg.size_mm / 1000.0;

    if (cfg.kind == K_CUSTOM) {
        *x = k < cust_n ? cust_x[k] : 0;
        *y = k < cust_n ? cust_y[k] : 0;
    } else if (kd->ula) {
        *x = 0;
        *y = (kd->n - 1) * size / 2 - k * size;
    } else {
        *x = size * cos(2 * M_PI * k / kd->n);
        *y = -size * sin(2 * M_PI * k / kd->n);
    }
}

/* ---- Colours, pixels ---- */

static lv_color_t amb, ink, dim, faint, green, red, magenta;
static uint32_t   c_amb, c_ink, c_dim, c_faint, c_green, c_red, c_mag, glow[64];

static uint32_t argb(lv_color_t c) { return lv_color_to32(c) | 0xff000000; }

static uint32_t mix(uint32_t fg, uint32_t bg, int a) {
    if (a <= 0) return bg;
    if (a >= 255) return fg;
    uint32_t r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * (255 - a)) / 255;
    uint32_t g = (((fg >> 8) & 0xff) * a + ((bg >> 8) & 0xff) * (255 - a)) / 255;
    uint32_t b = ((fg & 0xff) * a + (bg & 0xff) * (255 - a)) / 255;
    return 0xff000000 | r << 16 | g << 8 | b;
}

static lv_img_dsc_t *scope, *bars, *spec, *wf;
static uint16_t     *ang;           /* each compass pixel's angle on screen, tenths of a degree; 0xffff outside */
static uint16_t     *rad;           /* and its distance from the centre, quarter pixels */

static void put(uint32_t *px, int w, int h, int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[y * w + x] = c;
}

static void line(uint32_t *px, int w, int h, int x0, int y0, int x1, int y1, uint32_t c, int dash) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;

    for (int n = 0; n < 4 * SC; n++) {
        if (!dash || (n / dash) % 2 == 0) put(px, w, h, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

static void disc(uint32_t *px, double cx, double cy, double r, bool fill, uint32_t c) {
    for (int y = (int)floor(cy - r - 1); y <= (int)ceil(cy + r + 1); y++)
        for (int x = (int)floor(cx - r - 1); x <= (int)ceil(cx + r + 1); x++) {
            double d = hypot(x - cx, y - cy);
            if (d <= r + 0.5 && (fill || d >= r - 1.5)) put(px, SC, SC, x, y, c);
        }
}

/* ---- What page I shows ---- */

static lv_obj_t   *root, *title_l, *status_l, *badge_l[2], *scope_img, *centre_l, *up_l, *ant_l[DF_PROTO_MAXCH];
static lv_obj_t   *tgn_l[DF_PROTO_TARGETS];
static lv_obj_t   *tgt_l[3], *arr_cap_l, *arr_l[2], *row_l[NANT], *row_r[NANT], *bars_img, *spec_img, *wf_img;
static lv_obj_t   *legend_l, *key_l[5], *help_l, *msg_l, *panel, *panel_head_l, *panel_row_l[PANEL_ROWS], *panel_foot_l;
static lv_timer_t *timer;
static bool        help_on;
static int         rot_acc;
static uint32_t    msg_until;
static bool        built;           /* page I's objects exist: what touches them waits for it */
static struct {
    double   brg;               /* screen degrees as the daemon said them (before the heading) */
    uint32_t ms;
} trail[TRAIL];
static int      n_trail;
static uint32_t last_do;        /* the DFDO seq taken into the trail */
static uint32_t auto_ms;

static void msg(const char *t) {
    if (!built) return;
    lv_label_set_text(msg_l, t);
    lv_obj_clear_flag(msg_l, LV_OBJ_FLAG_HIDDEN);
    msg_until = lv_tick_get() + 4000;
}

static const char *state_name(int st) {
    static const char *const n[] = {"NO KRAKEN", "OPENING", "CALIBRATING", "LISTENING", "TRACKING",
                                    "HOLD",      "SLIP",    "NOT CALIBRATED", "ERROR"};
    return st >= 0 && st <= DF_ST_ERROR ? n[st] : "?";
}

/* An antenna's colour and its words: what it is doing, and whether it is in the bearing */
static uint32_t cond_c(int cond) {
    switch (cond) {
        case DF_CH_OK: return c_green;
        case DF_CH_QUIET: return c_amb;
        case DF_CH_LOW:
        case DF_CH_SLIP: return c_red;
        case DF_CH_CLIP: return c_mag;
        case DF_CH_NOCAL: return c_dim;
        default: return c_faint;        /* OFF, GONE */
    }
}

/* Short, for the antennas' box (21 letters a row) */
static const char *cond_name(int cond) {
    static const char *const n[] = {"GONE", "IN", "QUIET", "LOW, OUT", "SLIPPED", "CLIPPING", "NO CAL", "OFF (SET)"};
    return cond >= 0 && cond <= DF_CH_OFF ? n[cond] : "?";
}

/* The channel on antenna k (the Kraken's CHk), its condition as the daemon says it; without the daemon: GONE */
static int ant_cond(int k, bool *used) {
    *used = false;
    if (!sn.connected || !sn.have_st || k >= sn.st.nch) return DF_CH_GONE;
    *used = sn.have_do && (sn.dd.used_mask >> k & 1);
    return sn.st.ch[k].cond;
}

static uint32_t fit_c(void) {
    int f = sn.have_do ? sn.dd.fit_pct : 0;
    return f >= 80 ? c_green : f >= 50 ? c_amb : c_red;
}

/* ---- The compass ---- */

static void draw_scope(void) {
    uint32_t *px = (uint32_t *)scope->data;
    double    cx = SC / 2.0 - 0.5, cy = SC / 2.0 - 0.5, R = SC / 2.0 - 18, Rin = R * 0.62;
    int       head = heading_now();
    bool      have = sn.connected && sn.have_do;
    calc_t    c = calc_at(target_hz());
    uint8_t   p[360];
    int       gate = have && sn.dd.gate;
    uint32_t  now = lv_tick_get();
    bool      hold = have && !gate && sn.dd.bearing_ddeg >= 0 && sn.dd.age_ds < 300;
    int       hold_a = hold ? 255 - sn.dd.age_ds * 200 / 300 : 255;      /* HOLD fades over 30 s */

    if (have) memcpy(p, sn.dd.p, 360);
    else memset(p, 0, 360);

    /* the ground; the method's spectrum as a filled ring between Rin and R; the wedge of the bearing */
    double brg = have && sn.dd.bearing_ddeg >= 0 ? sn.dd.bearing_ddeg / 10.0 : -1;
    double unc = have ? sn.dd.uncert_ddeg / 10.0 : 0;
    if (unc < 1) unc = 1;
    uint32_t wedge = mix(fit_c(), c_ink, (gate ? 90 : 50) * hold_a / 255);

    for (int i = 0; i < SC * SC; i++) {
        uint16_t a = ang[i];
        int      r4 = rad[i];

        if (a == 0xffff) {
            px[i] = c_ink;
            continue;
        }
        uint32_t col = c_ink;
        double   r = r4 / 4.0;

        if (r >= Rin && r <= R) {
            int    deg = ((a / 10) - head + 720) % 360;              /* the daemon's degree under this pixel */
            double reach = Rin + (R - Rin) * p[deg] / 255.0;

            /* dimmed while no signal is over the squelch (LISTENING): the ring is then the noise's, as the
             * bearings' waterfall dims those rows */
            if (have && r <= reach) col = gate ? glow[8 + p[deg] * 40 / 255] : mix(glow[8 + p[deg] * 40 / 255], c_ink, 110);
            if (brg >= 0 && (gate || hold)) {
                double d = fabs(fmod(deg - brg + 540, 360) - 180);
                if (d <= unc && r > reach) col = wedge;
            }
        }
        px[i] = col;
    }
    /* rings, ticks: every 10 degrees, longer every 30 */
    for (int a = 0; a < 3600; a += 5) {
        double s = sin(a * M_PI / 1800), co = cos(a * M_PI / 1800);
        put(px, SC, SC, (int)lround(cx + R * s), (int)lround(cy - R * co), c_dim);
        if (a % 30 == 0) put(px, SC, SC, (int)lround(cx + Rin * s), (int)lround(cy - Rin * co), c_faint);
    }
    for (int a = 0; a < 360; a += 10) {
        double s = sin(a * M_PI / 180), co = cos(a * M_PI / 180), l = a % 30 ? 4 : 9;
        line(px, SC, SC, (int)lround(cx + R * s), (int)lround(cy - R * co), (int)lround(cx + (R + l) * s),
             (int)lround(cy - (R + l) * co), a % 90 ? c_dim : c_amb, 0);
    }

    /* the array to scale: what is wider, the array or half a wavelength, fills the inside */
    const kind_t *kd = &kinds[cfg.kind];
    double        ext = c.lambda / 4, ex[DF_PROTO_MAXCH], ey[DF_PROTO_MAXCH];

    for (int k = 0; k < kind_n(); k++) {
        double x, y;
        elem(k, &x, &y);
        ex[k] = x, ey[k] = y;
        if (hypot(x, y) > ext) ext = hypot(x, y);
    }
    double sc = (Rin - 22) / ext;       /* pixels a metre */
    for (double a = 0; a < 360; a += 4) {   /* half a wavelength across, dashed */
        double s = sin(a * M_PI / 180), co = cos(a * M_PI / 180), r = c.lambda / 4 * sc;
        put(px, SC, SC, (int)lround(cx + r * s), (int)lround(cy - r * co), c_faint);
        put(px, SC, SC, (int)lround(cx + r * s) + 1, (int)lround(cy - r * co), c_faint);
    }
    if (kd->ula) {                      /* the line itself */
        double x0, y0, x1, y1;
        elem(0, &x0, &y0);
        elem(kind_n() - 1, &x1, &y1);
        double a0 = atan2(-y0, x0) + head * M_PI / 180, a1 = atan2(-y1, x1) + head * M_PI / 180;
        double r0 = hypot(x0, y0) * sc, r1 = hypot(x1, y1) * sc;
        line(px, SC, SC, (int)lround(cx + r0 * sin(a0)), (int)lround(cy - r0 * cos(a0)), (int)lround(cx + r1 * sin(a1)),
             (int)lround(cy - r1 * cos(a1)), c_faint, 0);
    }

    /* the needle (the bearing) and a line's other side, dashed; other peaks as ticks on the rim */
    if (brg >= 0 && (gate || hold)) {
        double a = (brg + head) * M_PI / 180;
        uint32_t nc = mix(fit_c(), c_ink, hold_a);
        for (int w = -1; w <= 1; w++)
            line(px, SC, SC, (int)lround(cx + w * cos(a)), (int)lround(cy + w * sin(a)),
                 (int)lround(cx + (R + 10) * sin(a) + w * cos(a)), (int)lround(cy - (R + 10) * cos(a) + w * sin(a)), nc, 0);
        if (sn.dd.mirror_ddeg >= 0) {
            double m = (sn.dd.mirror_ddeg / 10.0 + head) * M_PI / 180;
            line(px, SC, SC, (int)lround(cx), (int)lround(cy), (int)lround(cx + (R + 6) * sin(m)),
                 (int)lround(cy - (R + 6) * cos(m)), mix(nc, c_ink, 160), 4);
        }
    }
    for (int i = 0; i < N_TG; i++) {               /* the other targets: a thin needle each, its number at the tip */
        bool hold;
        int  tb = tg_brg(i, &hold);

        if (tb < 0) {
            lv_obj_add_flag(tgn_l[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        tb = sn.tg.tg[i].bearing_ddeg;              /* drawn as the main one is: the daemon's, turned by head */
        double   a = (tb / 10.0 + head) * M_PI / 180;
        uint32_t tc = 0xff000000u | tg_col[i];

        if (hold) tc = mix(tc, c_ink, 150);
        line(px, SC, SC, (int)lround(cx + 18 * sin(a)), (int)lround(cy - 18 * cos(a)), (int)lround(cx + (R - 4) * sin(a)),
             (int)lround(cy - (R - 4) * cos(a)), tc, hold ? 4 : 0);
        lv_obj_set_pos(tgn_l[i], SC_X + (int)lround(cx + (R - 16) * sin(a)) - 9, SC_Y + (int)lround(cy - (R - 16) * cos(a)) - 8);
        lv_obj_clear_flag(tgn_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; have && i < sn.dd.npeaks && i < 3; i++) {
        double a = (sn.dd.peaks_ddeg[i] / 10.0 + head) * M_PI / 180;
        line(px, SC, SC, (int)lround(cx + (R - 8) * sin(a)), (int)lround(cy - (R - 8) * cos(a)),
             (int)lround(cx + (R + 6) * sin(a)), (int)lround(cy - (R + 6) * cos(a)), c_amb, 0);
    }
    /* the last bearings: dots just outside the rim, fading over two minutes */
    for (int i = 0; i < n_trail; i++) {
        uint32_t age = now - trail[i].ms;
        if (age > 120000) continue;
        double a = (trail[i].brg + head) * M_PI / 180, r = R + 13;
        disc(px, cx + r * sin(a), cy - r * cos(a), 1.5, true, mix(c_amb, c_ink, 255 - (int)(age * 220 / 120000)));
    }

    /* the antennas: filled in their state's colour while in the bearing, a ring alone when not; a bar of the
     * target's level on each (0-40 dB over its floor) */
    for (int k = 0; k < kind_n(); k++) {
        bool     used;
        int      cond = ant_cond(k, &used);
        double   a = atan2(-ey[k], ex[k]) + head * M_PI / 180, r = hypot(ex[k], ey[k]) * sc;
        double   x = cx + r * sin(a), y = cy - r * cos(a);
        uint32_t col = cond_c(cond);

        disc(px, x, y, 9, used, col);
        if (!used) disc(px, x, y, 8, false, col);
        if (sn.have_do && sn.connected) {
            int db = sn.dd.ch_ddb[k] / 10, h = db <= 0 ? 0 : db >= 40 ? 20 : db / 2;
            for (int yy = 0; yy < 20; yy++)
                for (int xx = 12; xx < 15; xx++) put(px, SC, SC, (int)x + xx, (int)y + 9 - yy, yy < h ? col : c_faint);
        }
        lv_obj_set_pos(ant_l[k], SC_X + (int)lround(x) - 9, SC_Y + (int)lround(y) - 8);
        lv_obj_set_style_text_color(ant_l[k], used ? ink : lv_color_hex(col & 0xffffff), 0);
        lv_obj_clear_flag(ant_l[k], LV_OBJ_FLAG_HIDDEN);
    }
    for (int k = kind_n(); k < DF_PROTO_MAXCH; k++) lv_obj_add_flag(ant_l[k], LV_OBJ_FLAG_HIDDEN);

    /* the middle */
    line(px, SC, SC, (int)cx - 3, (int)cy, (int)cx + 3, (int)cy, c_dim, 0);
    line(px, SC, SC, (int)cx, (int)cy - 3, (int)cx, (int)cy + 3, c_dim, 0);
    lv_label_set_text(up_l, head ? "N" : "FWD");
}

/* The words over the compass when there is nothing to point at */
static void show_centre(void) {
    char t[160] = "";

    if (!sn.connected) snprintf(t, sizeof(t), "%s", access("/usr/bin/x6x00-df", X_OK) ? "NO X6X00-DF ON THIS IMAGE"
                                                                                      : "STARTING THE DIRECTION FINDER");
    else if (!sn.have_st) snprintf(t, sizeof(t), "WAITING FOR THE DAEMON");
    else if (sn.st.state == DF_ST_NODEV) snprintf(t, sizeof(t), "NO KRAKEN\nFIVE STICKS 1000-1004 WANTED");
    else if (sn.st.state == DF_ST_OPENING) snprintf(t, sizeof(t), "OPENING THE STICKS");
    else if (sn.st.state == DF_ST_CAL) snprintf(t, sizeof(t), "CALIBRATING\nNOISE SOURCE ON");
    else if (sn.st.state == DF_ST_NOCAL || sn.st.state == DF_ST_ERROR) snprintf(t, sizeof(t), "%s\n%s", state_name(sn.st.state), sn.st.why);
    else if (sn.st.state == DF_ST_SLIP) snprintf(t, sizeof(t), "SAMPLES LOST\nCALIBRATING AGAIN");
    if (t[0]) {
        lv_label_set_text(centre_l, t);
        lv_obj_clear_flag(centre_l, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(centre_l, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- The right-hand side ---- */

static void fmt_mhz(char *s, size_t n, uint32_t hz) { snprintf(s, n, "%u.%04u", hz / 1000000, (hz % 1000000) / 100); }

static void show_target(void) {
    char f[16], t[3][120];
    int  st = sn.connected && sn.have_st ? sn.st.state : -1;

    fmt_mhz(f, sizeof(f), target_hz());
    snprintf(t[0], sizeof(t[0]), "%s MHZ  %gK%s  #%06X %s#", f, bws[cfg.bw] / 1000.0, cfg.autot ? " AUTO" : "",
             (unsigned)((st == DF_ST_TRACK ? c_green : st == DF_ST_SLIP || st == DF_ST_ERROR ? c_red : c_amb) & 0xffffff),
             st < 0 ? "NO DAEMON" : state_name(st));
    if (sn.have_do && sn.dd.bearing_ddeg >= 0 && (sn.dd.gate || st == DF_ST_HOLD)) {
        int b = sn.dd.bearing_ddeg, h = heading_now();
        if (*heading_from()) b = (b + h * 10) % 3600;
        snprintf(t[1], sizeof(t[1]), "#%06X BRG %03d.%d%s +-%d.%d#  FIT %d%%  %s", (unsigned)(fit_c() & 0xffffff), b / 10,
                 b % 10, *heading_from() ? " N" : "", sn.dd.uncert_ddeg / 10, sn.dd.uncert_ddeg % 10, sn.dd.fit_pct,
                 methods[cfg.method]);
    } else {
        snprintf(t[1], sizeof(t[1]), "BRG ---  %s", methods[cfg.method]);
    }
    char cal[24] = "CAL NONE";

    if (sn.have_st && sn.st.cal_age_s != 0xffff) snprintf(cal, sizeof(cal), "CAL %uS AGO", sn.st.cal_age_s);
    if (sn.have_do)
        snprintf(t[2], sizeof(t[2]), "%+d DB  SNR %d  %s  SLIPS %u", sn.dd.level_ddb / 10, sn.dd.snr_ddb / 10, cal,
                 sn.have_st ? sn.st.slips : 0);
    else snprintf(t[2], sizeof(t[2]), "NO ESTIMATE YET  %s", cal);
    for (int i = 0; i < 3; i++) lv_label_set_text(tgt_l[i], t[i]);
}

static void show_array(void) {
    calc_t        c = calc_at(target_hz());
    const kind_t *k = &kinds[cfg.kind];
    int           b = band_of_target();
    char          t[2][140];
    uint32_t      wc = c.level == 0 ? c_green : c.level == 1 ? c_amb : c_red;

    int any = 0;

    for (int i = 0; i < N_TG; i++) any += cfg.tg_hz[i] != 0;
    if (any) {                                      /* the other targets in its place (F2 still shows the array) */
        char l[2][160] = {"", ""};

        lv_label_set_text(arr_cap_l, "OTHER TARGETS (F1 TUNE)");
        for (int i = 0, k = 0; i < N_TG; i++) {
            char f[16], b[48];
            int  row = k < 2 ? 0 : 1;

            if (!cfg.tg_hz[i]) continue;
            fmt_mhz(f, sizeof(f), cfg.tg_hz[i]);
            tg_short(i, b, sizeof(b));
            size_t u = strlen(l[row]);

            snprintf(l[row] + u, sizeof(l[row]) - u, "%s#%06X %d# %s %s", u ? "   " : "", (unsigned)tg_col[i], i + 2, f, b);
            k++;
        }
        lv_label_set_text(arr_l[0], l[0]);
        lv_label_set_text(arr_l[1], l[1]);
        return;
    }
    lv_label_set_text_fmt(arr_cap_l, "ARRAY AGAINST THE BAND: %s", b >= 0 ? bands[b].name : "TUNED");
    if (cfg.kind == K_CUSTOM)
        snprintf(t[0], sizeof(t[0]), "CUSTOM %d ANT  S %.2f  RES %.0f DEG  #%06X %s#", cust_n, c.s, c.res,
                 (unsigned)(wc & 0xffffff), c.word);
    else
        snprintf(t[0], sizeof(t[0]), "%s %s %.1f CM  S %.2f  RES %.0f DEG  #%06X %s#", k->name, k->ula ? "D" : "R",
                 cfg.size_mm / 10.0, c.s, c.res, (unsigned)(wc & 0xffffff), c.word);
    const char *up = !*heading_from() ? "ANT0 UP" : !strcmp(heading_from(), "SET") ? "N UP"
                     : !strcmp(heading_from(), "COMPASS") ? "N UP CMP" : "N UP GPS";
    if (cfg.kind == K_CUSTOM)
        snprintf(t[1], sizeof(t[1]), "CLOSEST PAIR %.1f CM  HALF WAVE %.1f CM  %s", c.s * c.lambda * 100, c.lambda * 50, up);
    else
        snprintf(t[1], sizeof(t[1]), "FOR S 0.50: %s %.1f CM  HALF WAVE %.1f CM  %s", k->ula ? "D" : "R", c.want_m * 100,
                 c.lambda * 50, up);
    lv_label_set_text(arr_l[0], t[0]);
    lv_label_set_text(arr_l[1], t[1]);
}

static void show_ants(void) {
    uint32_t *px = (uint32_t *)bars->data;
    int       n = kind_n();

    for (int i = 0; i < BAR_W * NANT * ROW_H; i++) px[i] = c_ink;
    for (int r = 0; r < NANT; r++) {
        bool     used;
        int      cond = ant_cond(r, &used);
        uint32_t col = cond_c(cond);
        char     sn_s[12] = "----";
        int      db = sn.have_do && sn.connected ? sn.dd.ch_ddb[r] : 0;

        char ant[8] = "----";

        if (sn.have_st && r < sn.st.nch && sn.st.ch[r].serial[0]) snprintf(sn_s, sizeof(sn_s), "%.8s", sn.st.ch[r].serial);
        if (r < n) snprintf(ant, sizeof(ant), "ANT%d", r);
        lv_label_set_text_fmt(row_l[r], "#%06X %s# CH%d %s", (unsigned)(col & 0xffffff), ant, r, sn_s);
        if (r >= n) lv_label_set_text(row_r[r], "NOT IN THE ARRAY");
        else if (cond == DF_CH_GONE) lv_label_set_text_fmt(row_r[r], "#%06X GONE#", (unsigned)(c_faint & 0xffffff));
        else if (cond == DF_CH_CLIP && sn.have_do)
            lv_label_set_text_fmt(row_r[r], "%+5.1f DB #%06X CLIP %d.%d%%#", db / 10.0, (unsigned)(col & 0xffffff),
                                  sn.dd.ch_clip_pm[r] / 10, sn.dd.ch_clip_pm[r] % 10);
        else lv_label_set_text_fmt(row_r[r], "%+5.1f DB #%06X %s#", db / 10.0, (unsigned)(col & 0xffffff),
                                   !used && cond == DF_CH_OK ? "OUT" : cond_name(cond));
        int w = db <= 0 ? 0 : db >= 400 ? BAR_W : db * BAR_W / 400;
        for (int y = r * ROW_H + 4; y < r * ROW_H + ROW_H - 4; y++)
            for (int x = 0; x < BAR_W; x++) px[y * BAR_W + x] = x < w ? col : (x % 4 == 0 ? c_faint : c_ink);
    }
    lv_obj_invalidate(bars_img);
}

static void show_spec(void) {
    uint32_t *px = (uint32_t *)spec->data;
    uint32_t  rate = sn.have_sp ? sn.sp.rate : rates[cfg.rate];
    double    lo = cfg.off - bws[cfg.bw] / 2.0, hi = cfg.off + bws[cfg.bw] / 2.0;

    for (int x = 0; x < SPEC_W; x++) {
        double f = ((x + 0.5) / SPEC_W - 0.5) * rate;
        bool   in = f >= lo - rate / (2.0 * SPEC_W) && f <= hi + rate / (2.0 * SPEC_W);
        int    h = 0;

        if (sn.have_sp && sn.sp.n) {
            int i = x * sn.sp.n / SPEC_W, v = sn.sp.db[i];
            double over = v / 2.0 - 20;                         /* dB over the floor */
            h = (int)((over + 10) * SPEC_H / 50);
            if (h < 0) h = 0;
            if (h > SPEC_H) h = SPEC_H;
        }
        for (int y = 0; y < SPEC_H; y++) {
            uint32_t c = in ? mix(c_amb, c_ink, 40) : c_ink;
            if (SPEC_H - 1 - y < h) c = in ? c_amb : c_dim;
            px[y * SPEC_W + x] = c;
        }
    }
    int xc = (int)((cfg.off / (double)rate + 0.5) * SPEC_W);   /* the target */
    for (int y = 0; y < SPEC_H; y += 2) put(px, SPEC_W, SPEC_H, xc, y, c_green);
    for (int i = 0; i < N_TG; i++) {                            /* the other targets */
        int32_t off;

        if (!tg_off(i, &off)) continue;
        int xt = (int)((off / (double)rate + 0.5) * SPEC_W);

        for (int y = 1; y < SPEC_H; y += 2) put(px, SPEC_W, SPEC_H, xt, y, 0xff000000u | tg_col[i]);
    }
    lv_obj_invalidate(spec_img);

    /* the bearings' waterfall: 0-359 across, the newest on top, a row each WF_STEP estimates */
    uint32_t *w = (uint32_t *)wf->data;
    uint8_t   p[360], g;

    for (int r = 0; r < WF_H; r++) {
        uint32_t n = sn.do_n >= 1 + (uint32_t)(r * WF_STEP) ? sn.do_n - 1 - (uint32_t)(r * WF_STEP) : 0;
        bool     ok = sn.do_n > (uint32_t)(r * WF_STEP) && df_rx_hist(n, p, &g);

        for (int x = 0; x < SPEC_W; x++) {
            int deg = (x * 360 / SPEC_W - heading_now() + 360) % 360;   /* as the compass: by the heading */
            w[r * SPEC_W + x] = ok ? (g ? glow[p[deg] * 63 / 255] : mix(glow[p[deg] * 63 / 255], c_ink, 110)) : c_ink;
        }
    }
    for (int x = 0; x < SPEC_W; x += SPEC_W / 4) w[x] = c_dim;
    lv_obj_invalidate(wf_img);
}

static void show_head(void) {
    char      t[16], line[240], src[80];
    time_t    tt = time(NULL);
    struct tm tm;
    int       lent = df_rx_b_lent();

    localtime_r(&tt, &tm);
    strftime(t, sizeof(t), "%H:%M", &tm);
    if (df_rx_synth()) snprintf(src, sizeof(src), "TEST SIGNAL (NO STICKS)");
    else snprintf(src, sizeof(src), "KRAKEN %d OF 5 ON USB", sdr_pool_array_count());
    snprintf(line, sizeof(line), "%s   %s   %s   %s", src,
             sn.have_st ? (sn.st.synth ? "DAEMON: TEST" : "DAEMON: KRAKEN") : "DAEMON: -",
             lent == 2 ? "B LENT TO I" : lent == 1 ? "B ON ANOTHER SDR" : "B AS IT WAS", t);
    if (sn.have_st && sn.st.cpu_pct) snprintf(line + strlen(line), sizeof(line) - strlen(line), "   CPU %d%%", sn.st.cpu_pct);
    if (sn.have_st && (sn.st.iq_clients & 1)) snprintf(line + strlen(line), sizeof(line) - strlen(line), "   IQ CLIENT");
    lv_label_set_text(status_l, line);
}

/* ---- The panels: F2 ARRAY, F5 SET. MFK goes down the rows, the VFO changes the one chosen, a push does it ---- */

typedef enum { P_NONE, P_ARRAY, P_SET, P_TUNE } panel_t;
static panel_t pan;
static int     pan_sel;

enum { A_BAND, A_KIND, A_SIZE, A_FIT, A_HEAD, A_N };
enum { S_RATE, S_GAIN, S_SQL, S_CAL, S_ANT0, S_SRC = S_ANT0 + NANT, S_LOG, S_N };
enum { T_FOLLOW, T_BAND, T_FREQ, T_WIDTH, T_AUTO, T_TG0, T_N = T_TG0 + N_TG };

/* F2 ARRAY's sketch: the layout chosen, to scale, and in words how to set it out for the band (the owner, 2026-11-17:
 * "the antenna array layout should be suggested / dictated by the I page. give all available choices and roughly
 * approximate what it looks like"). Turning the ARRAY row goes through them all */
#define SK_W 200
#define SK_H 132
static lv_img_dsc_t *sk;
static lv_obj_t     *sk_img, *sk_t, *sk_l[NANT];

static void sk_disc(uint32_t *px, double cx, double cy, double r, uint32_t c) {
    for (int y = (int)floor(cy - r - 1); y <= (int)ceil(cy + r + 1); y++)
        for (int x = (int)floor(cx - r - 1); x <= (int)ceil(cx + r + 1); x++)
            if (hypot(x - cx, y - cy) <= r + 0.5) put(px, SK_W, SK_H, x, y, c);
}

static void sketch_text(char *t, size_t n) {
    calc_t        c = calc_at(cfg.band >= 0 ? bands[cfg.band].hz : target_hz());
    const kind_t *k = &kinds[cfg.kind];
    char          f[16];

    fmt_mhz(f, sizeof(f), cfg.band >= 0 ? bands[cfg.band].hz : target_hz());
    if (cfg.kind == K_CUSTOM)
        snprintf(t, n, "%d ANTENNAS WHERE THE WEB PAGE'S ARRAY EDITOR PUT THEM (DF_ARRAY.TXT). CLOSEST PAIR %.1f CM: "
                       "HALF A WAVE AT %s MHZ IS %.1f.", cust_n, c.s * c.lambda * 100, f, c.lambda * 50);
    else if (k->ula)
        snprintf(t, n, "A LINE OF %d, %.1f CM APART AS SET (HALF A WAVE AT %s MHZ: %.1f CM), ACROSS THE WAY YOU FACE, "
                       "ANT 0 AT THE LEFT END. A LINE CANNOT TELL FRONT FROM BACK: THE MIRROR SHOWS DASHED.",
                 k->n, cfg.size_mm / 10.0, f, c.want_m * 100);
    else
        snprintf(t, n, "A CIRCLE OF %d SEEN FROM ABOVE, ANT 0 FORWARD, THE REST CLOCKWISE. RADIUS %.1f CM AS SET; FOR %s "
                       "MHZ %.1f CM (NEIGHBOURS HALF A WAVE APART). WHIPS UPRIGHT ON ONE GROUND PLANE.",
                 k->n, cfg.size_mm / 10.0, f, c.want_m * 100);
}

static void draw_sketch(int y0) {
    uint32_t *px = (uint32_t *)sk->data;
    int       n = kind_n();
    double    xs[NANT], ys[NANT], ex = 0.001, ey = 0.001, cx = SK_W / 2.0, cy = SK_H / 2.0 + 7;
    char      t[320];

    for (int i = 0; i < SK_W * SK_H; i++) px[i] = c_ink;
    for (int k = 0; k < n && k < NANT; k++) {
        elem(k, &xs[k], &ys[k]);
        ex = fmax(ex, fabs(xs[k]));
        ey = fmax(ey, fabs(ys[k]));
    }
    double sc  = fmin((SK_H / 2.0 - 20) / ex, (SK_W / 2.0 - 14) / ey);  /* to scale, as big as it fits (a line uses
                                                                         * the width) */
    double ext = fmax(ex, ey);

    /* forward: an arrow at the top */
    line(px, SK_W, SK_H, (int)cx, 13, (int)cx, 2, c_dim, 0);
    line(px, SK_W, SK_H, (int)cx - 4, 6, (int)cx, 2, c_dim, 0);
    line(px, SK_W, SK_H, (int)cx + 4, 6, (int)cx, 2, c_dim, 0);
    if (cfg.kind != K_CUSTOM && !kinds[cfg.kind].ula)       /* the circle they stand on */
        for (int a = 0; a < 360; a += 4) {
            double r = ext * sc, ra = a * M_PI / 180;
            put(px, SK_W, SK_H, (int)lround(cx + r * sin(ra)), (int)lround(cy - r * cos(ra)), c_faint);
        }
    else if (cfg.kind != K_CUSTOM && n >= 2)                 /* the line */
        line(px, SK_W, SK_H, (int)lround(cx - ys[0] * sc), (int)lround(cy - xs[0] * sc), (int)lround(cx - ys[n - 1] * sc),
             (int)lround(cy - xs[n - 1] * sc), c_faint, 3);
    line(px, SK_W, SK_H, (int)cx - 3, (int)cy, (int)cx + 3, (int)cy, c_dim, 0);
    line(px, SK_W, SK_H, (int)cx, (int)cy - 3, (int)cx, (int)cy + 3, c_dim, 0);
    for (int k = 0; k < NANT; k++) {
        if (k >= n) {
            lv_obj_add_flag(sk_l[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        double sx = cx - ys[k] * sc, sy = cy - xs[k] * sc;     /* x forward is up, y left is left */

        sk_disc(px, sx, sy, 9, c_amb);
        lv_obj_set_pos(sk_l[k], 12 + (int)lround(sx) - 9, y0 + (int)lround(sy) - 8);
        lv_obj_clear_flag(sk_l[k], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_y(sk_img, y0);
    lv_obj_set_y(sk_t, y0 + 4);
    sketch_text(t, sizeof(t));
    lv_label_set_text(sk_t, t);
    lv_obj_invalidate(sk_img);
}

static void sketch_show(bool on) {
    if (!sk_img) return;
    if (on) {
        lv_obj_clear_flag(sk_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(sk_t, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(sk_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(sk_t, LV_OBJ_FLAG_HIDDEN);
    for (int k = 0; k < NANT; k++) lv_obj_add_flag(sk_l[k], LV_OBJ_FLAG_HIDDEN);
}

static int pan_rows(void) { return pan == P_ARRAY ? A_N : pan == P_SET ? S_N : pan == P_TUNE ? T_N : 0; }

static void pan_fill(void) {
    if (!built) return;
    char t[PANEL_ROWS][96];
    int  n = pan_rows();

    if (pan == P_ARRAY) {
        calc_t c = calc_at(target_hz());
        char   f[16];

        lv_label_set_text(panel_head_l, "ARRAY: HOW THE ANTENNAS ARE SET OUT, AGAINST WHICH BAND");
        fmt_mhz(f, sizeof(f), cfg.band >= 0 ? bands[cfg.band].hz : target_hz());
        snprintf(t[A_BAND], sizeof(t[0]), "BAND      < %s %s MHZ >", cfg.band >= 0 ? bands[cfg.band].name : "TUNED", f);
        snprintf(t[A_KIND], sizeof(t[0]), "ARRAY     < %s >  %s", kinds[cfg.kind].name,
                 cfg.kind == K_CUSTOM      ? "WHERE THE WEB PAGE'S ARRAY EDITOR PUT THEM"
                 : kinds[cfg.kind].ula     ? "A LINE, ANT 0 AT ITS LEFT END"
                                           : "A CIRCLE, ANT 0 FORWARD, CLOCKWISE");
        if (cfg.kind == K_CUSTOM)
            snprintf(t[A_SIZE], sizeof(t[0]), "LAYOUT    %d ANTENNAS FROM DF_ARRAY.TXT  S %.2f  RES %.0f DEG  %s", cust_n,
                     c.s, c.res, c.word);
        else
            snprintf(t[A_SIZE], sizeof(t[0]), "%s    < %.1f CM >  S %.2f  RES %.0f DEG  %s", kinds[cfg.kind].ula ? "SPACING" : "RADIUS ",
                     cfg.size_mm / 10.0, c.s, c.res, c.word);
        snprintf(t[A_FIT], sizeof(t[0]), "PUSH: SIZE FOR S 0.50 AT THIS BAND (%.1f CM)", c.want_m * 100);
        if (!strcmp(heading_from(), "COMPASS") || !strcmp(heading_from(), "GPS COURSE"))
            snprintf(t[A_HEAD], sizeof(t[0]), "HEADING   < %d >  IN USE: %s %d (GPSD)", cfg.heading, heading_from(), heading_now());
        else snprintf(t[A_HEAD], sizeof(t[0]), "HEADING   < %d >  ANT 0'S COMPASS BEARING, 0 = ANT 0 UP", cfg.heading);
    } else if (pan == P_TUNE) {
        char f[16];
        int  b = band_of_target();

        lv_label_set_text(panel_head_l, "TUNE: WHERE TO TAKE A BEARING");
        snprintf(t[T_FOLLOW], sizeof(t[0]), "FOLLOW    < %s >  %s", follow_names[cfg.follow],
                 cfg.follow == 1 ? "THE X6200'S DIAL, AS IT CHANGES" : cfg.follow == 2 ? "B'S FREQUENCY, AS IT CHANGES"
                                                                       : "TUNED HERE");
        fmt_mhz(f, sizeof(f), b >= 0 ? bands[b].hz : target_hz());
        snprintf(t[T_BAND], sizeof(t[0]), "BAND      < %s %s MHZ >", b >= 0 ? bands[b].name : "-", b >= 0 ? f : "");
        fmt_mhz(f, sizeof(f), target_hz());
        snprintf(t[T_FREQ], sizeof(t[0]), "FREQUENCY < %s MHZ >  A CHANNEL A STEP", f);
        snprintf(t[T_WIDTH], sizeof(t[0]), "WIDTH     < %g KHZ >", bws[cfg.bw] / 1000.0);
        snprintf(t[T_AUTO], sizeof(t[0]), "AUTO      < %s >  THE STRONGEST SIGNAL IN THE PASSBAND", cfg.autot ? "ON" : "OFF");
        for (int i = 0; i < N_TG; i++) {
            char b[48];

            if (!cfg.tg_hz[i]) {
                snprintf(t[T_TG0 + i], sizeof(t[0]), "TARGET %d  < OFF >  PUSH: ONE MORE ON THE FREQUENCY ABOVE", i + 2);
                continue;
            }
            fmt_mhz(f, sizeof(f), cfg.tg_hz[i]);
            tg_text(i, b, sizeof(b));
            int l = snprintf(t[T_TG0 + i], sizeof(t[0]), "TARGET %d  < %s MHZ >  %s", i + 2, f, b);

            if (l <= 44) snprintf(t[T_TG0 + i] + l, sizeof(t[0]) - (size_t)l, "  PUSH: CLEAR");    /* as it fits */
        }
    } else if (pan == P_SET) {
        lv_label_set_text(panel_head_l, "SET: THE KRAKEN AND THE BEARING");
        snprintf(t[S_RATE], sizeof(t[0]), "RATE      < %.3f MS/S >  WIDER SEES MORE, COSTS CPU", rates[cfg.rate] / 1e6);
        snprintf(t[S_GAIN], sizeof(t[0]), "GAIN      < %.1f DB >  EVERY CHANNEL THE SAME", gains[cfg.gain] / 10.0);
        snprintf(t[S_SQL], sizeof(t[0]), "SQUELCH   < %d DB >  OVER THE FLOOR TO TAKE A BEARING", cfg.sql_ddb / 10);
        if (cal_every[cfg.cal]) snprintf(t[S_CAL], sizeof(t[0]), "CALIBRATE < EVERY %d S >  IN A GAP IN THE SIGNAL", cal_every[cfg.cal]);
        else snprintf(t[S_CAL], sizeof(t[0]), "CALIBRATE < ON F4 ONLY >");
        for (int k = 0; k < NANT; k++)
            snprintf(t[S_ANT0 + k], sizeof(t[0]), "ANT %d     < %s >", k, cfg.off_mask >> k & 1 ? "LEFT OUT" : "USED");
        snprintf(t[S_SRC], sizeof(t[0]), "SOURCE    < %s >", df_rx_synth() ? "TEST SIGNAL" : "KRAKEN");
        snprintf(t[S_LOG], sizeof(t[0]), "LOG       < %s >  EACH BEARING TO /MNT/DF_LOG, A CSV A DAY", cfg.log ? "ON" : "OFF");
    }
    for (int r = 0; r < PANEL_ROWS; r++) {
        if (r >= n) {
            lv_obj_add_flag(panel_row_l[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(panel_row_l[r], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(panel_row_l[r], t[r]);
        bool on = r == pan_sel;
        lv_obj_set_style_bg_opa(panel_row_l[r], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(panel_row_l[r], on ? ink : amb, 0);
    }
    int extra = pan == P_ARRAY ? SK_H + 10 : 0;        /* F2 ARRAY: the layout sketched under its rows */

    if (pan == P_ARRAY) draw_sketch(50 + n * 28 + 4);
    sketch_show(pan == P_ARRAY);
    lv_obj_set_height(panel, 50 + n * 28 + 40 + extra);
    lv_obj_set_y(panel_foot_l, 50 + n * 28 + 8 + extra);
}

static void pan_open(panel_t p) {
    pan     = p;
    pan_sel = 0;
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
    pan_fill();
}

static void pan_close(void) {
    pan = P_NONE;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    cfg_save();
}

static void show_keys(void);
static void refresh(void);

/* The VFO on the chosen row: d steps */
static void pan_change(int d) {
    if (pan == P_ARRAY) {
        switch (pan_sel) {
            case A_BAND:
                follow_off();
                band_set(((cfg.band < 0 ? 0 : cfg.band) + d + N_BANDS) % N_BANDS);
                break;
            case A_KIND:
                cfg.kind = (cfg.kind + d + N_KINDS) % N_KINDS;
                if (cfg.kind == K_CUSTOM && !cust_load()) {     /* no layout of your own yet: past it */
                    msg("CUSTOM: NO /MNT/DF_ARRAY.TXT YET (THE WEB PAGE'S ARRAY EDITOR WRITES IT)");
                    cfg.kind = (cfg.kind + d + N_KINDS) % N_KINDS;
                }
                send_array();
                break;
            case A_SIZE:
                if (cfg.kind == K_CUSTOM) {
                    msg("CUSTOM: ITS LAYOUT IS THE FILE'S (THE WEB PAGE'S ARRAY EDITOR)");
                    break;
                }
                cfg.size_mm += 5 * d;
                if (cfg.size_mm < 10) cfg.size_mm = 10;
                if (cfg.size_mm > 5000) cfg.size_mm = 5000;
                send_array();
                break;
            case A_HEAD: cfg.heading = (cfg.heading + d + 360) % 360; send_log(); break;
            default: break;
        }
    } else if (pan == P_TUNE) {
        switch (pan_sel) {
            case T_FOLLOW:
                cfg.follow = (cfg.follow + d + 3) % 3;
                break;
            case T_BAND: {
                int b = band_of_target();

                follow_off();
                band_set(b < 0 ? (d > 0 ? 0 : N_BANDS - 1) : (b + d + N_BANDS) % N_BANDS);
                break;
            }
            case T_FREQ: {
                int b = band_of_target();

                follow_off();
                cfg.autot = false;
                tune_to((int64_t)target_hz() + d * (int64_t)(b >= 0 ? bands[b].step : bws[cfg.bw]));
                break;
            }
            case T_WIDTH:
                cfg.bw = cfg.bw + d < 0 ? 0 : cfg.bw + d >= N_BWS ? N_BWS - 1 : cfg.bw + d;
                clamp_target();
                send_target();
                break;
            case T_AUTO: cfg.autot = !cfg.autot; break;
            default:
                if (pan_sel >= T_TG0 && pan_sel < T_TG0 + N_TG) {          /* a channel a step, in the passband */
                    int     i = pan_sel - T_TG0, b = band_of_target();
                    int32_t off;

                    if (!cfg.tg_hz[i]) {
                        msg("PUSH TO SET IT ON THE FREQUENCY ABOVE, THEN TURN TO MOVE IT");
                        break;
                    }
                    uint32_t was = cfg.tg_hz[i];

                    cfg.tg_hz[i] += d * (int32_t)(b >= 0 ? bands[b].step : bws[cfg.tg_bw[i]]);
                    if (!tg_off(i, &off)) {
                        cfg.tg_hz[i] = was;
                        msg("THE PASSBAND ENDS THERE: TUNE THE MAIN TARGET NEARER, OR A WIDER RATE (F5)");
                    }
                    send_tg(i);
                }
                break;
        }
    } else if (pan == P_SET) {
        switch (pan_sel) {
            case S_RATE:
                cfg.rate = cfg.rate + d < 0 ? 0 : cfg.rate + d >= N_RATES ? N_RATES - 1 : cfg.rate + d;
                clamp_target();
                send_rate();
                send_target();
                break;
            case S_GAIN:
                cfg.gain = cfg.gain + d < 0 ? 0 : cfg.gain + d >= N_GAINS ? N_GAINS - 1 : cfg.gain + d;
                send_gain();
                break;
            case S_SQL:
                cfg.sql_ddb += 10 * d;
                if (cfg.sql_ddb < 30) cfg.sql_ddb = 30;
                if (cfg.sql_ddb > 400) cfg.sql_ddb = 400;
                send_sql();
                break;
            case S_CAL:
                cfg.cal = cfg.cal + d < 0 ? 0 : cfg.cal + d >= N_CALS ? N_CALS - 1 : cfg.cal + d;
                send_cal();
                break;
            case S_LOG:
                cfg.log = !cfg.log;
                send_log();
                break;
            case S_SRC:
                df_rx_set_synth(!df_rx_synth());
                df_rx_close();          /* the daemon started again with the other source; the pool and B as fits */
                df_rx_open();
                df_rx_cmd("Q");
                n_trail = 0;
                break;
            default:
                if (pan_sel >= S_ANT0 && pan_sel < S_ANT0 + NANT) {
                    cfg.off_mask ^= 1 << (pan_sel - S_ANT0);
                    send_mask();
                }
                break;
        }
    }
    pan_fill();
    show_keys();
    refresh();
}

static void pan_push(void) {
    if (pan == P_ARRAY && pan_sel == A_FIT) {
        calc_t c = calc_at(target_hz());
        if (cfg.kind == K_CUSTOM) {
            msg("CUSTOM: ITS LAYOUT IS THE FILE'S (THE WEB PAGE'S ARRAY EDITOR)");
            return;
        }
        cfg.size_mm = (int)lround(c.want_m * 200) * 5;          /* to the half centimetre */
        send_array();
        msg("ARRAY SIZE SET FOR S 0.50: SET THE ANTENNAS OUT SO");
        pan_fill();
        refresh();
        return;
    }
    if (pan == P_TUNE && pan_sel >= T_TG0 && pan_sel < T_TG0 + N_TG) {
        int i = pan_sel - T_TG0;

        if (cfg.tg_hz[i]) {
            tg_set_hz(i, 0);
            msg("TARGET CLEARED");
        } else {
            for (int k = 0; k < N_TG; k++)
                if (cfg.tg_hz[k] == target_hz()) {
                    msg("THAT FREQUENCY HAS A TARGET ALREADY: TUNE THE MAIN ONE ELSEWHERE FIRST");
                    return;
                }
            tg_set_hz(i, target_hz());
            msg("TARGET SET HERE: NOW TUNE THE MAIN ONE ELSEWHERE IN THE PASSBAND");
        }
        cfg_save();
        pan_fill();
        refresh();
        return;
    }
    if ((pan == P_SET && (pan_sel == S_SRC || pan_sel == S_LOG || (pan_sel >= S_ANT0 && pan_sel < S_ANT0 + NANT))) ||
        (pan == P_TUNE && (pan_sel == T_FOLLOW || pan_sel == T_AUTO))) {
        pan_change(1);
        return;
    }
    pan_close();
    show_keys();
    refresh();
}

/* ---- Keys ---- */

static const char *f_label(int i) {
    static char s[5][32];

    switch (i) {
        case 0:
            if (cfg.follow) snprintf(s[0], sizeof(s[0]), "TUNE\nFOLLOW %s", follow_names[cfg.follow]);
            else snprintf(s[0], sizeof(s[0]), "TUNE\n%s%gK", cfg.autot ? "AUTO " : "", bws[cfg.bw] / 1000.0);
            break;
        case 1: {
            int b = band_of_target();
            snprintf(s[1], sizeof(s[1]), "ARRAY\n%s %s", b >= 0 ? bands[b].name : "", kinds[cfg.kind].name);
            break;
        }
        case 2: snprintf(s[2], sizeof(s[2]), "METHOD\n%s", methods[cfg.method]); break;
        case 3:
            if (cal_every[cfg.cal]) snprintf(s[3], sizeof(s[3]), "CAL\nNOW %dS", cal_every[cfg.cal]);
            else snprintf(s[3], sizeof(s[3]), "CAL\nNOW");
            break;
        default: snprintf(s[4], sizeof(s[4]), "SET"); break;
    }
    return s[i];
}

static void show_keys(void) {
    if (!built) return;
    for (int i = 0; i < 5; i++) {
        char        t[48];
        const char *l = f_label(i);

        snprintf(t, sizeof(t), "F%d %s", i + 1, l);
        lv_label_set_text(key_l[i], t);
        lv_obj_set_style_pad_top(key_l[i], strchr(t, '\n') ? 2 : 12, 0);
        buttons_refresh(&btn[i]);
    }
}

static void help_show(bool on) {
    help_on = on;
    if (on) lv_obj_clear_flag(help_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(help_l, LV_OBJ_FLAG_HIDDEN);
}

static void target_move(int32_t d) {
    int32_t lim = (int32_t)(rates[cfg.rate] / 2) - (int32_t)bws[cfg.bw] / 2 - (int32_t)(rates[cfg.rate] / 20);

    follow_off();
    cfg.autot = false;
    if (llabs((int64_t)cfg.off + d) > lim) {        /* past the passband's edge: the Kraken goes with it */
        tune_to((int64_t)target_hz() + d);
    } else {
        cfg.off += d;
        cfg.band = band_of_target();
        send_target();
    }
    show_keys();
}

static void fkey(int k) {
    if (help_on) {
        help_show(false);
        return;
    }
    switch (k) {
        case 0:
            if (pan == P_TUNE) pan_close();
            else pan_open(P_TUNE);
            break;
        case 1:
            if (pan == P_ARRAY) pan_close();
            else pan_open(P_ARRAY);
            break;
        case 2: cfg.method = (cfg.method + 1) % N_METHODS; send_method(); break;
        case 3: df_rx_cmd("c"); msg("CALIBRATING: THE NOISE SOURCE ON FOR A MOMENT, THE ANTENNAS OFF"); break;
        default:
            if (pan == P_SET) pan_close();
            else pan_open(P_SET);
            break;
    }
    cfg_save();
    show_keys();
    refresh();
}

static void key_tap_cb(lv_event_t *e) { fkey((int)(intptr_t)lv_event_get_user_data(e)); }

static void step_channel(int d) {
    int b = band_of_target();
    target_move(d * (int32_t)(b >= 0 ? bands[b].step : bws[cfg.bw]));
    refresh();
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (help_on) {
        help_show(false);
        return;
    }
    if (pan != P_NONE) {
        switch (key) {
            case LV_KEY_LEFT:
            case LV_KEY_UP: pan_sel = (pan_sel + pan_rows() - 1) % pan_rows(); pan_fill(); return;
            case LV_KEY_RIGHT:
            case LV_KEY_DOWN: pan_sel = (pan_sel + 1) % pan_rows(); pan_fill(); return;
            case LV_KEY_ENTER: pan_push(); return;
            case LV_KEY_ESC: pan_close(); show_keys(); return;
            default: break;
        }
    }
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP: step_channel(-1); break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN: step_channel(1); break;
        case LV_KEY_ESC: dialog_destruct(); break;
        /* the MFK's inner ring: I is after H (AIRSPACE), before A */
        case KEY_MFK_NEXT: app_sentry_open_at('A'); break;
        case KEY_MFK_PREV: app_sentry_open_at('H'); break;
        default: break;
    }
}

/* The MFK's push: the chosen row of a panel */
static void click_cb(lv_event_t *e) {
    (void)e;
    if (pan != P_NONE) pan_push();
}

/* The VFO: in a panel the chosen row's value (a step each two clicks), else the target, 1 kHz a click */
static void rotary_cb(int32_t diff) {
    if (help_on) return;
    if (pan != P_NONE) {
        rot_acc += diff;
        if (rot_acc >= 2 || rot_acc <= -2) {
            pan_change(rot_acc > 0 ? 1 : -1);
            rot_acc = 0;
        }
        return;
    }
    target_move(diff * 1000);
    refresh();
}

/* The BAND keys: the band presets, as on every other screen (they moved the Kraken half a passband before) */
static void band_step(int d) {
    int b = band_of_target();

    follow_off();
    if (b < 0) {                                    /* off the presets: the next one up or down from here */
        uint32_t t = target_hz();

        for (b = d > 0 ? 0 : N_BANDS - 1; b >= 0 && b < N_BANDS; b += d)
            if (d > 0 ? bands[b].hz > t : bands[b].hz < t) break;
        if (b < 0 || b >= N_BANDS) b = d > 0 ? 0 : N_BANDS - 1;
    } else {
        b = (b + d + N_BANDS) % N_BANDS;
    }
    band_set(b);
    cfg_save();
    show_keys();
    refresh();
}

static bool front_key(int key, int state) {
    if (key == KEYPAD_F1 && !help_on) {                 /* F1: a tap TUNE, held AUTO */
        if (state == KEYPAD_RELEASE) fkey(0);
        else if (state == KEYPAD_LONG) {
            cfg.autot = !cfg.autot;
            msg(cfg.autot ? "TARGET AUTO: THE STRONGEST SIGNAL IN THE PASSBAND" : "TARGET MANUAL: THE VFO MOVES IT");
            cfg_save();
            show_keys();
        }
        return true;
    }
    if (key == KEYPAD_F4 && !help_on) {                 /* F4: a tap calibrates, held: how often */
        if (state == KEYPAD_RELEASE) fkey(3);
        else if (state == KEYPAD_LONG) {
            cfg.cal = (cfg.cal + 1) % N_CALS;
            send_cal();
            cfg_save();
            show_keys();
        }
        return true;
    }
    if (key == KEYPAD_BAND_UP || key == KEYPAD_BAND_DOWN) {
        if (state == KEYPAD_RELEASE && !help_on) band_step(key == KEYPAD_BAND_UP ? 1 : -1);
        return true;
    }
    if (key == KEYPAD_GEN) {
        if (state == KEYPAD_RELEASE) help_show(!help_on);
        return true;
    }
    if (key == KEYPAD_APP && (state == KEYPAD_LONG || state == KEYPAD_LONG_RELEASE)) {
        if (state == KEYPAD_LONG) app_sentry_open_pool();
        return true;
    }
    if (help_on && state == KEYPAD_RELEASE) {
        help_show(false);
        return true;
    }
    return false;
}

static void panel_row_tap_cb(lv_event_t *e) {
    int r = (int)(intptr_t)lv_event_get_user_data(e);

    if (r == pan_sel) pan_push();
    else {
        pan_sel = r;
        pan_fill();
    }
}

/* ---- Every 200 ms ---- */

/* AUTO: the strongest signal in the passband (clear of the sticks' DC and the edges), moved to only when it stands
 * 3 dB over the one held, and once a second at most */
static void auto_target(void) {
    if (!cfg.autot || !sn.have_sp || !sn.sp.n || lv_tick_get() - auto_ms < 1000) return;
    auto_ms = lv_tick_get();
    int    n = sn.sp.n, best = -1, cur = (int)((cfg.off / (double)sn.sp.rate + 0.5) * n);
    double bin = sn.sp.rate / (double)n;

    for (int i = n / 20; i < n - n / 20; i++) {
        if (fabs((i - n / 2) * bin) < 15000) continue;
        if (best < 0 || sn.sp.db[i] > sn.sp.db[best]) best = i;
    }
    if (best < 0 || sn.sp.db[best] / 2 - 20 < cfg.sql_ddb / 10) return;
    int curv = cur >= 0 && cur < n ? sn.sp.db[cur] : 0;
    if (sn.sp.db[best] < curv + 6 && abs(best - cur) * bin < bws[cfg.bw]) return;
    if (sn.sp.db[best] < curv + 6 && curv / 2 - 20 >= cfg.sql_ddb / 10) return;
    cfg.off = (int32_t)lround((best - n / 2) * bin);
    clamp_target();
    cfg.band = band_of_target();
    send_target();
}

/* A program on the IQ server tuned the Kraken (its control's FREQ, x6x00-df's DFST iq_clients bit 1): page I's
 * settings take where it is, so the screen, the web page and that program agree; FOLLOW stops */
static void iq_adopt(void) {
    if (!sn.connected || !sn.have_st || !(sn.st.iq_clients & 2) || lv_tick_get() - own_tune_ms < 2000) return;
    if (cfg.fc == (int32_t)sn.st.fc_hz && cfg.off == sn.st.f_off_hz) return;
    cfg.fc     = (int32_t)sn.st.fc_hz;
    cfg.off    = sn.st.f_off_hz;
    cfg.band   = band_of_target();
    cfg.follow = 0;
    cfg.autot  = false;
    cfg_save();
    show_keys();
}

static void refresh(void) {
    if (!built) return;
    df_rx_snapshot(&sn);
    iq_adopt();
    if (sn.have_do && sn.dd.seq != last_do) {       /* a new good bearing to the rim's dots */
        last_do = sn.dd.seq;
        if (sn.dd.gate && sn.dd.bearing_ddeg >= 0) {
            if (n_trail == TRAIL) memmove(trail, trail + 1, sizeof(trail[0]) * (TRAIL - 1)), n_trail--;
            trail[n_trail].brg  = sn.dd.bearing_ddeg / 10.0;
            trail[n_trail++].ms = lv_tick_get();
        }
    }
    auto_target();
    draw_scope();
    show_centre();
    show_target();
    show_array();
    show_ants();
    show_spec();
    show_head();
    if (msg_until && (int32_t)(lv_tick_get() - msg_until) > 0) {
        msg_until = 0;
        lv_obj_add_flag(msg_l, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(scope_img);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    if (!dialog.run) return;
    follow_tick();
    refresh();
}

/* ---- Building ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, int x, int y, int w, lv_text_align_t al,
                       const char *s) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, al, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    }
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, int border, bool filled) {
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_border_color(o, amb, 0);
    lv_obj_set_style_bg_color(o, filled ? amb : ink, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *box(int x, int y, int w, int h, const char *cap) {
    rect(root, x, y, w, h, 3, false);
    lv_obj_t *l = label(root, &pixel_20, amb, x + 3, y + 3, w - 6, LV_TEXT_ALIGN_CENTER, cap);
    rect(root, x, y + 25, w, 3, 0, true);
    return l;
}

static lv_obj_t *img(lv_img_dsc_t *d, int x, int y) {
    lv_obj_t *o = lv_img_create(root);

    lv_img_set_src(o, d);
    lv_obj_set_pos(o, x, y);
    return o;
}

static void build(void) {
    /* the head, as the Sentry's */
    rect(root, 6, 3, SW - 12, HEAD_H, 3, false);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = rect(root, i ? SW - 50 : 12, 7, 36, 36, 3, false);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        badge_l[i] = label(root, &pixel_28, amb, i ? SW - 50 : 12, 10, 36, LV_TEXT_ALIGN_CENTER, "I");
    }
    char t[96];
    snprintf(t, sizeof(t), "XIEGU X6200 DIRECTION FINDER SENTRY: %s", app_sentry_release());
    title_l  = label(root, &pixel_20, amb, 60, 6, SW - 120, LV_TEXT_ALIGN_CENTER, t);
    status_l = label(root, &pixel_16, amb, 60, 27, SW - 120, LV_TEXT_ALIGN_CENTER, "");

    /* the compass */
    rect(root, SC_X - 3, BODY_Y, SC + 6, SC + 6, 3, false);
    scope     = lv_img_buf_alloc(SC, SC, LV_IMG_CF_TRUE_COLOR);
    scope_img = img(scope, SC_X, SC_Y);
    up_l      = label(root, &pixel_16, amb, SC_X + SC / 2 - 20, SC_Y - 1, 40, LV_TEXT_ALIGN_CENTER, "FWD");
    for (int k = 0; k < DF_PROTO_MAXCH; k++) {
        char n[4] = {(char)('0' + k), 0};
        ant_l[k] = label(root, &pixel_16, ink, 0, 0, 18, LV_TEXT_ALIGN_CENTER, n);
        lv_obj_add_flag(ant_l[k], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < N_TG; i++) {
        char n[4] = {(char)('2' + i), 0};

        tgn_l[i] = label(root, &pixel_16, lv_color_hex(tg_col[i]), 0, 0, 18, LV_TEXT_ALIGN_CENTER, n);
        lv_obj_add_flag(tgn_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    centre_l = label(root, &pixel_20, amb, SC_X + 30, SC_Y + SC / 2 + 40, SC - 60, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_style_bg_color(centre_l, ink, 0);
    lv_obj_set_style_bg_opa(centre_l, LV_OPA_COVER, 0);
    lv_label_set_long_mode(centre_l, LV_LABEL_LONG_WRAP);

    /* the target */
    box(RX0, BODY_Y, RW, 96, "TARGET AND BEARING");
    for (int i = 0; i < 3; i++) {
        tgt_l[i] = label(root, &pixel_20, amb, RX0 + 6, BODY_Y + 30 + i * 21, RW - 12, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(tgt_l[i], true);
    }
    /* the array against the band */
    arr_cap_l = box(RX0, BODY_Y + 100, RW, 70, "ARRAY");
    for (int i = 0; i < 2; i++) {
        arr_l[i] = label(root, &pixel_16, amb, RX0 + 6, BODY_Y + 130 + i * 18, RW - 12, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(arr_l[i], true);
    }
    /* the antennas */
    box(RX0, BODY_Y + 174, RW, 30 + NANT * ROW_H + 4, "ANTENNAS: FILLED = IN THE BEARING");
    bars = lv_img_buf_alloc(BAR_W, NANT * ROW_H, LV_IMG_CF_TRUE_COLOR);
    bars_img = img(bars, RX0 + 128, BODY_Y + 204);
    for (int r = 0; r < NANT; r++) {
        row_l[r] = label(root, &pixel_16, amb, RX0 + 6, BODY_Y + 204 + r * ROW_H, 120, LV_TEXT_ALIGN_LEFT, "");
        row_r[r] = label(root, &pixel_16, amb, RX0 + 132 + BAR_W, BODY_Y + 204 + r * ROW_H, RW - 138 - BAR_W,
                         LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(row_l[r], true);
        lv_label_set_recolor(row_r[r], true);
    }
    /* CH0's spectrum over the bearings' waterfall */
    int sy = BODY_Y + 174 + 30 + NANT * ROW_H + 8;
    rect(root, RX0, sy, RW, SC + 6 - (sy - BODY_Y), 3, false);
    spec     = lv_img_buf_alloc(SPEC_W, SPEC_H, LV_IMG_CF_TRUE_COLOR);
    spec_img = img(spec, RX0 + 6, sy + 4);
    wf       = lv_img_buf_alloc(SPEC_W, WF_H, LV_IMG_CF_TRUE_COLOR);
    wf_img   = img(wf, RX0 + 6, sy + 6 + SPEC_H);

    /* what the knobs do, and the F keys */
    int ky = 480 - KEYS_H - 3;
    legend_l = label(root, &pixel_16, dim, 6, ky - LEGEND_H, SW - 12, LV_TEXT_ALIGN_CENTER,
                     "VFO: TARGET FINE | MFK: CHANNEL | BAND KEYS: BAND | RING: SCREEN | F1 HELD: AUTO | GEN: HELP");
    int w = (SW - 12 - 4 * 6) / 5;
    for (int i = 0; i < 5; i++) {
        lv_obj_t *k = lv_label_create(root);
        key_l[i]    = k;
        lv_obj_set_pos(k, 6 + i * (w + 6), ky);
        lv_obj_set_size(k, w, KEYS_H);
        lv_obj_set_style_text_font(k, &pixel_20, 0);
        lv_obj_set_style_text_color(k, amb, 0);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_border_width(k, 2, 0);
        lv_obj_set_style_border_color(k, amb, 0);
        lv_obj_set_style_bg_color(k, amb, 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(k, ink, LV_STATE_PRESSED);
        lv_label_set_long_mode(k, LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(k, key_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    msg_l = label(root, &pixel_20, amb, 60, SC_Y + 8, SW - 120, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_style_bg_color(msg_l, ink, 0);
    lv_obj_set_style_bg_opa(msg_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(msg_l, 2, 0);
    lv_obj_set_style_border_color(msg_l, amb, 0);
    lv_obj_set_style_pad_all(msg_l, 6, 0);
    lv_obj_add_flag(msg_l, LV_OBJ_FLAG_HIDDEN);

    panel = rect(root, (SW - PANEL_W) / 2, 60, PANEL_W, 200, 3, false);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    panel_head_l = label(panel, &pixel_20, amb, 12, 12, PANEL_W - 30, LV_TEXT_ALIGN_LEFT, "");
    for (int r = 0; r < PANEL_ROWS; r++) {
        lv_obj_t *l = label(panel, &pixel_20, amb, 9, 50 + r * 28, PANEL_W - 24, LV_TEXT_ALIGN_LEFT, "");

        lv_obj_set_style_bg_color(l, amb, 0);
        lv_obj_set_style_pad_ver(l, 2, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, panel_row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        panel_row_l[r] = l;
    }
    panel_foot_l = label(panel, &pixel_20, amb, 12, 0, PANEL_W - 30, LV_TEXT_ALIGN_LEFT,
                         "MFK: A ROW   VFO: CHANGE IT   PUSH: DO IT / CLOSE");
    sk     = lv_img_buf_alloc(SK_W, SK_H, LV_IMG_CF_TRUE_COLOR);
    sk_img = lv_img_create(panel);
    lv_img_set_src(sk_img, sk);
    lv_obj_set_pos(sk_img, 12, 0);
    sk_t = label(panel, &pixel_16, amb, 12 + SK_W + 14, 0, PANEL_W - 30 - SK_W - 14, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(sk_t, LV_LABEL_LONG_WRAP);
    for (int k = 0; k < NANT; k++) {
        char d[4] = {(char)('0' + k), 0};

        sk_l[k] = label(panel, &pixel_16, ink, 0, 0, 18, LV_TEXT_ALIGN_CENTER, d);
    }
    sketch_show(false);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    help_l = label(root, &pixel_20, amb, 30, 56, SW - 60, LV_TEXT_ALIGN_LEFT,
                   "PAGE I: DIRECTION FINDING WITH A KRAKENSDR (FIVE COHERENT RECEIVERS, SERIALS 1000-1004) ON USB.\n\n"
                   "THE COMPASS: THE ARRAY TO SCALE, ANT 0 FORWARD (UP; NORTH UP WITH A HEADING), THE REST CLOCKWISE "
                   "AS KRAKENRF'S CALCULATOR NUMBERS THEM. AN ANTENNA FILLED IS IN THE BEARING, A RING ALONE IS LEFT OUT. "
                   "GREEN IN, AMBER QUIET, RED LOW OR SLIPPED, MAGENTA CLIPPING, DIM NOT CALIBRATED, OFF OR GONE. ITS BAR: "
                   "THE TARGET OVER ITS FLOOR. THE DASHED CIRCLE IS HALF A WAVELENGTH ACROSS. ROUND IT THE METHOD'S "
                   "SPECTRUM, THE NEEDLE (GREEN FIT 80% UP, AMBER 50-80, RED UNDER) AND ITS +- WEDGE; DOTS: LAST BEARINGS.\n\n"
                   "F1 TUNE: FOLLOW A OR B (THE X6200'S DIAL OR B'S FREQUENCY, AS THEY CHANGE), BAND, FREQUENCY, WIDTH, AUTO "
                   "(HELD: AUTO).  F2 ARRAY: BAND, KIND, SIZE, SIZE FOR THE BAND, HEADING.  F3 METHOD.  F4 CAL NOW (HELD: "
                   "HOW OFTEN).  F5 SET: RATE, GAIN, SQUELCH, ANTENNAS LEFT OUT, TEST SOURCE.  VFO: TARGET 1 KHZ (THE KRAKEN "
                   "FOLLOWS PAST THE EDGE).  MFK: A CHANNEL.  BAND KEYS: THE BAND PRESETS.  RING: H BEFORE, A AFTER.\n\n"
                   "WHILE I IS OPEN THE KRAKEN IS ITS ALONE: AIRSPACE AND WATCH CHANNELS LET GO, B MOVES OR IS LENT. "
                   "RECEIVE ONLY.  ANY KEY: CLOSE.");
    lv_label_set_long_mode(help_l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(help_l, ink, 0);
    lv_obj_set_style_bg_opa(help_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(help_l, 3, 0);
    lv_obj_set_style_border_color(help_l, amb, 0);
    lv_obj_set_style_pad_all(help_l, 12, 0);
    lv_obj_add_flag(help_l, LV_OBJ_FLAG_HIDDEN);
}

static void construct_cb(lv_obj_t *parent) {
    amb     = TC(accent);
    ink     = TC(bg);
    dim     = lv_color_mix(amb, ink, 110);
    faint   = lv_color_mix(amb, ink, 50);
    green   = lv_color_hex(0x7cd65a);
    red     = TC(err);
    magenta = lv_color_hex(0xe066d6);
    c_amb   = argb(amb);
    c_ink   = argb(ink);
    c_dim   = argb(dim);
    c_faint = argb(faint);
    c_green = argb(green);
    c_red   = argb(red);
    c_mag   = argb(magenta);
    for (int i = 0; i < 64; i++) glow[i] = mix(c_amb, c_ink, i * 230 / 63);

    ang = malloc(sizeof(uint16_t) * SC * SC);
    rad = malloc(sizeof(uint16_t) * SC * SC);
    if (ang && rad) {
        double r = SC / 2.0 - 18 + 0.5, cx = SC / 2.0 - 0.5, cy = SC / 2.0 - 0.5;
        for (int y = 0; y < SC; y++)
            for (int x = 0; x < SC; x++) {
                double dx = x - cx, dy = cy - y, d = hypot(dx, dy);
                ang[y * SC + x] = d > r ? 0xffff : (uint16_t)((int)lround(fmod(atan2(dx, dy) * 1800 / M_PI + 3600, 3600)) % 3600);
                rad[y * SC + x] = (uint16_t)lround(d * 4);
            }
    }

    cfg_load();
    cfg_loaded = true;
    root       = lv_obj_create(parent);
    dialog.obj = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, SW, 480);
    lv_obj_set_style_bg_color(root, ink, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    buttons_load_page(&page);
    build();

    lv_group_add_obj(keyboard_group, root);
    lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(root, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);

    help_on = false;
    pan     = P_NONE;
    rot_acc = 0;
    n_trail = 0;
    last_do = 0;
    memset(&sn, 0, sizeof(sn));
    built = true;
    df_use(DF_USER_PAGE, true);         /* the daemon started, or kept as the web page has it */
    clamp_target();
    send_all();
    show_keys();
    if (ang && rad) refresh();
    timer = lv_timer_create(timer_cb, 200, NULL);
}

static void destruct_cb(void) {
    if (timer) lv_timer_del(timer);
    timer = NULL;
    built = false;
    cfg_save();
    df_use(DF_USER_PAGE, false);        /* the daemon ended (the Kraken everyone's again, B back), unless the web page
                                         * still has it */
    rtl_spectrum_mute(false);           /* the Sentry's OFF let go: what opens next sets the speaker (as AIRSPACE) */
    lv_group_remove_obj(root);
    lv_img_dsc_t **d[] = {&scope, &bars, &spec, &wf};
    for (size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++)
        if (*d[i]) {
            lv_img_cache_invalidate_src(*d[i]);
            lv_img_buf_free(*d[i]);
            *d[i] = NULL;
        }
    free(ang);
    free(rad);
    ang  = NULL;
    rad  = NULL;
    root = NULL;
}

/* ---- The web page: the web configurator's Direction finder page, through cfg_socket.cpp (df_get, df_set, df_run) ----
 * The web page holds the Kraken as page I does (df_use): started by df_run, kept while df_get keeps coming (the page
 * asks a few times a second), let go WEB_IDLE_MS after the last one. Its settings are page I's own (gui_state df.*),
 * so the screen and the browser always agree. df_set and df_run run on the LVGL thread (the scheduler); df_get on the
 * socket's thread, from the snapshot (its own lock) and the settings (whole ints, written on the LVGL thread) */

#define WEB_IDLE_MS 20000

static lv_timer_t  *bg_timer;
static atomic_uint  web_last_ms;

static void web_load_cb(void *arg) {
    (void)arg;
    if (!cfg_loaded) cfg_load(), cfg_loaded = true;
}


static void bg_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!built) {                           /* page I's own timer does these while it is open */
        df_rx_snapshot(&sn);
        iq_adopt();
        auto_target();
        follow_tick();
    }
    if (mono_ms() - atomic_load(&web_last_ms) > WEB_IDLE_MS) {
        lv_timer_del(bg_timer);
        bg_timer = NULL;
        df_use(DF_USER_WEB, false);
    }
}

void app_df_web_run(bool on) {
    atomic_store(&web_last_ms, mono_ms());
    if (on) {
        df_use(DF_USER_WEB, true);
        if (!bg_timer) bg_timer = lv_timer_create(bg_timer_cb, 200, NULL);
    } else {
        if (bg_timer) lv_timer_del(bg_timer);
        bg_timer = NULL;
        df_use(DF_USER_WEB, false);
    }
}

static int idx_ok(double v, int n) {
    int i = (int)lround(v);
    return i >= 0 && i < n ? i : -1;
}

int app_df_web_set(const char *k, double v) {
    int i = (int)lround(v);

    if (!cfg_loaded) cfg_load(), cfg_loaded = true;
    atomic_store(&web_last_ms, mono_ms());
    if (!strcmp(k, "target")) {
        follow_off();
        cfg.autot = false;
        if (!tune_to((int64_t)llround(v))) return -1;
    } else if (!strcmp(k, "band")) {
        if (idx_ok(v, N_BANDS) < 0) return -1;
        follow_off();
        band_set(i);
    } else if (!strcmp(k, "follow")) {
        if (i < 0 || i > 2) return -1;
        cfg.follow = i;
    } else if (!strcmp(k, "bw")) {
        if (idx_ok(v, N_BWS) < 0) return -1;
        cfg.bw = i;
        clamp_target();
        send_target();
    } else if (!strcmp(k, "auto")) {
        cfg.autot = i != 0;
    } else if (!strcmp(k, "method")) {
        if (idx_ok(v, N_METHODS) < 0) return -1;
        cfg.method = i;
        send_method();
    } else if (!strcmp(k, "rate")) {
        if (idx_ok(v, N_RATES) < 0) return -1;
        cfg.rate = i;
        clamp_target();
        send_rate();
        send_target();
    } else if (!strcmp(k, "gain")) {
        if (idx_ok(v, N_GAINS) < 0) return -1;
        cfg.gain = i;
        send_gain();
    } else if (!strcmp(k, "sql")) {                     /* dB over the floor */
        if (v < 3 || v > 40) return -1;
        cfg.sql_ddb = (int)lround(v * 10);
        send_sql();
    } else if (!strcmp(k, "cal")) {
        if (idx_ok(v, N_CALS) < 0) return -1;
        cfg.cal = i;
        send_cal();
    } else if (!strcmp(k, "kind")) {
        if (idx_ok(v, N_KINDS) < 0) return -1;
        if (i == K_CUSTOM && !cust_load()) return -1;           /* no layout file */
        cfg.kind = i;
        send_array();
    } else if (!strcmp(k, "custom")) {                  /* the web page wrote df_array.txt: read it, use it */
        if (!cust_load()) return -1;
        cfg.kind = K_CUSTOM;
        send_array();
    } else if (!strcmp(k, "size_mm")) {
        if (i < 10 || i > 5000) return -1;
        cfg.size_mm = i;
        send_array();
    } else if (!strcmp(k, "heading")) {
        cfg.heading = ((i % 360) + 360) % 360;
        send_log();
    } else if (!strcmp(k, "log")) {
        cfg.log = i != 0;
        send_log();
    } else if (!strcmp(k, "mask")) {
        if (i < 0 || i > 0xff) return -1;
        cfg.off_mask = i;
        send_mask();
    } else if (!strcmp(k, "nsrc")) {
        if (i < 0 || i > 3) return -1;
        cfg.nsrc = i;
        send_nsrc();
    } else if (!strcmp(k, "fb")) {
        cfg.fb = i != 0;
        send_fb();
    } else if (!strcmp(k, "smooth")) {
        if (i != 0 && i != 2 && i != 3) return -1;
        cfg.smooth = i;
        send_smooth();
    } else if (!strncmp(k, "tg", 2) && k[2] >= '2' && k[2] < '2' + N_TG && !k[3]) {   /* tg2-tg4: Hz, 0 clears */
        int     t = k[2] - '2';
        int64_t hz = llround(v);

        for (int o = 0; o < N_TG && hz; o++)
            if (o != t && cfg.tg_hz[o] == (uint32_t)hz) return -1;
        if (!tg_set_hz(t, hz)) return -1;
    } else if (!strcmp(k, "cal_now")) {
        df_rx_cmd("c");
    } else {
        return -1;
    }
    cfg_save();
    show_keys();
    pan_fill();
    refresh();
    return 0;
}

/* JSON, appended with the room left */
typedef struct {
    char  *p;
    size_t n, l;
} jbuf_t;

static void jb(jbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb(jbuf_t *b, const char *fmt, ...) {
    va_list ap;

    if (b->l + 1 >= b->n) return;
    va_start(ap, fmt);
    int r = vsnprintf(b->p + b->l, b->n - b->l, fmt, ap);
    va_end(ap);
    if (r > 0) b->l = b->l + (size_t)r < b->n ? b->l + (size_t)r : b->n - 1;
}

static void jstr(jbuf_t *b, const char *s, size_t max) {
    jb(b, "\"");
    for (size_t i = 0; i < max && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') jb(b, "\\%c", c);
        else if (c >= 32 && c < 127) jb(b, "%c", c);
    }
    jb(b, "\"");
}

int app_df_web_json(char *out, size_t n) {
    jbuf_t    b = {out, n, 0};
    df_snap_t s;
    int       bnd = band_of_target();

    if (!cfg_loaded) {                  /* page I's settings read on the LVGL thread; the next df_get has them */
        scheduler_put_noargs(web_load_cb);
        jb(&b, "{\"t\":\"df\",\"loaded\":0}\n");
        return (int)b.l;
    }
    atomic_store(&web_last_ms, mono_ms());
    df_rx_snapshot(&s);
    jb(&b, "{\"t\":\"df\",\"loaded\":1,\"present\":%d,\"run\":%d,\"page\":%d,\"web\":%d,\"connected\":%d,\"lent\":%d,"
           "\"iq\":{\"client\":%d,\"tuned\":%d},",
       df_rx_present(), df_users != 0, (df_users & DF_USER_PAGE) != 0, (df_users & DF_USER_WEB) != 0, s.connected,
       df_rx_b_lent(), s.have_st && (s.st.iq_clients & 1), s.have_st && (s.st.iq_clients & 2) ? 1 : 0);
    jb(&b, "\"cfg\":{\"fc\":%d,\"off\":%d,\"target\":%u,\"bw\":%d,\"band\":%d,\"follow\":%d,\"auto\":%d,\"kind\":%d,"
           "\"size_mm\":%d,\"heading\":%d,\"method\":%d,\"rate\":%d,\"gain\":%d,\"sql\":%.1f,\"cal\":%d,\"mask\":%d,"
           "\"nsrc\":%d,\"fb\":%d,\"smooth\":%d,\"log\":%d,\"heading_now\":%d,\"heading_from\":",
       cfg.fc, cfg.off, target_hz(), cfg.bw, bnd, cfg.follow, cfg.autot, cfg.kind, cfg.size_mm, cfg.heading, cfg.method,
       cfg.rate, cfg.gain, cfg.sql_ddb / 10.0, cfg.cal, cfg.off_mask, cfg.nsrc, cfg.fb, cfg.smooth, cfg.log, heading_now());
    jstr(&b, heading_from(), 16);
    jb(&b, ",\"xy\":[");                    /* the antennas where they are, mm (x forward, y left) */
    for (int m = 0; m < kind_n(); m++) {
        double x, y;
        elem(m, &x, &y);
        jb(&b, "%s[%ld,%ld]", m ? "," : "", lround(x * 1000), lround(y * 1000));
    }
    jb(&b, "]");
    jb(&b, ",\"tg\":[");                    /* the other targets: Hz 0 none; in the passband; the daemon's newest */
    for (int i = 0; i < N_TG; i++) {
        int32_t        off;
        const df_tg_t *t = s.connected ? tg_now_in(&s, i) : NULL;

        jb(&b, "%s{\"hz\":%u,\"bw\":%u,\"in\":%d", i ? "," : "", cfg.tg_hz[i], bws[cfg.tg_bw[i]], tg_off(i, &off));
        if (t)
            jb(&b, ",\"gate\":%d,\"bearing\":%.1f,\"mirror\":%.1f,\"uncert\":%.1f,\"fit\":%d,\"level\":%.1f,\"age\":%.1f,"
                   "\"done\":%d", t->gate, t->bearing_ddeg / 10.0, t->mirror_ddeg / 10.0, t->uncert_ddeg / 10.0, t->fit_pct,
               t->level_ddb / 10.0, t->age_ds == 0xffff ? -1.0 : t->age_ds / 10.0, t->done_pct);
        jb(&b, "}");
    }
    jb(&b, "]");
    jb(&b, "},\"lists\":{\"bands\":[");
    for (int i = 0; i < N_BANDS; i++) jb(&b, "%s[\"%s\",%u,%u]", i ? "," : "", bands[i].name, bands[i].hz, bands[i].step);
    jb(&b, "],\"kinds\":[");
    for (int i = 0; i < N_KINDS; i++)
        jb(&b, "%s[\"%s\",%d,%d]", i ? "," : "", kinds[i].name, kinds[i].ula, i == K_CUSTOM ? cust_n : kinds[i].n);
    jb(&b, "],\"methods\":[");
    for (int i = 0; i < N_METHODS; i++) jb(&b, "%s\"%s\"", i ? "," : "", methods[i]);
    jb(&b, "],\"rates\":[");
    for (int i = 0; i < N_RATES; i++) jb(&b, "%s%u", i ? "," : "", rates[i]);
    jb(&b, "],\"bws\":[");
    for (int i = 0; i < N_BWS; i++) jb(&b, "%s%u", i ? "," : "", bws[i]);
    jb(&b, "],\"gains\":[");
    for (int i = 0; i < N_GAINS; i++) jb(&b, "%s%.1f", i ? "," : "", gains[i] / 10.0);
    jb(&b, "],\"cals\":[");
    for (int i = 0; i < N_CALS; i++) jb(&b, "%s%d", i ? "," : "", cal_every[i]);
    jb(&b, "]}");
    if (s.have_st) {
        jb(&b, ",\"st\":{\"state\":\"%s\",\"nch\":%d,\"synth\":%d,\"cal_age\":%d,\"cals\":%u,\"cal_fails\":%u,"
               "\"slips\":%u,\"overruns\":%u,\"cpu\":%d,\"fc\":%u,\"rate\":%u,\"why\":",
           state_name(s.st.state), s.st.nch, s.st.synth, s.st.cal_age_s == 0xffff ? -1 : s.st.cal_age_s, s.st.cals,
           s.st.cal_fails, s.st.slips, s.st.overruns, s.st.cpu_pct, s.st.fc_hz, s.st.rate);
        jstr(&b, s.st.why, sizeof(s.st.why));
        jb(&b, ",\"ch\":[");
        for (int m = 0; m < s.st.nch && m < DF_PROTO_MAXCH; m++) {
            jb(&b, "%s{\"serial\":", m ? "," : "");
            jstr(&b, s.st.ch[m].serial, sizeof(s.st.ch[m].serial));
            jb(&b, ",\"cond\":\"%s\",\"used\":%d,\"phase\":%.1f,\"gain_db\":%.2f,\"margin\":%.1f,\"resid\":%.2f,"
                   "\"noise\":%.1f}",
               cond_name(s.st.ch[m].cond), s.st.ch[m].used, s.st.ch[m].cal_phase_ddeg / 10.0,
               s.st.ch[m].cal_gain_mdb / 100.0, s.st.ch[m].cal_margin_ddb / 10.0, s.st.ch[m].cal_resid_cdeg / 100.0,
               s.st.ch[m].noise_ddbfs / 10.0);
        }
        jb(&b, "]}");
    }
    if (s.have_do) {
        jb(&b, ",\"do\":{\"seq\":%u,\"gate\":%d,\"bearing\":%.1f,\"mirror\":%.1f,\"uncert\":%.1f,\"fit\":%d,"
               "\"level\":%.1f,\"snr\":%.1f,\"age\":%.1f,\"used\":%d,\"bad_ch\":%d,\"nsrc\":%d,\"peaks\":[",
           s.dd.seq, s.dd.gate, s.dd.bearing_ddeg / 10.0, s.dd.mirror_ddeg / 10.0, s.dd.uncert_ddeg / 10.0, s.dd.fit_pct,
           s.dd.level_ddb / 10.0, s.dd.snr_ddb / 10.0, s.dd.age_ds == 0xffff ? -1 : s.dd.age_ds / 10.0, s.dd.used_mask,
           s.dd.bad_ch, s.dd.nsrc ? s.dd.nsrc : 1);
        for (int i = 0; i < s.dd.npeaks && i < 3; i++) jb(&b, "%s%.1f", i ? "," : "", s.dd.peaks_ddeg[i] / 10.0);
        jb(&b, "],\"ch\":[");
        for (int m = 0; m < DF_PROTO_MAXCH && (!s.have_st || m < s.st.nch); m++)
            jb(&b, "%s%.1f", m ? "," : "", s.dd.ch_ddb[m] / 10.0);
        jb(&b, "],\"p\":[");
        for (int g = 0; g < 360; g++) jb(&b, "%s%u", g ? "," : "", s.dd.p[g]);
        jb(&b, "]}");
    }
    if (s.have_sp && s.sp.n) {
        jb(&b, ",\"sp\":{\"fc\":%u,\"rate\":%u,\"floor\":%.1f,\"db\":[", s.sp.fc_hz, s.sp.rate, s.sp.floor_ddbfs / 10.0);
        for (int i = 0; i < s.sp.n && i < DF_PROTO_SPEC; i++) jb(&b, "%s%u", i ? "," : "", s.sp.db[i]);
        jb(&b, "]}");
    }
    if (s.have_gp)
        jb(&b, ",\"gp\":{\"gpsd\":%d,\"mode\":%d,\"src\":%d,\"heading\":%.1f,\"speed\":%.2f,\"lat\":%.7f,\"lon\":%.7f}",
           s.gp.gpsd, s.gp.mode, s.gp.head_src, s.gp.head_ddeg / 10.0, s.gp.speed_cms == 0xffff ? -1 : s.gp.speed_cms / 100.0,
           s.gp.lat_e7 / 1e7, s.gp.lon_e7 / 1e7);
    jb(&b, "}\n");
    return b.l + 1 < n ? (int)b.l : -1;
}

