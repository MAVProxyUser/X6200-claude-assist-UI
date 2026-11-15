/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - RDS from a WFM channel's multiplex (rds.h).
 *
 *  The chain, at the multiplex's rate (264.6 kHz):
 *    - the 57 kHz subcarrier mixed to 0 Hz by an oscillator of its own (the station's 57 kHz is three times its
 *      19 kHz pilot, but a Costas loop further on takes whatever phase and the few hertz the stick's clock puts on it);
 *    - a CIC of order 3 down by 24 to 11025 Hz, in integers (it wraps as it should), then a 41-tap low pass at 2.6 kHz:
 *      the stereo difference signal (23-53 kHz), the pilot and an SCA subcarrier at 67 kHz kept out of it;
 *    - the Costas loop (about 10 Hz wide): the BPSK's carrier, the signal on the real axis;
 *    - the chips (2375 a second, two a bit) integrated over their own time, the sample at the edge shared between
 *      the two it straddles, and the chip clock kept on them by Gardner's error (the integral over the half chips
 *      either side of a boundary: zero there when the clock is right, where the chips change);
 *    - the chips paired into bits: a biphase bit is always two chips of opposite sign, so of the two ways to pair
 *      them the one with fewer equal pairs is taken (a slip of one chip is mended within a few dozen bits);
 *    - the bits decoded differentially (each the change from the one before: the Costas loop's 180 degree
 *      ambiguity drops out), then the blocks found by their checkword and offset word, four a group.
 *  Nothing is corrected: a block that fails its check is dropped, and what it carried waits for the next group
 *  that has it (the name comes in four parts, taken only in their order, a RadioText in sixteen).
 */

#include "rds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEC    24               /* 264600 -> 11025 */
#define NT     41               /* the low pass's taps at 11025 Hz */
#define CHIP   2375.0f

static const uint16_t OFFSET[5] = {0x0FC, 0x198, 0x168, 0x350, 0x1B4};   /* A, B, C, C', D */

struct rds_s {
    float rate, rate2;
    /* the 57 kHz oscillator, a rotator renormalised now and then */
    float osc_r, osc_i, step_r, step_i;
    int   osc_n;
    /* the CIC, order 3, in integers */
    uint32_t i1r, i2r, i3r, i1i, i2i, i3i, c1r, c2r, c3r, c1i, c2i, c3i;
    int      cic_n;
    /* the low pass at 11025 Hz */
    float hr[NT], hi[NT], taps[NT];
    int   h_at;
    /* the Costas loop */
    float ph, fr, agc;
    /* the chips: their clock, what is being integrated, the last ones */
    float cp, step, chip_acc, mid_acc, mid_last, chip_last, energy;
    bool  have_mid;
    uint32_t chip_n;
    float pair_bad[2];
    /* the bits */
    int      last_bit;
    uint32_t reg;
    uint64_t bit_n;
    /* the blocks */
    bool     synced;
    int      expect, in_block, bad_run, prev_idx;
    uint64_t prev_pos;
    uint16_t blk[4];
    bool     ok[4];
    /* what they said */
    char     ps_buf[8], rt_buf[64];
    uint8_t  ps_seen;
    uint16_t rt_seen;
    int      rt_ab, rt_len;      /* -1: no A/B flag yet; the RadioText's length when it ended with a carriage return */
    bool     rt_b;               /* the RadioText of version B (32 characters) */
    rds_info_t info;
};

static void design(float *taps, float cutoff, float rate) {
    float fc = cutoff / rate, sum = 0;

    for (int i = 0; i < NT; i++) {
        int   m = i - NT / 2;
        float s = m ? sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m) : 2 * fc;
        float w = 0.42f - 0.5f * cosf(2 * (float)M_PI * i / (NT - 1)) + 0.08f * cosf(4 * (float)M_PI * i / (NT - 1));
        taps[i] = s * w;
        sum += taps[i];
    }
    for (int i = 0; i < NT; i++) taps[i] /= sum;
}

rds_t *rds_new(float rate) {
    rds_t *r = calloc(1, sizeof(*r));

    if (!r) return NULL;
    r->rate  = rate;
    r->rate2 = rate / DEC;
    r->step_r = cosf(-2 * (float)M_PI * 57000 / rate);
    r->step_i = sinf(-2 * (float)M_PI * 57000 / rate);
    design(r->taps, 2600, r->rate2);
    rds_reset(r);
    return r;
}

void rds_free(rds_t *r) {
    free(r);
}

void rds_reset(rds_t *r) {
    float rate = r->rate, rate2 = r->rate2, sr = r->step_r, si = r->step_i, taps[NT];

    memcpy(taps, r->taps, sizeof(taps));
    memset(r, 0, sizeof(*r));
    r->rate = rate;
    r->rate2 = rate2;
    r->step_r = sr;
    r->step_i = si;
    memcpy(r->taps, taps, sizeof(taps));
    r->osc_r = 1;
    r->agc = 1e-6f;
    r->step = CHIP / rate2;
    r->prev_idx = -1;
    r->rt_ab = -1;
    r->info.pty = -1;
    memset(r->ps_buf, ' ', sizeof(r->ps_buf));
    memset(r->rt_buf, ' ', sizeof(r->rt_buf));
}

/* ---- The blocks and groups ---- */

static uint16_t crc10(uint16_t info) {
    uint32_t reg = (uint32_t)info << 10;

    for (int i = 25; i >= 10; i--)
        if (reg & (1u << i)) reg ^= 0x5B9u << (i - 10);
    return (uint16_t)(reg & 0x3FF);
}

/* Which block a 26-bit word is (0 A, 1 B, 2 C, 3 C', 4 D), -1 none */
static int block_kind(uint32_t w) {
    uint16_t off = (uint16_t)((w & 0x3FF) ^ crc10((uint16_t)(w >> 10)));

    for (int k = 0; k < 5; k++)
        if (off == OFFSET[k]) return k;
    return -1;
}

static char shown(uint8_t c) {
    return c >= 0x20 && c < 0x7F ? (char)c : ' ';
}

static bool set_text(char *dst, size_t n, const char *src, size_t len) {
    char t[65];

    if (len >= sizeof(t)) len = sizeof(t) - 1;
    memcpy(t, src, len);
    t[len] = 0;
    while (len && t[len - 1] == ' ') t[--len] = 0;
    if (!strncmp(dst, t, n)) return false;
    snprintf(dst, n, "%s", t);
    return true;
}

/* The RadioText as far as it is whole: up to its carriage return, or all 64 (32) characters once every part came */
static bool rt_publish(rds_t *r) {
    int parts = r->rt_b ? 16 : 16, per = r->rt_b ? 2 : 4, len = r->rt_len >= 0 ? r->rt_len : parts * per;
    int need = (len + per - 1) / per;

    if (!len || need > 16) return false;
    for (int i = 0; i < need; i++)
        if (!(r->rt_seen & (1u << i))) return false;
    return set_text(r->info.rt, sizeof(r->info.rt), r->rt_buf, (size_t)len);
}

static bool group(rds_t *r) {
    bool     changed = false;
    uint16_t b = r->blk[1], c = r->blk[2], d = r->blk[3];

    if (r->ok[0] && r->blk[0] != r->info.pi) {
        r->info.pi = r->blk[0];
        changed = true;
    }
    if (!r->ok[1]) return changed;
    int  type = b >> 12;
    bool ver_b = b & 0x800;

    r->info.groups++;
    if ((int)((b >> 5) & 31) != r->info.pty || (bool)(b & 0x400) != r->info.tp) {
        r->info.pty = (b >> 5) & 31;
        r->info.tp = b & 0x400;
        changed = true;
    }
    if (type == 0) {                    /* 0A, 0B: two of the name's characters */
        int seg = b & 3;

        if ((bool)(b & 0x10) != r->info.ta) {
            r->info.ta = b & 0x10;
            changed = true;
        }
        if (r->ok[3]) {
            r->ps_buf[seg * 2] = shown(d >> 8);
            r->ps_buf[seg * 2 + 1] = shown(d & 0xFF);
            /* the four parts in their order, none missed between: a name that scrolls 8 characters at a time
             * (a song's title, "CARSON" "DALY" ...) is never shown half one frame and half the next */
            if (seg == 0) r->ps_seen = 1;
            else if (r->ps_seen == (1u << seg) - 1) r->ps_seen |= 1 << seg;
            else r->ps_seen = 0;
            if (r->ps_seen == 15) {     /* all four: shown, and four new ones for the next */
                char ps[9];

                memcpy(ps, r->ps_buf, 8);
                ps[8] = 0;
                if (strcmp(ps, r->info.ps)) {
                    memcpy(r->info.ps, ps, 9);
                    changed = true;
                }
                r->ps_seen = 0;
            }
        }
    } else if (type == 2) {             /* 2A: four characters of 64; 2B: two of 32 */
        int addr = b & 15, ab = (b >> 4) & 1;

        if (ab != r->rt_ab || ver_b != r->rt_b) {      /* a new text */
            memset(r->rt_buf, ' ', sizeof(r->rt_buf));
            r->rt_seen = 0;
            r->rt_len = -1;
            r->rt_ab = ab;
            r->rt_b = ver_b;
        }
        uint8_t ch[4];
        int     n = 0;

        if (!ver_b && r->ok[2] && r->ok[3]) {
            ch[0] = c >> 8, ch[1] = c & 0xFF, ch[2] = d >> 8, ch[3] = d & 0xFF;
            n = 4;
        } else if (ver_b && r->ok[3]) {
            ch[0] = d >> 8, ch[1] = d & 0xFF;
            n = 2;
        }
        for (int i = 0; i < n; i++) {
            int at = addr * n + i;

            if (ch[i] == 0x0D) {        /* the end */
                if (r->rt_len < 0 || at < r->rt_len) r->rt_len = at;
                break;
            }
            r->rt_buf[at] = shown(ch[i]);
        }
        if (n) {
            r->rt_seen |= 1u << addr;
            changed |= rt_publish(r);
        }
    }
    return changed;
}

/* One data bit */
static bool bit_in(rds_t *r, int bit) {
    bool changed = false;

    r->reg = ((r->reg << 1) | (uint32_t)bit) & 0x3FFFFFF;
    r->bit_n++;
    if (!r->synced) {
        int k = block_kind(r->reg);

        if (k < 0) return false;
        int idx = k == 3 ? 2 : k == 4 ? 3 : k, step = (idx - r->prev_idx + 4) % 4;

        /* two blocks the right distance apart, in their order: in step from here */
        if (r->prev_idx >= 0 && step && r->bit_n - r->prev_pos == (uint64_t)(26 * step)) {
            r->synced = true;
            r->bad_run = 0;
            r->in_block = 0;
            memset(r->ok, 0, sizeof(r->ok));
            r->blk[idx] = (uint16_t)(r->reg >> 10);
            r->ok[idx] = true;
            r->expect = (idx + 1) % 4;
            if (idx == 3) changed = group(r);
        }
        r->prev_idx = idx;
        r->prev_pos = r->bit_n;
        return changed;
    }
    if (++r->in_block < 26) return false;
    r->in_block = 0;

    int  k = block_kind(r->reg);
    bool ok = r->expect == 2 ? (k == 2 || k == 3) : r->expect == 3 ? k == 4 : k == r->expect;   /* C or C'; D */

    r->blk[r->expect] = (uint16_t)(r->reg >> 10);
    r->ok[r->expect] = ok;
    r->info.blocks++;
    if (!ok) {
        r->info.bad++;
        r->bad_run++;
    } else {
        r->bad_run = 0;
    }
    if (r->expect == 3) {
        changed = group(r);
        memset(r->ok, 0, sizeof(r->ok));
    }
    r->expect = (r->expect + 1) % 4;
    if (r->bad_run >= 20) {             /* out of step (or the station gone): looked for again */
        r->synced = false;
        r->prev_idx = -1;
    }
    return changed;
}

/* One chip's integral */
static bool chip_in(rds_t *r, float c) {
    bool changed = false;

    /* Gardner: the half chips either side of the boundary just passed against the change across it */
    if (r->have_mid) {
        float e = r->mid_last * (r->chip_last - c);

        r->energy += 0.01f * (c * c - r->energy);
        r->cp -= 0.01f * e / (r->energy + 1e-12f);     /* late (e < 0): the next boundary sooner */
    }
    /* the pairing: a bit's two chips have opposite signs */
    int   way = r->chip_n & 1;
    float lead = r->chip_last;

    r->pair_bad[way] = r->pair_bad[way] * 0.97f + ((lead > 0) == (c > 0) ? 1 : 0);
    if (r->chip_n && r->pair_bad[way] <= r->pair_bad[way ^ 1]) {
        int sym = lead > c, bit = sym ^ r->last_bit;

        r->last_bit = sym;
        changed = bit_in(r, bit);
    }
    r->chip_last = c;
    r->chip_n++;
    return changed;
}

/* One sample at 11025 Hz: the carrier, then the chips (a sample is spread over the time it covers) */
static bool base_in(rds_t *r, float zr, float zi) {
    bool  changed = false;
    float cr = cosf(r->ph), ci = sinf(r->ph);
    float ur = zr * cr + zi * ci, ui = zi * cr - zr * ci;

    r->agc += 0.002f * (ur * ur + ui * ui - r->agc);
    float e = ur * ui / (r->agc + 1e-12f);

    r->fr += 3.2e-5f * e;
    if (r->fr > 0.01f) r->fr = 0.01f;   /* +-17 Hz */
    if (r->fr < -0.01f) r->fr = -0.01f;
    r->ph += r->fr + 0.008f * e;
    if (r->ph > (float)M_PI) r->ph -= 2 * (float)M_PI;
    if (r->ph < -(float)M_PI) r->ph += 2 * (float)M_PI;

    float rem = r->step;

    while (rem > 1e-7f) {
        float edge = r->cp < 0.5f ? 0.5f : 1.0f;
        float take = edge - r->cp < rem ? edge - r->cp : rem;
        float part = ur * take / r->step;

        r->chip_acc += part;
        r->mid_acc += part;
        r->cp += take;
        rem -= take;
        if (r->cp >= edge - 1e-6f) {
            if (edge == 0.5f) {
                r->mid_last = r->mid_acc;
                r->mid_acc = 0;
            } else {
                float c = r->chip_acc;

                r->chip_acc = 0;
                r->cp = 0;
                changed |= chip_in(r, c);
                r->have_mid = true;
                if (r->cp > 0.45f) r->cp = 0.45f;   /* Gardner's nudge, kept within the chip */
                if (r->cp < -0.45f) r->cp = -0.45f;
            }
        }
    }
    return changed;
}

bool rds_feed(rds_t *r, const float *x, size_t n) {
    bool changed = false;

    for (size_t k = 0; k < n; k++) {
        float mr = x[k] * r->osc_r, mi = x[k] * r->osc_i;
        float nr = r->osc_r * r->step_r - r->osc_i * r->step_i;

        r->osc_i = r->osc_r * r->step_i + r->osc_i * r->step_r;
        r->osc_r = nr;
        if (++r->osc_n == 4096) {       /* back to unit length */
            float g = 1.0f / sqrtf(r->osc_r * r->osc_r + r->osc_i * r->osc_i);

            r->osc_r *= g;
            r->osc_i *= g;
            r->osc_n = 0;
        }
        /* the CIC: +-2 at most in, 16384 a unit, 24^3 its gain: within 32 bits */
        r->i1r += (uint32_t)(int32_t)lrintf(mr * 16384);
        r->i1i += (uint32_t)(int32_t)lrintf(mi * 16384);
        r->i2r += r->i1r;
        r->i2i += r->i1i;
        r->i3r += r->i2r;
        r->i3i += r->i2i;
        if (++r->cic_n < DEC) continue;
        r->cic_n = 0;

        uint32_t ar = r->i3r - r->c1r, ai = r->i3i - r->c1i;
        r->c1r = r->i3r;
        r->c1i = r->i3i;
        uint32_t br = ar - r->c2r, bi = ai - r->c2i;
        r->c2r = ar;
        r->c2i = ai;
        uint32_t cr = br - r->c3r, ci = bi - r->c3i;
        r->c3r = br;
        r->c3i = bi;

        const float g = 1.0f / (16384.0f * DEC * DEC * DEC);
        r->hr[r->h_at] = (int32_t)cr * g;
        r->hi[r->h_at] = (int32_t)ci * g;
        r->h_at = (r->h_at + 1) % NT;

        float yr = 0, yi = 0;
        for (int t = 0; t < NT; t++) {
            int h = (r->h_at + t) % NT;
            yr += r->hr[h] * r->taps[t];
            yi += r->hi[h] * r->taps[t];
        }
        changed |= base_in(r, yr, yi);
    }
    return changed;
}

void rds_info(const rds_t *r, rds_info_t *out) {
    *out = r->info;
}

void rds_call_letters(uint16_t pi, char out[5]) {
    int n;

    out[0] = 0;
    if (pi >= 0x1000 && pi <= 0x54A7) {
        n = pi - 0x1000;
        out[0] = 'K';
    } else if (pi >= 0x54A8 && pi <= 0x994F) {
        n = pi - 0x54A8;
        out[0] = 'W';
    } else {
        return;
    }
    out[1] = (char)('A' + n / 676);
    out[2] = (char)('A' + n % 676 / 26);
    out[3] = (char)('A' + n % 26);
    out[4] = 0;
}

const char *rds_pty_name(int pty, bool rbds) {
    static const char *const RBDS[32] = {
        "",           "NEWS",        "INFORMATION", "SPORTS",       "TALK",          "ROCK",        "CLASSIC ROCK",
        "ADULT HITS", "SOFT ROCK",   "TOP 40",      "COUNTRY",      "OLDIES",        "SOFT",        "NOSTALGIA",
        "JAZZ",       "CLASSICAL",   "R&B",         "SOFT R&B",     "LANGUAGE",      "RELIGIOUS MUSIC",
        "RELIGIOUS TALK", "PERSONALITY", "PUBLIC",  "COLLEGE",      "SPANISH TALK",  "SPANISH MUSIC", "HIP HOP",
        "",           "",            "WEATHER",     "EMERGENCY TEST", "EMERGENCY"};
    static const char *const RDS[32] = {
        "",            "NEWS",        "CURRENT AFFAIRS", "INFORMATION", "SPORT",       "EDUCATION",   "DRAMA",
        "CULTURE",     "SCIENCE",     "VARIED",      "POP MUSIC",    "ROCK MUSIC",    "EASY LISTENING",
        "LIGHT CLASSICAL", "SERIOUS CLASSICAL", "OTHER MUSIC", "WEATHER", "FINANCE",  "CHILDREN'S",
        "SOCIAL AFFAIRS", "RELIGION", "PHONE IN",    "TRAVEL",       "LEISURE",       "JAZZ",        "COUNTRY",
        "NATIONAL MUSIC", "OLDIES",  "FOLK MUSIC",  "DOCUMENTARY",  "ALARM TEST",    "ALARM"};

    return pty < 0 || pty > 31 ? "" : rbds ? RBDS[pty] : RDS[pty];
}
