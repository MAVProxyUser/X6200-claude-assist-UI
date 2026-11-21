/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the V.29 transmitter (v29.h), ported from BinModem.
 */
#include "v29.h"
#include "v27ter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

cplx v29_xy(v29_point_t p) {
    uint8_t e = p.eighths & 7;
    if (e % 2 == 0) {
        double r = p.outer ? 5.0 : 3.0;
        switch (e) {
        case 0: return r;
        case 2: return I * r;
        case 4: return -r;
        default: return -I * r;
        }
    }
    double k = p.outer ? 3.0 : 1.0;
    switch (e) {
    case 1: return k + I * k;
    case 3: return -k + I * k;
    case 5: return -k - I * k;
    default: return k - I * k;
    }
}

int v29_constellation(v29_rate_t r, cplx *unit, v29_point_t *pts) {
    int    n   = 0;
    double rms = v29_rms(r);
    for (int ring = 0; ring < (r == V29_9600 ? 2 : 1); ring++)
        for (uint8_t e = 0; e < 8; e++) {
            if (r == V29_4800 && e % 2)
                continue;
            v29_point_t p = {e, ring == 1};
            if (pts)
                pts[n] = p;
            if (unit)
                unit[n] = v29_xy(p) / rms;
            n++;
        }
    return n;
}

double v29_rms(v29_rate_t r) {
    switch (r) {
    case V29_9600: return sqrt(13.5);
    case V29_7200: return sqrt(5.5);
    default: return 3.0;
    }
}

int v29_bits(v29_rate_t r) {
    return r == V29_9600 ? 4 : r == V29_7200 ? 3 : 2;
}

static bool v32_feedback(const v32_scrambler_t *s) {
    return (((s->reg >> 17) & 1) ^ ((s->reg >> 22) & 1)) != 0;
}

bool v32_scramble(v32_scrambler_t *s, bool bit) {
    bool out = bit ^ v32_feedback(s);
    s->reg   = (s->reg << 1) | out;
    return out;
}

bool v32_descramble(v32_scrambler_t *s, bool bit) {
    bool out = bit ^ v32_feedback(s);
    s->reg   = (s->reg << 1) | bit;
    return out;
}

static const v29_point_t A = {4, false}, C = {0, false};

static v29_point_t point_b(v29_rate_t r) {
    return r == V29_4800 ? (v29_point_t){6, false} : r == V29_7200 ? (v29_point_t){7, false} : (v29_point_t){7, true};
}

static v29_point_t point_d(v29_rate_t r) {
    return r == V29_4800 ? (v29_point_t){2, false} : r == V29_7200 ? (v29_point_t){3, false} : (v29_point_t){3, true};
}

/* "The initial condition of the generator is 0101010"; a ZERO sends C and a ONE sends D */
static bool cond_bit(v29_tx_t *t) {
    bool    out = t->cond & 1;
    uint8_t fed = (t->cond ^ (t->cond >> 1)) & 1;
    t->cond     = (uint8_t)((t->cond >> 1) | (fed << 6));
    return out;
}

static rrc_table_t *shared_rrc(void) {
    static rrc_table_t *r;
    if (!r) {
        r = malloc(sizeof(*r));
        rrc_table_init(r, V29_ROLLOFF, V29_SPAN);
    }
    return r;
}

void v29_tx_init(v29_tx_t *t, double fs) {
    memset(t, 0, sizeof(*t));
    t->fs    = fs;
    t->point = C;
    nco_init(&t->nco, V29_CARRIER, fs);
    t->rrc = shared_rrc();
}

void v29_tx_start(v29_tx_t *t, v29_rate_t rate) {
    t->rate          = rate;
    t->scrambler.reg = 0;
    t->cond          = 0x2a;
    t->stage         = V29_QUIET;
    t->left          = V29_SILENCE;
    t->point         = C;
    t->phase         = 0.0;
    memset(t->history, 0, sizeof(t->history));
    bitq_clear(&t->pending);
}

void v29_tx_stop(v29_tx_t *t) {
    if (t->stage != V29_SILENT && t->stage != V29_TURNOFF) {
        t->stage = V29_TURNOFF;
        t->left  = V29_TURN_OFF;
    }
}

static bool data_bit(v29_tx_t *t, bool ones) {
    bool bit = true;
    if (!ones && !bitq_pop(&t->pending, &bit))
        bit = true;
    return v32_scramble(&t->scrambler, bit);
}

/* 2.2: Q1 picks the ring, Q2 Q3 Q4 the phase change */
static v29_point_t coded(v29_tx_t *t, bool ones) {
    bool q1 = false, q2, q3, q4;
    switch (t->rate) {
    case V29_9600:
        q1 = data_bit(t, ones);
        q2 = data_bit(t, ones);
        q3 = data_bit(t, ones);
        q4 = data_bit(t, ones);
        break;
    case V29_7200:
        q2 = data_bit(t, ones);
        q3 = data_bit(t, ones);
        q4 = data_bit(t, ones);
        break;
    default:
        q2 = data_bit(t, ones);
        q3 = data_bit(t, ones);
        q4 = !(q2 ^ q3);
        break;
    }
    int tribit = q2 << 2 | q3 << 1 | q4;
    t->point   = (v29_point_t){(uint8_t)((t->point.eighths + v27_tribit_turn[tribit]) & 7), q1};
    return t->point;
}

bool v29_tx_symbol(v29_tx_t *t, v29_point_t *out) {
    v29_point_t p;

    switch (t->stage) {
    case V29_SILENT:
        return false;
    case V29_QUIET:
        if (--t->left == 0) {
            t->stage = V29_ALT;
            t->left  = V29_ALTERNATIONS;
        }
        return false;
    case V29_ALT:
        /* counting down from 128, so the first is even: A first */
        p = (V29_ALTERNATIONS - t->left) % 2 == 0 ? A : point_b(t->rate);
        if (--t->left == 0) {
            t->stage = V29_COND;
            t->left  = V29_CONDITIONING;
        }
        break;
    case V29_COND:
        if (--t->left == 0) {
            t->stage = V29_ONESEG;
            t->left  = V29_ONES;
        }
        p = cond_bit(t) ? point_d(t->rate) : C;
        break;
    case V29_ONESEG:
        if (--t->left == 0)
            t->stage = V29_DATA;
        *out = coded(t, true);
        return true;
    case V29_DATA:
        *out = coded(t, false);
        return true;
    case V29_TURNOFF:
        if (bitq_len(&t->pending)) {
            *out = coded(t, false);
            return true;
        }
        if (--t->left == 0)
            t->stage = V29_SILENT;
        *out = coded(t, true);
        return true;
    default:
        return false;
    }
    /* segments 2 and 3 send absolute points; the data that follows is a change from the last */
    t->point = p;
    *out     = p;
    return true;
}

double v29_tx_sample(v29_tx_t *t) {
    double c, s;

    if (t->stage == V29_SILENT) {
        nco_step(&t->nco, &c, &s);
        return 0.0;
    }
    t->phase += V29_BAUD / t->fs;
    while (t->phase >= 1.0) {
        v29_point_t p;
        t->phase -= 1.0;
        memmove(t->history, t->history + 1, sizeof(t->history) - sizeof(t->history[0]));
        cplx sym                 = v29_tx_symbol(t, &p) ? v29_xy(p) : 0;
        t->history[2 * V29_SPAN] = sym;
        t->sent                  = sym;
    }
    cplx bb = 0;
    for (int i = 0; i <= 2 * V29_SPAN; i++)
        bb += t->history[i] * rrc_table_at(t->rrc, t->phase + V29_SPAN - i);
    nco_step(&t->nco, &c, &s);
    return (creal(bb) * c - cimag(bb) * s) / v29_rms(t->rate);
}
