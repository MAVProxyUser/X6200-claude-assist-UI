/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - SAME header decoder (same.h). A sliding DFT over one bit at the mark and the space frequency gives
 *  the tone of each moment; a phase-locked bit clock pulled to the transitions samples it at the middle of each
 *  bit; bytes are found from the preamble (0xAB) and read least significant bit first until a character that no
 *  header has. Each header comes three times: kept, and decided by a vote of the copies, character by character.
 */

#include "same.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAUD     520.8333f
#define MARK_HZ  2083.3333f
#define SPACE_HZ 1562.5f
#define COPIES   3
#define LONE_S   6.0f           /* a copy left alone this long is taken by itself */

struct same_s {
    float    rate, spb;          /* samples per bit */
    int      nwin;               /* the DFT's window: one bit */
    float   *hist;               /* the window's samples */
    int      at;
    double   wm, ws;             /* radians a sample */
    double   mr, mi, sr, si;     /* the running sums */
    long     n;                  /* samples seen */
    float    phase;              /* bit clock, 0..1, a bit sampled at 0.5 */
    int      last;               /* the tone of the last sample: 1 mark, 0 space */
    uint8_t  shift;              /* the last 8 bits, the newest at the top (LSB first on air) */
    int      bits;               /* bits since the byte boundary, -1: looking for the preamble */
    char     cur[SAME_MAX + 1];  /* the header being read */
    int      len;
    char     copy[COPIES][SAME_MAX + 1];
    int      copies;
    long     first_at;           /* the sample the first copy ended at */
    long     quiet_until;        /* a header or NNNN decided: its remaining copies ignored till this sample */
};

#define AFTER_S  4.5f            /* a set of three copies is over this long after two were decided */

same_t *same_new(float rate) {
    same_t *s = calloc(1, sizeof(*s));

    if (!s) return NULL;
    s->rate = rate;
    s->spb  = rate / BAUD;
    s->nwin = (int)lroundf(s->spb);
    s->hist = calloc((size_t)s->nwin, sizeof(float));
    s->wm   = 2 * M_PI * MARK_HZ / rate;
    s->ws   = 2 * M_PI * SPACE_HZ / rate;
    s->bits = -1;
    if (!s->hist) {
        free(s);
        return NULL;
    }
    return s;
}

void same_free(same_t *s) {
    if (!s) return;
    free(s->hist);
    free(s);
}

static bool header_char(int c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '+' || c == '/' || c == ' ' ||
           (c >= 'a' && c <= 'z');
}

/* The copies voted, character by character: false when two of them do not agree enough to say */
static bool vote(same_t *s, char *out, size_t out_n) {
    int  n = s->copies;
    char v[SAME_MAX + 1];
    int  len = 0;

    if (n == 0) return false;
    for (int i = 0; i < SAME_MAX; ++i) {
        int  best = 0;
        char c    = 0;

        for (int a = 0; a < n; ++a) {
            char ca = s->copy[a][i];
            int  k  = 0;

            if (!ca) continue;
            for (int b = 0; b < n; ++b) k += s->copy[b][i] == ca;
            if (k > best) best = k, c = ca;
        }
        if (!c) break;
        if (n >= 2 && best < 2) return false;       /* the copies disagree here */
        v[len++] = c;
    }
    v[len] = 0;
    if (len < 4) return false;
    snprintf(out, out_n, "%s", v);
    return true;
}

static bool copy_done(same_t *s, char *out, size_t out_n) {
    bool decided = false;

    s->cur[s->len] = 0;
    /* a header ("ZCZC-" ... at least the fields) or the end of a message */
    if (s->n < s->quiet_until) {
        /* the rest of a set already decided */
    } else if ((s->len >= 30 && !strncmp(s->cur, "ZCZC", 4)) || !strncmp(s->cur, "NNNN", 4)) {
        if (!strncmp(s->cur, "NNNN", 4)) {
            s->copies      = 0;
            s->quiet_until = s->n + (long)(AFTER_S * s->rate);
            snprintf(out, out_n, "NNNN");
            decided = true;
        } else {
            if (s->copies == 0) s->first_at = s->n;
            if (s->copies < COPIES) snprintf(s->copy[s->copies++], SAME_MAX + 1, "%s", s->cur);
            if (s->copies >= 2 && vote(s, out, out_n)) {
                s->copies      = 0;
                s->quiet_until = s->n + (long)(AFTER_S * s->rate);
                decided        = true;
            } else if (s->copies == COPIES) {
                s->copies = 0;      /* three that do not agree: nothing said */
            }
        }
    }
    s->len  = 0;
    s->bits = -1;
    return decided;
}

static bool bit(same_t *s, int b, char *out, size_t out_n) {
    s->shift = (uint8_t)((s->shift >> 1) | (b ? 0x80 : 0));
    if (s->bits < 0) {
        if (s->shift == 0xAB) s->bits = 0;      /* a preamble byte: the boundary */
        return false;
    }
    if (++s->bits < 8) return false;
    s->bits = 0;

    int c = s->shift;

    if (c == 0xAB && s->len == 0) return false;     /* still the preamble */
    if (!header_char(c) || s->len >= SAME_MAX) return s->len ? copy_done(s, out, out_n) : (s->bits = -1, false);
    s->cur[s->len++] = (char)c;
    if (s->len >= 4 && !strncmp(s->cur, "NNNN", 4)) return copy_done(s, out, out_n);
    /* a header ends exactly: after the "+", its duration, issue time and sender, each closed by "-"; what follows
     * (the attention tone, noise) is no part of it */
    const char *plus = memchr(s->cur, '+', (size_t)s->len);
    if (plus) {
        int dashes = 0;
        for (const char *q = plus; q < s->cur + s->len; ++q) dashes += *q == '-';
        if (dashes == 3) return copy_done(s, out, out_n);
    }
    return false;
}

bool same_feed(same_t *s, const float *x, size_t n, char *out, size_t out_n) {
    bool decided = false;

    for (size_t i = 0; i < n; ++i) {
        float  v = x[i], old = s->hist[s->at];
        double km = s->wm * (double)s->n, ks = s->ws * (double)s->n;
        double kmo = s->wm * (double)(s->n - s->nwin), kso = s->ws * (double)(s->n - s->nwin);

        s->hist[s->at] = v;
        s->at          = (s->at + 1) % s->nwin;
        s->mr += v * cos(km) - old * cos(kmo);
        s->mi += v * sin(km) - old * sin(kmo);
        s->sr += v * cos(ks) - old * cos(kso);
        s->si += v * sin(ks) - old * sin(kso);
        s->n++;
        if ((s->n & 0xffff) == 0) {         /* the sums kept from drifting: recomputed now and then */
            s->mr = s->mi = s->sr = s->si = 0;
            for (int k = 0; k < s->nwin; ++k) {
                long   t = s->n - s->nwin + k;
                double h = s->hist[(s->at + k) % s->nwin];
                s->mr += h * cos(s->wm * t);
                s->mi += h * sin(s->wm * t);
                s->sr += h * cos(s->ws * t);
                s->si += h * sin(s->ws * t);
            }
        }
        double pm = s->mr * s->mr + s->mi * s->mi, ps = s->sr * s->sr + s->si * s->si;
        int    tone = pm > ps;

        /* the clock: a tone change is a bit boundary, at phase 0; pulled a quarter of the way there */
        if (tone != s->last) {
            float e = s->phase > 0.5f ? s->phase - 1.0f : s->phase;
            s->phase -= 0.25f * e;
            s->last = tone;
        }
        float before = s->phase;
        s->phase += 1.0f / s->spb;
        if (s->phase >= 1.0f) s->phase -= 1.0f;
        /* the middle of a bit: the window (one bit long, behind) is centred on it then */
        if (before < 0.5f && s->phase >= 0.5f)
            if (bit(s, tone, out, out_n)) decided = true;

        /* a copy left alone: taken by itself */
        if (s->copies == 1 && (float)(s->n - s->first_at) > LONE_S * s->rate) {
            snprintf(out, out_n, "%s", s->copy[0]);
            s->copies = 0;
            decided   = true;
        }
    }
    return decided;
}

const char *same_event_name(const char *code) {
    static const char *const t[][2] = {
        {"TOR", "TORNADO WARNING"}, {"TOA", "TORNADO WATCH"}, {"SVR", "SEVERE THUNDERSTORM WARNING"},
        {"SVA", "SEVERE THUNDERSTORM WATCH"}, {"SVS", "SEVERE WEATHER STATEMENT"}, {"SPS", "SPECIAL WEATHER STATEMENT"},
        {"FFW", "FLASH FLOOD WARNING"}, {"FFA", "FLASH FLOOD WATCH"}, {"FFS", "FLASH FLOOD STATEMENT"},
        {"FLW", "FLOOD WARNING"}, {"FLA", "FLOOD WATCH"}, {"FLS", "FLOOD STATEMENT"},
        {"WSW", "WINTER STORM WARNING"}, {"WSA", "WINTER STORM WATCH"}, {"BZW", "BLIZZARD WARNING"},
        {"HWW", "HIGH WIND WARNING"}, {"HWA", "HIGH WIND WATCH"}, {"EWW", "EXTREME WIND WARNING"},
        {"HUW", "HURRICANE WARNING"}, {"HUA", "HURRICANE WATCH"}, {"HLS", "HURRICANE STATEMENT"},
        {"TRW", "TROPICAL STORM WARNING"}, {"TRA", "TROPICAL STORM WATCH"}, {"SSW", "STORM SURGE WARNING"},
        {"SSA", "STORM SURGE WATCH"}, {"SMW", "SPECIAL MARINE WARNING"}, {"TSW", "TSUNAMI WARNING"},
        {"TSA", "TSUNAMI WATCH"}, {"DSW", "DUST STORM WARNING"}, {"AVW", "AVALANCHE WARNING"},
        {"AVA", "AVALANCHE WATCH"}, {"FRW", "FIRE WARNING"}, {"EQW", "EARTHQUAKE WARNING"},
        {"VOW", "VOLCANO WARNING"}, {"CEM", "CIVIL EMERGENCY MESSAGE"}, {"CDW", "CIVIL DANGER WARNING"},
        {"EVI", "EVACUATION IMMEDIATE"}, {"SPW", "SHELTER IN PLACE WARNING"}, {"LAE", "LOCAL AREA EMERGENCY"},
        {"LEW", "LAW ENFORCEMENT WARNING"}, {"HMW", "HAZARDOUS MATERIALS WARNING"}, {"NUW", "NUCLEAR POWER PLANT WARNING"},
        {"RHW", "RADIOLOGICAL HAZARD WARNING"}, {"TOE", "911 TELEPHONE OUTAGE"}, {"BLU", "BLUE ALERT"},
        {"CAE", "CHILD ABDUCTION EMERGENCY"}, {"EAN", "EMERGENCY ACTION NOTIFICATION"}, {"NPT", "NATIONAL PERIODIC TEST"},
        {"RWT", "REQUIRED WEEKLY TEST"}, {"RMT", "REQUIRED MONTHLY TEST"}, {"DMO", "PRACTICE / DEMO"},
        {"ADR", "ADMINISTRATIVE MESSAGE"}, {"NMN", "NETWORK MESSAGE NOTIFICATION"},
    };
    for (size_t i = 0; code && i < sizeof(t) / sizeof(t[0]); ++i)
        if (!strncmp(code, t[i][0], 3)) return t[i][1];
    return NULL;
}
