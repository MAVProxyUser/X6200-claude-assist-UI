/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - WWV and WWVH's time from the receiver's audio (wwv.h).
 *
 *  The second: the power at 1000 Hz and at 1200 Hz over 5 ms (a tick's length), each folded onto one second (a sample
 *  of the second each, decaying over some 7 s): the ticks pile up at their place in the second, the rest (voice, the
 *  500 and 600 Hz tones, noise) spreads out. The higher peak over its mean names the station; the peak, a tick's
 *  length after its start, is the second's start in the audio.
 *  The code: the 100 Hz mixed to 0 Hz and averaged over 10 ms (one period: the 200 Hz image cancels), its level kept
 *  at 1 kHz for 4 s. Each second, once it is past, read in four windows: 60-180 ms (on in every second but :00),
 *  250-450 (on for a 1 and a marker), 550-750 (a marker only) and 850-970 (off: the level the 100 Hz drops to, 15 dB
 *  down). The threshold sits between the on and off levels kept from the seconds before (their geometric mean).
 *  The minute: a marker then a second with no 100 Hz is :59 then :00. A frame is taken only whole: its :00 empty, the
 *  six markers where they belong, every other second a 0 or a 1, the unused seconds 0, the digits BCD.
 */

#include "wwv.h"

#include <math.h>
#ifdef WWV_DEBUG
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#define MAX_RATE  12000
#define DEC       11            /* the 100 Hz level kept every 11 samples: ~1 kHz at 11025 Hz */
#define ERING     4096          /* its ring: ~4 s */
#define FOLD_KEEP 0.85f         /* a fold's place keeps this much of itself each second: ~7 s */
#define LOCK_DB   8.0f          /* ticks this far over the rest of the second: locked */
#define CODE_DB   6.0f          /* 100 Hz on over off at least, or no code is read */
#define LAG_MAX   256           /* the tick's power against its own 15 ms before (wwv_feed) */

typedef struct {
    float  f, rr, ri, sr, si;   /* the mixer: frequency, phasor, its step */
    int    n;                   /* samples since the phasor was renormalised */
    float *hr, *hi;             /* the boxcar's ring */
    int    len, at;
    double sum_r, sum_i;
} mix_t;

struct wwv_s {
    float    rate;
    int      S;                 /* samples a second */
    mix_t    m1000, m1200, m100;
    float   *fold1000, *fold1200;
    float    ering[ERING];
    float    lag1000[LAG_MAX], lag1200[LAG_MAX];   /* the tick powers of the last 15 ms */
    int      lag, lag_at;
    uint64_t count;             /* samples in so far */
    /* time base: sample `base_count` came at `base_t` */
    uint64_t base_count;
    double   base_t;
    /* the second */
    bool     locked;
    int      epoch;             /* the second starts this many samples into each fold */
    int64_t  next_b;            /* the next second to read: its start, absolute sample; -1 none yet */
    float    hi, lo;            /* the 100 Hz on and off levels */
    bool     levels;
    /* the minute */
    int      sec;
    char     prev_sym;
    char     cur[61];
    int64_t  frame_b0;          /* this frame's :00, absolute sample */
    int64_t  prev_serial;       /* the last frame's minute, minutes since the epoch; -1 none */
    double   prev_t;
    wwv_info_t info;
};

static bool mix_init(mix_t *m, float f, float rate, int len) {
    m->f   = f;
    m->rr  = 1;
    m->ri  = 0;
    m->sr  = cosf(-2 * (float)M_PI * f / rate);
    m->si  = sinf(-2 * (float)M_PI * f / rate);
    m->len = len;
    m->hr  = calloc(len, sizeof(float));
    m->hi  = calloc(len, sizeof(float));
    return m->hr && m->hi;
}

static void mix_clear(mix_t *m) {
    m->rr = 1;
    m->ri = 0;
    m->n = m->at = 0;
    m->sum_r = m->sum_i = 0;
    memset(m->hr, 0, m->len * sizeof(float));
    memset(m->hi, 0, m->len * sizeof(float));
}

/* One sample in; the boxcar's mean power out */
static float mix_power(mix_t *m, float x) {
    float r = x * m->rr, i = x * m->ri;
    float nr = m->rr * m->sr - m->ri * m->si;

    m->ri = m->rr * m->si + m->ri * m->sr;
    m->rr = nr;
    if (++m->n == 4096) {
        float g = 1.0f / sqrtf(m->rr * m->rr + m->ri * m->ri);
        m->rr *= g;
        m->ri *= g;
        m->n = 0;
    }
    m->sum_r += r - m->hr[m->at];
    m->sum_i += i - m->hi[m->at];
    m->hr[m->at] = r;
    m->hi[m->at] = i;
    m->at = (m->at + 1) % m->len;
    double mr = m->sum_r / m->len, mi = m->sum_i / m->len;
    return (float)(mr * mr + mi * mi);
}

wwv_t *wwv_new(float rate) {
    wwv_t *w;

    if (rate < 4000 || rate > MAX_RATE || !(w = calloc(1, sizeof(*w)))) return NULL;
    w->rate = rate;
    w->S = (int)lrintf(rate);
    w->lag = (int)lrintf(0.015f * rate);
    w->fold1000 = calloc(w->S, sizeof(float));
    w->fold1200 = calloc(w->S, sizeof(float));
    int tick = (int)lrintf(0.005f * rate), per = (int)lrintf(0.010f * rate);

    if (!w->fold1000 || !w->fold1200 || !mix_init(&w->m1000, 1000, rate, tick) ||
        !mix_init(&w->m1200, 1200, rate, tick) || !mix_init(&w->m100, 100, rate, per)) {
        wwv_free(w);
        return NULL;
    }
    wwv_reset(w);
    return w;
}

void wwv_free(wwv_t *w) {
    if (!w) return;
    free(w->fold1000);
    free(w->fold1200);
    free(w->m1000.hr), free(w->m1000.hi);
    free(w->m1200.hr), free(w->m1200.hi);
    free(w->m100.hr), free(w->m100.hi);
    free(w);
}

void wwv_reset(wwv_t *w) {
    mix_clear(&w->m1000);
    mix_clear(&w->m1200);
    mix_clear(&w->m100);
    memset(w->fold1000, 0, w->S * sizeof(float));
    memset(w->fold1200, 0, w->S * sizeof(float));
    memset(w->ering, 0, sizeof(w->ering));
    memset(w->lag1000, 0, sizeof(w->lag1000));
    memset(w->lag1200, 0, sizeof(w->lag1200));
    w->lag_at = 0;
    w->count = w->base_count = 0;
    w->locked = w->levels = false;
    w->next_b = -1;
    w->sec = -1;
    w->prev_sym = 0;
    memset(w->cur, 0, sizeof(w->cur));
    w->prev_serial = -1;
    memset(&w->info, 0, sizeof(w->info));
    w->info.second = -1;
}

static double t_of(const wwv_t *w, int64_t sample) {
    return w->base_t + (double)(sample - (int64_t)w->base_count) / w->rate;
}

/* ---- The frame ---- */

static int bits(const char *c, const int *at, const int *weight, int n) {
    int v = 0;

    for (int i = 0; i < n; i++)
        if (c[at[i]] == '1') v += weight[i];
    return v;
}

static int64_t days_from_civil(int y) {      /* days from 1970-01-01 to y-01-01 */
    int64_t d = 0;

    for (int k = 1970; k < y; k++) d += (k % 4 == 0 && (k % 100 != 0 || k % 400 == 0)) ? 366 : 365;
    return d;
}

int64_t wwv_minute_utc(const wwv_info_t *in) {
    return ((days_from_civil(in->year) + in->yday - 1) * 24 + in->hour) * 3600 + in->minute * 60;
}

static bool frame(wwv_t *w) {
    static const int unused[] = {1, 8, 14, 18, 24, 27, 28, 34, 42, 43, 44, 45, 46, 47, 48};
    const char      *c = w->cur;

    w->info.frames++;
    if (c[0] != '-') goto bad;
    for (int s = 1; s < 60; s++) {
        bool marker = s % 10 == 9;

        if (marker ? c[s] != 'M' : (c[s] != '0' && c[s] != '1')) goto bad;
    }
    for (size_t i = 0; i < sizeof(unused) / sizeof(unused[0]); i++)
        if (c[unused[i]] == '1') goto bad;

    static const int yu[] = {4, 5, 6, 7}, yt[] = {51, 52, 53, 54}, mu[] = {10, 11, 12, 13}, mt[] = {15, 16, 17},
                     hu[] = {20, 21, 22, 23}, ht[] = {25, 26}, du[] = {30, 31, 32, 33}, dt[] = {35, 36, 37, 38},
                     dh[] = {40, 41}, w1248[] = {1, 2, 4, 8}, w124[] = {1, 2, 4};
    int y1 = bits(c, yu, w1248, 4), y10 = bits(c, yt, w1248, 4), m1 = bits(c, mu, w1248, 4), m10 = bits(c, mt, w124, 3);
    int h1 = bits(c, hu, w1248, 4), h10 = bits(c, ht, w124, 2), d1 = bits(c, du, w1248, 4), d10 = bits(c, dt, w1248, 4);
    int d100 = bits(c, dh, w124, 2);

    if (y1 > 9 || y10 > 9 || m1 > 9 || m10 > 5 || h1 > 9 || d1 > 9 || d10 > 9) goto bad;
    int minute = m10 * 10 + m1, hour = h10 * 10 + h1, yday = d100 * 100 + d10 * 10 + d1;

    if (hour > 23 || yday < 1 || yday > 366) goto bad;

    wwv_info_t *in = &w->info;

    in->have_time = true;
    in->year = 2000 + y10 * 10 + y1;
    in->yday = yday;
    in->hour = hour;
    in->minute = minute;
    in->dut1 = (c[50] == '1' ? 1 : -1) * 0.1f * ((c[56] == '1') + 2 * (c[57] == '1') + 4 * (c[58] == '1'));
    in->dst1 = c[2] == '1';
    in->dst2 = c[55] == '1';
    in->leap_warning = c[3] == '1';
    in->minute_t = t_of(w, w->frame_b0);

    int64_t serial = wwv_minute_utc(in) / 60;

    in->frames_ok = w->prev_serial >= 0 && serial == w->prev_serial + 1 && fabs(in->minute_t - w->prev_t - 60) < 0.5
                    ? in->frames_ok + 1 : 1;
    w->prev_serial = serial;
    w->prev_t = in->minute_t;
    return true;
bad:
    w->info.frames_bad++;
    w->info.frames_ok = 0;
    w->prev_serial = -1;
    return false;
}

/* ---- The seconds ---- */

static float level(const wwv_t *w, int64_t b, float from, float to) {
    int64_t a = (b + (int64_t)(from * w->rate)) / DEC, e = (b + (int64_t)(to * w->rate)) / DEC;
    double  sum = 0;

    for (int64_t k = a; k < e; k++) sum += w->ering[k % ERING];
    return e > a ? (float)(sum / (e - a)) : 0;
}

/* The second that began at sample b, now past: its symbol, then the minute it is in */
static bool second(wwv_t *w, int64_t b) {
    float A = level(w, b, 0.06f, 0.18f), B = level(w, b, 0.25f, 0.45f), C = level(w, b, 0.55f, 0.75f);
    float D = level(w, b, 0.85f, 0.97f);
    char  s;

    if (!w->levels) {
        w->hi = A > D ? A : D;
        w->lo = A > D ? D : A;
        w->levels = true;
    }
    float thr = sqrtf(w->hi * w->lo + 1e-30f);

    if (A > thr) w->hi += 0.2f * (A - w->hi);
    w->lo += 0.2f * (D - w->lo);
    w->info.code_db = 20 * log10f((w->hi + 1e-20f) / (w->lo + 1e-20f));
    if (w->info.code_db < CODE_DB) s = '?';
    else s = A < thr ? '-' : B < thr ? '0' : C < thr ? '1' : 'M';
#ifdef WWV_DEBUG
    fprintf(stderr, "b=%lld A=%.4g B=%.4g C=%.4g D=%.4g hi=%.4g lo=%.4g -> %c sec %d\n", (long long)b, A, B, C, D,
            w->hi, w->lo, s, w->sec);
#endif

    if (w->prev_sym == 'M' && s == '-') {   /* :59 then :00 */
        w->sec = 0;
        w->frame_b0 = b;
        memset(w->cur, 0, sizeof(w->cur));
    } else if (w->sec >= 0) {
        if (++w->sec == 60) {               /* no :00 where one was due: from here as if it were */
            w->sec = 0;
            w->frame_b0 = b;
            memset(w->cur, 0, sizeof(w->cur));
        }
    }
    w->prev_sym = s;
    if (w->sec >= 0) {
        w->cur[w->sec] = s;
        if (w->sec == 59) frame(w);
    }
    w->info.second = w->sec;
    memcpy(w->info.symbols, w->cur, sizeof(w->cur));
    return true;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* A fold's peak, where it is, and its median (from every 7th place: enough, and cheap once a second) */
static float fold_peak(const float *fold, int S, int *at, float *median) {
    float pk = 0, few[MAX_RATE / 7 + 1];
    int   n = 0;

    for (int i = 0; i < S; i++)
        if (fold[i] > pk) pk = fold[i], *at = i;
    for (int i = 0; i < S; i += 7) few[n++] = fold[i];
    qsort(few, n, sizeof(float), cmp_float);
    *median = few[n / 2];
    return pk;
}

/* Once a second: where the ticks are, and the seconds now past */
static bool each_second(wwv_t *w) {
    float   m1, m2;
    int     i1 = 0, i2 = 0;
    bool    changed = false;
    float   p1 = fold_peak(w->fold1000, w->S, &i1, &m1), p2 = fold_peak(w->fold1200, w->S, &i2, &m2);
    /* The station: the stronger ticks (WWV's 800 ms minute tone at 1000 Hz lifts its fold's middle, not its peak) */
    bool    h = p2 > p1;
    float   db = 10 * log10f(((h ? p2 : p1) + 1e-30f) / ((h ? m2 : m1) + 1e-30f));
    int   ep = ((h ? i2 : i1) - (w->m1000.len - 1) + w->S) % w->S;

    w->info.tick_db = db;
    w->info.station = db >= LOCK_DB ? (h ? 2 : 1) : 0;
    if (db < LOCK_DB) {
        if (w->locked) changed = true;
        w->locked = w->info.locked = false;
        w->next_b = -1;
        w->sec = -1;
        w->info.second = -1;
        return changed;
    }
    if (!w->locked) changed = true;
    w->locked = w->info.locked = true;
    w->epoch = ep;

    /* The seconds to read: from two back (the ring holds four), the one nearest where the last left off */
    int64_t now = (int64_t)w->count;

    if (w->next_b < 0) {
        int64_t k = (now - 2 * w->S - ep) / w->S;
        w->next_b = k * w->S + ep;
    } else {
        int64_t k = (w->next_b - ep + w->S / 2) / w->S;
        w->next_b = k * w->S + ep;
    }
    while (w->next_b + w->S + 2 * DEC <= now) {
        if (w->next_b >= 0 && w->next_b >= now - (int64_t)(ERING - 2) * DEC) changed |= second(w, w->next_b);
        w->next_b += w->S;
    }
    return changed;
}

bool wwv_feed(wwv_t *w, const float *x, size_t n, double t0) {
    bool changed = false;

    w->base_count = w->count;
    w->base_t = t0;
    for (size_t k = 0; k < n; k++) {
        int   at = (int)(w->count % (uint64_t)w->S);
        float p1 = mix_power(&w->m1000, x[k]), p2 = mix_power(&w->m1200, x[k]), p0 = mix_power(&w->m100, x[k]);

        /* Only a rise counts: a tick comes out of the 10 ms of silence before it, a steady tone (WWV's 800 ms one at
         * :00, a 1000 Hz test tone) adds nothing after its first 5 ms, and that start is on the second anyway */
        float r1 = p1 - w->lag1000[w->lag_at], r2 = p2 - w->lag1200[w->lag_at];

        w->lag1000[w->lag_at] = p1;
        w->lag1200[w->lag_at] = p2;
        w->lag_at = (w->lag_at + 1) % w->lag;
        w->fold1000[at] = w->fold1000[at] * FOLD_KEEP + (r1 > 0 ? r1 : 0);
        w->fold1200[at] = w->fold1200[at] * FOLD_KEEP + (r2 > 0 ? r2 : 0);
        if (w->count % DEC == 0) w->ering[(w->count / DEC) % ERING] = sqrtf(p0);
        w->count++;
        if (at == w->S - 1) changed |= each_second(w);
    }
    return changed;
}

void wwv_info(const wwv_t *w, wwv_info_t *out) {
    *out = w->info;
}
