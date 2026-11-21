/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the V.27 ter transmitter (v27ter.h), ported from BinModem.
 */
#include "v27ter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Table 1: a tribit's phase change in eighths of a turn, and back */
const uint8_t v27_tribit_turn[8] = {1, 0, 2, 3, 6, 7, 5, 4};
const uint8_t v27_turn_tribit[8] = {1, 0, 2, 3, 7, 6, 4, 5};
/* Table 2 */
const uint8_t v27_dibit_turn[4] = {0, 2, 6, 4};
const uint8_t v27_turn_dibit[4] = {0, 1, 3, 2};

static const bool TRAINING_SEED[7] = {false, true, true, true, true, false, false};

static bool taps(const v27_scrambler_t *s) {
    return ((s->history >> 5) & 1) != ((s->history >> 6) & 1);
}

static void push(v27_scrambler_t *s, bool bit) {
    s->history = (uint8_t)(((s->history << 1) | bit) & 0x7f);
}

void v27_scrambler_seed(v27_scrambler_t *s) {
    s->history = 0;
    for (int i = 0; i < 7; i++)
        push(s, TRAINING_SEED[i]);
}

bool v27_scramble(v27_scrambler_t *s, bool bit) {
    bool out = bit ^ taps(s);
    push(s, out);
    return out;
}

bool v27_descramble(v27_scrambler_t *s, bool bit) {
    bool out = bit ^ taps(s);
    push(s, bit);
    return out;
}

static rrc_table_t *shared_rrc(void) {
    static rrc_table_t *r;
    if (!r) {
        r = malloc(sizeof(*r));
        rrc_table_init(r, V27_ROLLOFF, V27_SPAN);
    }
    return r;
}

void v27_tx_init(v27_tx_t *t, double fs) {
    memset(t, 0, sizeof(*t));
    t->fs         = fs;
    t->r4800      = true;
    t->long_train = true;
    nco_init(&t->nco, V27_CARRIER, fs);
    t->rrc = shared_rrc();
}

void v27_tx_start(v27_tx_t *t, bool r4800, bool long_train) {
    t->r4800      = r4800;
    t->long_train = long_train;
    v27_scrambler_seed(&t->scrambler);
    t->stage   = V27_REVERSALS;
    t->left    = v27_reversals(long_train);
    t->eighths = 0;
    t->phase   = 0.0;
    memset(t->history, 0, sizeof(t->history));
    bitq_clear(&t->pending);
}

void v27_tx_stop(v27_tx_t *t) {
    if (t->stage != V27_SILENT && t->stage != V27_TURNOFF) {
        t->stage = V27_TURNOFF;
        t->left  = V27_TURN_OFF;
    }
}

static cplx turn(v27_tx_t *t, uint8_t eighths) {
    t->eighths   = (t->eighths + eighths) & 7;
    double angle = TAU * t->eighths / 8.0;
    return cos(angle) + I * sin(angle);
}

static bool training_bit(v27_tx_t *t) {
    return v27_scramble(&t->scrambler, true);
}

static bool next_bit(v27_tx_t *t) {
    bool bit;
    if (!bitq_pop(&t->pending, &bit))
        bit = true;
    return v27_scramble(&t->scrambler, bit);
}

static cplx coded(v27_tx_t *t, bool ones) {
    if (t->r4800) {
        bool a = ones ? training_bit(t) : next_bit(t);
        bool b = ones ? training_bit(t) : next_bit(t);
        bool c = ones ? training_bit(t) : next_bit(t);
        return turn(t, v27_tribit_turn[a << 2 | b << 1 | c]);
    }
    bool a = ones ? training_bit(t) : next_bit(t);
    bool b = ones ? training_bit(t) : next_bit(t);
    return turn(t, v27_dibit_turn[a << 1 | b]);
}

cplx v27_tx_symbol(v27_tx_t *t) {
    switch (t->stage) {
    case V27_SILENT:
        return 0;
    case V27_REVERSALS:
        if (--t->left == 0) {
            t->stage = V27_CONDITIONING;
            t->left  = v27_conditioning(t->long_train);
        }
        return turn(t, 4);
    case V27_CONDITIONING: {
        if (--t->left == 0) {
            t->stage = V27_ONES;
            t->left  = V27_SCRAMBLED_ONES;
        }
        /* 2.5.1.2: every third bit of the scrambled ONEs, ZERO no change, ONE a reversal */
        bool bit = training_bit(t);
        training_bit(t);
        training_bit(t);
        return turn(t, bit ? 4 : 0);
    }
    case V27_ONES:
        if (--t->left == 0)
            t->stage = V27_DATA;
        return coded(t, true);
    case V27_DATA:
        return coded(t, false);
    case V27_TURNOFF:
        if (bitq_len(&t->pending))
            return coded(t, false);
        if (--t->left == 0)
            t->stage = V27_SILENT;
        return coded(t, true);
    }
    return 0;
}

double v27_tx_sample(v27_tx_t *t) {
    double c, s;

    if (t->stage == V27_SILENT) {
        nco_step(&t->nco, &c, &s);
        return 0.0;
    }
    t->phase += v27_baud(t->r4800) / t->fs;
    while (t->phase >= 1.0) {
        t->phase -= 1.0;
        memmove(t->history, t->history + 1, sizeof(t->history) - sizeof(t->history[0]));
        cplx sym                       = v27_tx_symbol(t);
        t->history[2 * V27_SPAN]       = sym;
        t->sent                        = sym;
    }
    cplx bb = 0;
    for (int i = 0; i <= 2 * V27_SPAN; i++)
        bb += t->history[i] * rrc_table_at(t->rrc, t->phase + V27_SPAN - i);
    nco_step(&t->nco, &c, &s);
    return creal(bb) * c - cimag(bb) * s;
}
