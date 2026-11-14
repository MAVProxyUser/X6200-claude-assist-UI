/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - AIRSPACE, the Sentry's look on 1090 MHz: the aircraft an ADS-B receiver on USB hears (a GNS 5890 or
 *  a microADSB: adsb_rx.c, decoded by adsb.c), drawn on a scope around the station's QTH locator.
 *
 *  - The scope: north up, four range rings, a sweep turning every 4 s. A live aircraft (heard in the last minute)
 *    is an arrow along its track with its call and flight level; each message it sends pings a ring around it.
 *  - History: the track each aircraft flew, fading as it ages, and the aircraft no longer heard as ghosts. DECAY
 *    (F2) is how long both stay: OFF (live only), 1 to 60 minutes, or HOLD (till CLEAR); 5 minutes by default.
 *  - The list: live aircraft nearest first, then the ghosts, newest first. MFK chooses one: brackets on the scope,
 *    all it said in the box under the list.
 *  - The signals: messages a second over the last two minutes.
 *  Listening goes on when the screen closes, so the history covers the time away. Receive only: it does not touch
 *  the radio. Without a QTH locator (Settings > General) the scope centres on the traffic.
 */

#include "app_adsb.h"

#include "adsb.h"
#include "adsb_rx.h"
#include "rtl_spectrum.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "gps.h"
#include "gui_events.h"
#include "keyboard.h"
#include "main_screen.h"
#include "params/params.h"
#include "qth/qth.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const lv_font_t pixel_16, pixel_20, pixel_28;

#define SW        800
#define HEAD_H    44
#define BODY_Y    50
#define KEYS_H    46
#define LEGEND_H  20
#define SC        348           /* the scope, square */
#define SC_X      9
#define SC_Y      (BODY_Y + 3)
#define RX0       366           /* the right-hand column */
#define RW        (SW - 6 - RX0)
#define ROWS      8
#define LABELS    20
#define SPARK_W   416
#define SPARK_H   22
#define SPARK_N   104           /* seconds, 4 px each */
#define SWEEP_S   4.0

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static bool front_key(int key, int state);
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
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .rotary_cb    = rotary_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
    .front_key_cb = front_key,
};

dialog_t *dialog_adsb = &dialog;

/* The settings: the scope's range, how long history stays */
static const int         ranges[] = {5, 10, 25, 50, 100, 150, 250};
static const int         decays[] = {0, 60, 120, 300, 600, 1800, 3600, -1};    /* s; 0 OFF, -1 HOLD */
static const char *const decay_names[] = {"OFF", "1 MIN", "2 MIN", "5 MIN", "10 MIN", "30 MIN", "60 MIN", "HOLD"};
#define N_RANGES (int)(sizeof(ranges) / sizeof(ranges[0]))
#define N_DECAYS (int)(sizeof(decays) / sizeof(decays[0]))

static lv_color_t   amb, ink, dim, faint;
static uint32_t     c_amb, c_ink, c_dim, c_faint, glow[64];
static lv_obj_t    *root, *title_l, *status_l, *badge_l[2], *scope_img, *spark_img, *centre_l, *north_l, *ring_l[4];
static lv_obj_t    *lbl[LABELS], *list_head_l, *row_l[ROWS], *det_l[3], *sig_l, *legend_l, *key_l[5], *help_l;
static lv_img_dsc_t *scope, *spark;
static uint16_t    *ang;            /* each scope pixel's bearing from the centre, tenths of a degree; 0xffff outside */
static lv_timer_t  *timer;
static uint32_t     sel_icao;
static int          rot_acc;
static bool         help_on;
static double       c_lat, c_lon;   /* the scope's centre */
static bool         c_qth, c_ok;    /* the station's place (GPS or locator); any centre at all */
static char         c_from[40];     /* where the centre comes from, said in the head */

#define FIX_MAX_S 600               /* a GPS or phone position this recent is the centre */

static int range_nm(void) { return ranges[param_i_get(cfg_adsb_range)]; }
static int decay_s(void) { return decays[param_i_get(cfg_adsb_decay)]; }

/* ---- Pixels ---- */

static uint32_t argb(lv_color_t c) { return lv_color_to32(c) | 0xff000000; }

/* fg over bg, a of 255 */
static uint32_t mix(uint32_t fg, uint32_t bg, int a) {
    if (a <= 0) return bg;
    if (a >= 255) return fg;
    uint32_t r = (((fg >> 16) & 0xff) * a + ((bg >> 16) & 0xff) * (255 - a)) / 255;
    uint32_t g = (((fg >> 8) & 0xff) * a + ((bg >> 8) & 0xff) * (255 - a)) / 255;
    uint32_t b = ((fg & 0xff) * a + (bg & 0xff) * (255 - a)) / 255;
    return 0xff000000 | r << 16 | g << 8 | b;
}

static void put(uint32_t *px, int x, int y, uint32_t c) {
    if (x >= 0 && y >= 0 && x < SC && y < SC) px[y * SC + x] = c;
}

static void line(uint32_t *px, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;

    for (int n = 0; n < 2 * SC; n++) {
        put(px, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

/* A circle of points, every step degrees (dashed when step is large) */
static void circle(uint32_t *px, double cx, double cy, double r, double step, uint32_t c) {
    for (double a = 0; a < 360; a += step) put(px, (int)lround(cx + r * sin(a * M_PI / 180)), (int)lround(cy - r * cos(a * M_PI / 180)), c);
}

/* An arrow along trk (degrees) with its tip ahead of (x, y) */
static void arrow(uint32_t *px, double x, double y, double trk, uint32_t c) {
    double s = sin(trk * M_PI / 180), co = cos(trk * M_PI / 180);
    double tx = x + 7 * s, ty = y - 7 * co;                     /* tip */
    double lx = x - 4 * s - 4.5 * co, ly = y + 4 * co - 4.5 * s; /* tail corners */
    double rx = x - 4 * s + 4.5 * co, ry = y + 4 * co + 4.5 * s;
    int    x0 = (int)floor(fmin(tx, fmin(lx, rx))), x1 = (int)ceil(fmax(tx, fmax(lx, rx)));
    int    y0 = (int)floor(fmin(ty, fmin(ly, ry))), y1 = (int)ceil(fmax(ty, fmax(ly, ry)));

    for (int yy = y0; yy <= y1; yy++)
        for (int xx = x0; xx <= x1; xx++) {
            double d1 = (xx - lx) * (ty - ly) - (yy - ly) * (tx - lx);
            double d2 = (xx - tx) * (ry - ty) - (yy - ty) * (rx - tx);
            double d3 = (xx - rx) * (ly - ry) - (yy - ry) * (lx - rx);
            if ((d1 >= 0 && d2 >= 0 && d3 >= 0) || (d1 <= 0 && d2 <= 0 && d3 <= 0)) put(px, xx, yy, c);
        }
}

static void square(uint32_t *px, int x, int y, int h, bool fill, uint32_t c) {
    for (int yy = -h; yy <= h; yy++)
        for (int xx = -h; xx <= h; xx++)
            if (fill || abs(xx) == h || abs(yy) == h) put(px, x + xx, y + yy, c);
}

static void brackets(uint32_t *px, int x, int y, int h, uint32_t c) {
    for (int k = 0; k < 4; k++) {
        int sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1;
        line(px, x + sx * h, y + sy * h, x + sx * (h - 4), y + sy * h, c);
        line(px, x + sx * h, y + sy * h, x + sx * h, y + sy * (h - 4), c);
    }
}

/* ---- Where things are ---- */

static bool proj(double lat, double lon, double *x, double *y) {
    double r = SC / 2.0 - 14, k = r / range_nm();

    *x = SC / 2.0 + (lon - c_lon) * cos(c_lat * M_PI / 180) * 60.0 * k;
    *y = SC / 2.0 - (lat - c_lat) * 60.0 * k;
    return *x >= -20 && *y >= -20 && *x < SC + 20 && *y < SC + 20;
}

/* The centre: a USB GPS (gpsd), else the QTH locator (Settings > General), else the traffic's middle, moved there
 * slowly */
static void find_centre(double now) {
    const char *q = params.qth.x;
    double      lat, lon;
    int         age;

    if (gps_last_fix(&lat, &lon, &age) && age < FIX_MAX_S) {
        c_lat = lat, c_lon = lon, c_ok = c_qth = true;
        snprintf(c_from, sizeof(c_from), "GPS %.4f %.4f", lat, lon);
        adsb_set_ref(true, c_lat, c_lon);
        return;
    }
    c_qth = qth_grid_check(q);
    if (c_qth) {
        qth_str_to_pos(q, &c_lat, &c_lon);
        c_ok = true;
        snprintf(c_from, sizeof(c_from), "QTH %s", q);
        adsb_set_ref(true, c_lat, c_lon);
        return;
    }
    snprintf(c_from, sizeof(c_from), "NO GPS OR QTH: CENTRED ON TRAFFIC");
    adsb_set_ref(false, 0, 0);
    double la = 0, lo = 0;
    int    n  = 0;
    for (int i = 0; i < adsb_count(); i++) {
        const adsb_plane_t *p = adsb_plane(i);
        if (p->pos_ok && now - p->seen <= ADSB_LIVE_S) la += p->lat, lo += p->lon, n++;
    }
    if (!n) return;
    la /= n, lo /= n;
    if (!c_ok) c_lat = la, c_lon = lo, c_ok = true;
    else c_lat += (la - c_lat) * 0.05, c_lon += (lo - c_lon) * 0.05;
}

static bool live(const adsb_plane_t *p, double now) { return now - p->seen <= ADSB_LIVE_S; }

/* 255 fresh, less as it ages towards the decay; ghosts dimmer */
static int fade(double age) {
    int d = decay_s();

    if (d < 0) return age < 600 ? 255 - (int)(age / 600 * 150) : 105;     /* HOLD: never gone, an hour's dim */
    if (d == 0) return 0;
    double a = 1.0 - age / d;
    return a <= 0 ? 0 : (int)(40 + 215 * a);
}

/* The list: live aircraft nearest first, then the ghosts newest first */
static int order(int *idx, double now) {
    int    n = adsb_count();
    double key[ADSB_MAX_PLANES];

    for (int i = 0; i < n; i++) {
        const adsb_plane_t *p = adsb_plane(i);
        idx[i] = i;
        if (live(p, now))
            key[i] = p->pos_ok && c_ok ? adsb_range_nm(c_lat, c_lon, p->lat, p->lon, NULL) : 1e6 - p->seen / 1e6;
        else
            key[i] = 2e6 + (now - p->seen);
    }
    for (int i = 1; i < n; i++)                 /* few: an insertion sort */
        for (int j = i; j > 0 && key[idx[j]] < key[idx[j - 1]]; j--) {
            int t = idx[j];
            idx[j] = idx[j - 1], idx[j - 1] = t;
        }
    return n;
}

/* ---- Drawing ---- */

static void draw_scope(double now, const int *idx, int n) {
    uint32_t *px = (uint32_t *)scope->data;
    double    cx = SC / 2.0, cy = SC / 2.0, R = SC / 2.0 - 14;
    int       sweep = (int)(fmod(now / SWEEP_S, 1.0) * 3600);

    /* the ground and the sweep's afterglow, a quarter turn behind it */
    for (int i = 0; i < SC * SC; i++) {
        uint16_t a = ang[i];
        if (a == 0xffff) {
            px[i] = c_ink;
            continue;
        }
        int d = (sweep - a + 3600) % 3600;
        px[i] = d < 900 ? glow[(900 - d) * 63 / 900] : c_ink;
    }
    for (int k = 1; k <= 4; k++) circle(px, cx, cy, R * k / 4, k == 4 ? 0.25 : 3.0, k == 4 ? c_dim : c_faint);
    for (int a = 0; a < 360; a += 30) {
        double s = sin(a * M_PI / 180), c = cos(a * M_PI / 180);
        line(px, (int)lround(cx + R * s), (int)lround(cy - R * c), (int)lround(cx + (R + 6) * s), (int)lround(cy - (R + 6) * c), c_dim);
    }
    for (int t = -(int)R; t <= (int)R; t += 4) put(px, (int)cx + t, (int)cy, c_faint), put(px, (int)cx, (int)cy + t, c_faint);
    {   /* the sweep */
        double s = sin(sweep * M_PI / 1800), c = cos(sweep * M_PI / 1800);
        line(px, (int)cx, (int)cy, (int)lround(cx + R * s), (int)lround(cy - R * c), mix(c_amb, c_ink, 150));
    }
    /* the station */
    line(px, (int)cx - 5, (int)cy, (int)cx + 5, (int)cy, c_amb);
    line(px, (int)cx, (int)cy - 5, (int)cx, (int)cy + 5, c_amb);
    if (!c_ok) return;

    /* history: each track, older parts dimmer */
    bool trails = param_i_get(cfg_adsb_trails) && decay_s() != 0;
    for (int k = n - 1; k >= 0 && trails; k--) {
        const adsb_plane_t *p = adsb_plane(idx[k]);
        double              x0 = 0, y0 = 0;
        bool                have = false;
        for (int j = 0; j < p->trail_n; j++) {
            const adsb_point_t *pt = &p->trail[(p->trail_head + ADSB_TRAIL - p->trail_n + j) % ADSB_TRAIL];
            double              x, y;
            int                 a = fade(now - pt->t);
            if (!proj(pt->lat, pt->lon, &x, &y) || a <= 0) {
                have = false;
                continue;
            }
            if (have) line(px, (int)lround(x0), (int)lround(y0), (int)lround(x), (int)lround(y), mix(c_amb, c_ink, a * 3 / 4));
            else put(px, (int)lround(x), (int)lround(y), mix(c_amb, c_ink, a * 3 / 4));
            x0 = x, y0 = y, have = true;
        }
        double x, y;
        if (have && p->pos_ok && proj(p->lat, p->lon, &x, &y))
            line(px, (int)lround(x0), (int)lround(y0), (int)lround(x), (int)lround(y), mix(c_amb, c_ink, 190));
    }

    /* the aircraft: live ones bright, ghosts fading; a ping for each message */
    for (int k = n - 1; k >= 0; k--) {
        const adsb_plane_t *p = adsb_plane(idx[k]);
        double              x, y;
        if (!p->pos_ok || !proj(p->lat, p->lon, &x, &y)) continue;
        int xi = (int)lround(x), yi = (int)lround(y);
        if (live(p, now)) {
            double since = now - p->seen;
            if (since < 1.5) circle(px, x, y, 5 + since * 8, 6, mix(c_amb, c_ink, (int)(255 * (1 - since / 1.5))));
            if (p->vel_ok) arrow(px, x, y, p->trk, c_amb);
            else square(px, xi, yi, 3, true, c_amb);
        } else {
            int a = fade(now - p->seen);
            if (a > 0) square(px, xi, yi, 3, false, mix(c_amb, c_ink, a * 3 / 4));
        }
        if (p->icao == sel_icao) brackets(px, xi, yi, 11, c_amb);
    }
}

static void draw_spark(double now) {
    uint32_t *px = (uint32_t *)spark->data;
    uint16_t  r[SPARK_N];
    int       top = 8;

    adsb_rate(now, r, SPARK_N);
    for (int i = 0; i < SPARK_N; i++)
        if (r[i] > top) top = r[i];
    for (int i = 0; i < SPARK_W * SPARK_H; i++) px[i] = c_ink;
    for (int x = 0; x < SPARK_W; x += 2) px[(SPARK_H - 1) * SPARK_W + x] = c_faint;
    for (int i = 0; i < SPARK_N; i++) {
        int h = r[i] ? 1 + (r[i] * (SPARK_H - 2)) / top : 0;
        for (int y = SPARK_H - 1 - h; y < SPARK_H - 1; y++)
            for (int x = i * 4; x < i * 4 + 3; x++) px[y * SPARK_W + x] = i == SPARK_N - 1 ? c_amb : c_dim;
    }
}

/* ---- Words ---- */

static void fmt_alt(char *s, size_t n, const adsb_plane_t *p) {
    if (!p->alt_ok) snprintf(s, n, "-");
    else if (p->alt >= 18000) snprintf(s, n, "FL%03d", p->alt / 100);
    else snprintf(s, n, "%d", p->alt);
}

static void fmt_age(char *s, size_t n, double age) {
    if (age < 100) snprintf(s, n, "%dS", (int)age);
    else if (age < 6000) snprintf(s, n, "%dM", (int)(age / 60));
    else snprintf(s, n, "%dH", (int)(age / 3600));
}

static const char *name_of(const adsb_plane_t *p, char *s, size_t n) {
    if (p->call[0]) snprintf(s, n, "%s", p->call);
    else snprintf(s, n, "%06X", p->icao);
    return s;
}

static void show_labels(double now, const int *idx, int n) {
    int used = 0;

    for (int pass = 0; pass < 2; pass++)        /* the one chosen first, so it always has its label */
        for (int k = 0; k < n && used < LABELS; k++) {
            const adsb_plane_t *p = adsb_plane(idx[k]);
            double              x, y;
            bool                chosen = p->icao == sel_icao;
            if (chosen != (pass == 0)) continue;
            if (!c_ok || !p->pos_ok || !proj(p->lat, p->lon, &x, &y) || x < 0 || y < 0 || x >= SC || y >= SC) continue;
            bool on = live(p, now);
            if (!on && !chosen && fade(now - p->seen) < 120) continue;
            char nm[12], alt[12];
            fmt_alt(alt, sizeof(alt), p);
            lv_label_set_text_fmt(lbl[used], "%s\n%s", name_of(p, nm, sizeof(nm)), alt);
            int lx = SC_X + (int)x + 9, ly = SC_Y + (int)y - 16;
            if (lx > SC_X + SC - 64) lx = SC_X + (int)x - 70;
            if (ly < SC_Y) ly = SC_Y;
            if (ly > SC_Y + SC - 34) ly = SC_Y + SC - 34;
            lv_obj_set_pos(lbl[used], lx, ly);
            lv_obj_set_style_text_color(lbl[used], on ? amb : dim, 0);
            lv_obj_clear_flag(lbl[used], LV_OBJ_FLAG_HIDDEN);
            used++;
        }
    for (int i = used; i < LABELS; i++) lv_obj_add_flag(lbl[i], LV_OBJ_FLAG_HIDDEN);
}

static void show_list(double now, const int *idx, int n) {
    int sel = -1;

    for (int k = 0; k < n; k++)
        if (adsb_plane(idx[k])->icao == sel_icao) sel = k;
    if (sel < 0 && n) {
        sel      = 0;
        sel_icao = adsb_plane(idx[0])->icao;
    }
    int first = sel < ROWS / 2 ? 0 : sel - ROWS / 2;
    if (first > n - ROWS) first = n - ROWS > 0 ? n - ROWS : 0;
    for (int r = 0; r < ROWS; r++) {
        int k = first + r;
        if (k >= n) {
            lv_label_set_text(row_l[r], r == 0 && !n ? "NOTHING HEARD YET" : "");
            lv_obj_set_style_bg_opa(row_l[r], LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(row_l[r], dim, 0);
            continue;
        }
        const adsb_plane_t *p = adsb_plane(idx[k]);
        char nm[12], alt[12], kt[8], trk[8], rng[10], brg[8], age[8];
        double b = 0;
        fmt_alt(alt, sizeof(alt), p);
        snprintf(kt, sizeof(kt), p->vel_ok ? "%.0f" : "-", p->gs);
        snprintf(trk, sizeof(trk), p->vel_ok ? "%03.0f" : "-", p->trk);
        if (p->pos_ok && c_ok) {
            snprintf(rng, sizeof(rng), "%.1f", adsb_range_nm(c_lat, c_lon, p->lat, p->lon, &b));
            snprintf(brg, sizeof(brg), "%03.0f", b);
        } else {
            snprintf(rng, sizeof(rng), "-");
            snprintf(brg, sizeof(brg), "-");
        }
        fmt_age(age, sizeof(age), now - p->seen);
        lv_label_set_text_fmt(row_l[r], "%-8s %06X %5s %3s %3s %5s %3s %4s", name_of(p, nm, sizeof(nm)), p->icao, alt,
                              kt, trk, rng, brg, age);
        bool chosen = k == sel;
        lv_obj_set_style_bg_opa(row_l[r], chosen ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row_l[r], chosen ? ink : live(p, now) ? amb : dim, 0);
    }
}

static void show_detail(double now, const int *idx, int n) {
    const adsb_plane_t *p = NULL;

    for (int k = 0; k < n; k++)
        if (adsb_plane(idx[k])->icao == sel_icao) p = adsb_plane(idx[k]);
    if (!p) {
        lv_label_set_text(det_l[0], "NO TARGET");
        lv_label_set_text(det_l[1], "MFK: CHOOSE ONE");
        lv_label_set_text(det_l[2], "");
        return;
    }
    char nm[12], alt[16], age[8], vr[16];
    fmt_alt(alt, sizeof(alt), p);
    fmt_age(age, sizeof(age), now - p->seen);
    lv_label_set_text_fmt(det_l[0], "%s  %06X  %s %s  %u MSGS", name_of(p, nm, sizeof(nm)), p->icao,
                          live(p, now) ? "LIVE," : "GHOST,", age, p->msgs);
    snprintf(vr, sizeof(vr), p->vr_ok ? "%+d FPM" : "", p->vr);
    if (p->vel_ok)
        lv_label_set_text_fmt(det_l[1], "%s%s  %.0f KT  TRK %03.0f  %s", alt, p->alt_ok && p->alt < 18000 ? " FT" : "",
                              p->gs, p->trk, vr);
    else
        lv_label_set_text_fmt(det_l[1], "%s%s  %s", alt, p->alt_ok && p->alt < 18000 ? " FT" : "", vr);
    if (p->pos_ok && c_ok) {
        double b, r = adsb_range_nm(c_lat, c_lon, p->lat, p->lon, &b);
        lv_label_set_text_fmt(det_l[2], "%.1f NM AT %03.0f  %.4f %.4f", r, b, p->lat, p->lon);
    } else {
        lv_label_set_text(det_l[2], p->pos_ok ? "NO CENTRE YET" : "NO POSITION YET");
    }
}

static void show_scope_words(double now, int nlive) {
    char              dev[32];
    adsb_rx_state_t   st = adsb_rx_state(dev, sizeof(dev));
    const char       *msg = NULL;

    switch (st) {
        case ADSB_RX_NO_STICK: msg = "NO ADS-B RECEIVER\n\nPLUG A GNS 5890 OR A\nMICROADSB INTO USB"; break;
        case ADSB_RX_NO_DRIVER: msg = "RECEIVER FOUND\n\nTHE KERNEL HAS\nNO CDC_ACM DRIVER"; break;
        case ADSB_RX_OPENING: msg = "RECEIVER FOUND\n\nOPENING ITS PORT"; break;
        case ADSB_RX_OFF: msg = "RECEIVER NOT STARTED"; break;
        case ADSB_RX_LENT: msg = "RTL-SDR COMING\n\nFROM THE SENTRY'S\nVHF/UHF SPECTRUM"; break;
        default:
            if (!nlive && !adsb_count()) msg = "LISTENING ON 1090 MHZ\n\nNO AIRCRAFT YET";
            break;
    }
    if (msg) {
        lv_label_set_text(centre_l, msg);
        lv_obj_clear_flag(centre_l, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(centre_l, LV_OBJ_FLAG_HIDDEN);
    }
    double r = range_nm();
    for (int k = 0; k < 4; k++) {
        double v = r * (k + 1) / 4;
        if (v == floor(v)) lv_label_set_text_fmt(ring_l[k], "%d", (int)v);
        else lv_label_set_text_fmt(ring_l[k], "%.1f", v);
    }
    (void)now;
}

static void show_head(double now, int nlive, int nghost) {
    char      dev[32], name[48], t[16];
    time_t    tt = time(NULL);
    struct tm tm;
    adsb_rx_state_t st = adsb_rx_state(dev, sizeof(dev));

    adsb_rx_name(name, sizeof(name));
    localtime_r(&tt, &tm);              /* the radio's time zone, named, as the Sentry's header */
    strftime(t, sizeof(t), "%H:%M %Z", &tm);
    lv_label_set_text_fmt(status_l, "%s   %d LIVE   %d HISTORY   %s   %s",
                          st == ADSB_RX_ON ? name : "NO RECEIVER", nlive, nghost,
                          c_from, t);
    const adsb_stats_t *s = adsb_stats();
    uint16_t            r[10];
    unsigned            sum = 0;
    adsb_rate(now - 1, r, 10);              /* the last ten whole seconds */
    for (int i = 0; i < 10; i++) sum += r[i];
    lv_label_set_text_fmt(sig_l, "1090 MHZ   %u.%u MSG/S   %u ADS-B   %u BAD CRC", sum / 10, sum % 10, s->adsb,
                          s->crc_bad);
}

static void show_keys(void) {
    for (int i = 0; i < 5; i++) {
        char t[40];
        snprintf(t, sizeof(t), "F%d %s", i + 1, f_label(i));
        lv_label_set_text(key_l[i], t);
        lv_obj_set_style_pad_top(key_l[i], strchr(t, '\n') ? 2 : 12, 0);
        buttons_refresh(&btn[i]);
    }
}

static void refresh(void) {
    double now = adsb_now();
    int    idx[ADSB_MAX_PLANES], n, nlive = 0;
    int    d = decay_s();

    adsb_lock();
    adsb_expire(now, d == 0 ? ADSB_LIVE_S : d < 0 ? -1 : fmax(d, ADSB_LIVE_S), d == 0 ? 0 : d < 0 ? -1 : d);
    find_centre(now);
    n = order(idx, now);
    for (int k = 0; k < n; k++) nlive += live(adsb_plane(idx[k]), now);
    draw_scope(now, idx, n);
    draw_spark(now);
    show_labels(now, idx, n);
    show_list(now, idx, n);
    show_detail(now, idx, n);
    show_head(now, nlive, n - nlive);
    adsb_unlock();
    show_scope_words(now, nlive);
    lv_obj_invalidate(scope_img);
    lv_obj_invalidate(spark_img);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    refresh();
}

/* ---- Keys ---- */

static const char *f_label(int i) {
    static char s[5][24];

    switch (i) {
        case 0: snprintf(s[0], sizeof(s[0]), "RANGE\n%d NM", range_nm()); break;
        case 1: snprintf(s[1], sizeof(s[1]), "DECAY\n%s", decay_names[param_i_get(cfg_adsb_decay)]); break;
        case 2: snprintf(s[2], sizeof(s[2]), "TRACKS\n%s", param_i_get(cfg_adsb_trails) ? "ON" : "OFF"); break;
        case 3: snprintf(s[3], sizeof(s[3]), "CLEAR\nHISTORY"); break;
        default: snprintf(s[4], sizeof(s[4]), "SENTRY"); break;
    }
    return s[i];
}

static void set_range(int d) {
    int r = param_i_get(cfg_adsb_range) + d;

    if (r < 0 || r >= N_RANGES) return;
    param_i_set(cfg_adsb_range, r);
    show_keys();
    refresh();
}

static void step_sel(int d) {
    double now = adsb_now();
    int    idx[ADSB_MAX_PLANES], n, k = -1;

    adsb_lock();
    n = order(idx, now);
    for (int i = 0; i < n; i++)
        if (adsb_plane(idx[i])->icao == sel_icao) k = i;
    if (n) sel_icao = adsb_plane(idx[k < 0 ? 0 : (k + d + n) % n])->icao;
    adsb_unlock();
    refresh();
}

static void help_show(bool on) {
    help_on = on;
    if (on) lv_obj_clear_flag(help_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(help_l, LV_OBJ_FLAG_HIDDEN);
}

static void fkey(int k) {
    if (help_on) {
        help_show(false);
        return;
    }
    switch (k) {
        case 0: set_range(param_i_get(cfg_adsb_range) + 1 < N_RANGES ? 1 : -(N_RANGES - 1)); return;
        case 1: param_i_set(cfg_adsb_decay, (param_i_get(cfg_adsb_decay) + 1) % N_DECAYS); break;
        case 2: param_i_set(cfg_adsb_trails, !param_i_get(cfg_adsb_trails)); break;
        case 3:
            adsb_lock();
            adsb_clear();
            adsb_unlock();
            sel_icao = 0;
            break;
        default: dialog_destruct(); return;
    }
    show_keys();
    refresh();
}

static void key_tap_cb(lv_event_t *e) { fkey((int)(intptr_t)lv_event_get_user_data(e)); }

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (help_on) {
        help_show(false);
        return;
    }
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP: step_sel(-1); break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN: step_sel(1); break;
        case LV_KEY_ESC: dialog_destruct(); break;
        default: break;
    }
}

/* The VFO knob: the range, a step for each three clicks, clockwise further out */
static void rotary_cb(int32_t diff) {
    rot_acc += diff;
    if (rot_acc >= 3 || rot_acc <= -3) {
        set_range(rot_acc > 0 ? 1 : -1);
        rot_acc = 0;
    }
}

static bool front_key(int key, int state) {
    if (key == KEYPAD_GEN) {
        if (state == KEYPAD_RELEASE) help_show(!help_on);
        return true;
    }
    if (help_on && state == KEYPAD_RELEASE) {
        help_show(false);
        return true;
    }
    return false;
}

/* A tap on the scope chooses the aircraft nearest it, on a row that one */
static void scope_tap_cb(lv_event_t *e) {
    (void)e;
    lv_point_t pt;
    lv_area_t  a;
    lv_indev_t *in = lv_indev_get_act();

    if (!in) return;
    lv_indev_get_point(in, &pt);
    lv_obj_get_coords(scope_img, &a);
    double tx = pt.x - a.x1, ty = pt.y - a.y1, best = 30 * 30, now = adsb_now();
    adsb_lock();
    for (int i = 0; i < adsb_count(); i++) {
        const adsb_plane_t *p = adsb_plane(i);
        double              x, y;
        if (!p->pos_ok || !c_ok || !proj(p->lat, p->lon, &x, &y)) continue;
        double d = (x - tx) * (x - tx) + (y - ty) * (y - ty);
        if (d < best && (live(p, now) || fade(now - p->seen) > 0)) best = d, sel_icao = p->icao;
    }
    adsb_unlock();
    refresh();
}

static void row_tap_cb(lv_event_t *e) {
    int    r = (int)(intptr_t)lv_event_get_user_data(e), idx[ADSB_MAX_PLANES], n, sel = 0;
    double now = adsb_now();

    adsb_lock();
    n = order(idx, now);
    for (int k = 0; k < n; k++)
        if (adsb_plane(idx[k])->icao == sel_icao) sel = k;
    int first = sel < ROWS / 2 ? 0 : sel - ROWS / 2;
    if (first > n - ROWS) first = n - ROWS > 0 ? n - ROWS : 0;
    if (first + r < n) sel_icao = adsb_plane(idx[first + r])->icao;
    adsb_unlock();
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

/* A box: a frame, its caption over a line */
static void box(int x, int y, int w, int h, const char *cap) {
    rect(root, x, y, w, h, 3, false);
    label(root, &pixel_20, amb, x + 3, y + 3, w - 6, LV_TEXT_ALIGN_CENTER, cap);
    rect(root, x, y + 25, w, 3, 0, true);
}

static void build(void) {
    /* the head, as the Sentry's */
    rect(root, 6, 3, SW - 12, HEAD_H, 3, false);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = rect(root, i ? SW - 50 : 12, 7, 36, 36, 3, false);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        badge_l[i] = label(root, &pixel_28, amb, i ? SW - 50 : 12, 10, 36, LV_TEXT_ALIGN_CENTER, "R");
    }
    title_l  = label(root, &pixel_20, amb, 60, 6, SW - 120, LV_TEXT_ALIGN_CENTER, "XIEGU X6200  AIRSPACE SENTRY");
    status_l = label(root, &pixel_16, amb, 60, 27, SW - 120, LV_TEXT_ALIGN_CENTER, "");

    /* the scope */
    rect(root, SC_X - 3, BODY_Y, SC + 6, SC + 6, 3, false);
    scope     = lv_img_buf_alloc(SC, SC, LV_IMG_CF_TRUE_COLOR);
    scope_img = lv_img_create(root);
    lv_img_set_src(scope_img, scope);
    lv_obj_set_pos(scope_img, SC_X, SC_Y);
    lv_obj_add_flag(scope_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scope_img, scope_tap_cb, LV_EVENT_CLICKED, NULL);
    north_l = label(root, &pixel_16, amb, SC_X + SC / 2 - 20, SC_Y - 1, 40, LV_TEXT_ALIGN_CENTER, "N");
    for (int k = 0; k < 4; k++) {           /* along a diagonal, clear of the N */
        double r = (SC / 2.0 - 14) * (k + 1) / 4;
        ring_l[k] = label(root, &pixel_16, dim, SC_X + SC / 2 + (int)(r * 0.5) + 3, SC_Y + SC / 2 - (int)(r * 0.866) - 14,
                          0, LV_TEXT_ALIGN_LEFT, "");
    }
    for (int i = 0; i < LABELS; i++) {
        lbl[i] = label(root, &pixel_16, amb, 0, 0, 0, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_add_flag(lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    centre_l = label(root, &pixel_20, amb, SC_X + 10, SC_Y + SC / 2 - 60, SC - 20, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_style_bg_color(centre_l, ink, 0);
    lv_obj_set_style_bg_opa(centre_l, LV_OPA_COVER, 0);
    lv_label_set_long_mode(centre_l, LV_LABEL_LONG_WRAP);

    /* the list */
    box(RX0, BODY_Y, RW, 196, "AIRCRAFT: LIVE, THEN HISTORY");
    list_head_l = label(root, &pixel_16, dim, RX0 + 6, BODY_Y + 30, RW - 12, LV_TEXT_ALIGN_LEFT,
                        "CALL     ICAO     ALT  KT TRK    NM BRG  AGE");
    for (int r = 0; r < ROWS; r++) {
        row_l[r] = label(root, &pixel_16, amb, RX0 + 6, BODY_Y + 48 + r * 18, RW - 12, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_set_height(row_l[r], 18);
        lv_obj_set_style_bg_color(row_l[r], amb, 0);
        lv_obj_add_flag(row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row_l[r], row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    }

    /* the one chosen */
    box(RX0, BODY_Y + 202, RW, 96, "TARGET");
    for (int i = 0; i < 3; i++) det_l[i] = label(root, &pixel_20, amb, RX0 + 6, BODY_Y + 232 + i * 21, RW - 12, LV_TEXT_ALIGN_LEFT, "");

    /* the signals */
    rect(root, RX0, BODY_Y + 304, RW, SC + 6 - 304, 3, false);
    sig_l     = label(root, &pixel_16, amb, RX0 + 6, BODY_Y + 307, RW - 12, LV_TEXT_ALIGN_LEFT, "");
    spark     = lv_img_buf_alloc(SPARK_W, SPARK_H, LV_IMG_CF_TRUE_COLOR);
    spark_img = lv_img_create(root);
    lv_img_set_src(spark_img, spark);
    lv_obj_set_pos(spark_img, RX0 + 6, BODY_Y + 325);

    /* what the knobs do, and the F keys */
    int ky = 480 - KEYS_H - 3;
    legend_l = label(root, &pixel_16, dim, 6, ky - LEGEND_H, SW - 12, LV_TEXT_ALIGN_CENTER,
                     "VFO: RANGE | MFK: AIRCRAFT | TAP: CHOOSE | F2: HOW LONG HISTORY STAYS | GEN: HELP");
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

    help_l = label(root, &pixel_20, amb, 40, 70, SW - 80, LV_TEXT_ALIGN_LEFT,
                   "AIRSPACE: AIRCRAFT ON 1090 MHZ, FROM AN ADS-B RECEIVER ON USB (AN RTL-SDR STICK, GNS 5890 OR MICROADSB).\n\n"
                   "AN ARROW IS AN AIRCRAFT HEARD IN THE LAST MINUTE, POINTING ALONG ITS TRACK; A RING GROWS "
                   "ROUND IT FOR EACH MESSAGE. A SQUARE IS A GHOST: NOT HEARD FOR A MINUTE, FADING WITH ITS TRACK.\n\n"
                   "F1 / VFO: RANGE OF THE SCOPE.  F2: DECAY, HOW LONG GHOSTS AND TRACKS STAY: OFF, 1 TO 60 "
                   "MINUTES, OR HOLD TILL CLEARED.  F3: TRACKS ON OR OFF.  F4: FORGET THE HISTORY.  F5: BACK TO "
                   "THE SENTRY.\n\n"
                   "MFK OR A TAP CHOOSES AN AIRCRAFT. A GNS 5890 OR MICROADSB GOES ON LISTENING WHEN THIS SCREEN CLOSES; "
                   "AN RTL-SDR IS THIS SCREEN'S WHILE IT IS OPEN AND THE SENTRY'S THE REST OF THE TIME. RECEIVE ONLY.\n\n"
                   "THE CENTRE: A USB GPS, ELSE THE QTH LOCATOR (SETTINGS > GENERAL), ELSE THE TRAFFIC.\n\n"
                   "ANY KEY: CLOSE.");
    lv_label_set_long_mode(help_l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(help_l, ink, 0);
    lv_obj_set_style_bg_opa(help_l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(help_l, 3, 0);
    lv_obj_set_style_border_color(help_l, amb, 0);
    lv_obj_set_style_pad_all(help_l, 14, 0);
    lv_obj_add_flag(help_l, LV_OBJ_FLAG_HIDDEN);
}

static void construct_cb(lv_obj_t *parent) {
    amb     = TC(accent);
    ink     = TC(bg);
    dim     = lv_color_mix(amb, ink, 110);
    faint   = lv_color_mix(amb, ink, 50);
    c_amb   = argb(amb);
    c_ink   = argb(ink);
    c_dim   = argb(dim);
    c_faint = argb(faint);
    for (int i = 0; i < 64; i++) glow[i] = mix(c_amb, c_ink, i * 60 / 63);

    ang = malloc(sizeof(uint16_t) * SC * SC);
    if (ang) {
        double r = SC / 2.0 - 14, cx = SC / 2.0 - 0.5, cy = SC / 2.0 - 0.5;
        for (int y = 0; y < SC; y++)
            for (int x = 0; x < SC; x++) {
                double dx = x - cx, dy = cy - y;
                ang[y * SC + x] = dx * dx + dy * dy > r * r ? 0xffff
                                                            : (uint16_t)((int)lround(fmod(atan2(dx, dy) * 1800 / M_PI + 3600, 3600)) % 3600);
            }
    }

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
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);

    rtl_spectrum_yield(true);       /* an RTL-SDR: the Sentry's helper lets it go, dump1090 has it while this is open */
    adsb_rx_start();
    help_on = false;
    rot_acc = 0;
    show_keys();
    if (ang) refresh();
    timer = lv_timer_create(timer_cb, 200, NULL);
}

static void destruct_cb(void) {
    if (timer) lv_timer_del(timer);
    timer = NULL;
    rtl_spectrum_yield(false);      /* the RTL-SDR back to the Sentry */
    lv_group_remove_obj(root);
    if (scope) {
        lv_img_cache_invalidate_src(scope);
        lv_img_buf_free(scope);
        scope = NULL;
    }
    if (spark) {
        lv_img_cache_invalidate_src(spark);
        lv_img_buf_free(spark);
        spark = NULL;
    }
    free(ang);
    ang  = NULL;
    root = NULL;
}
