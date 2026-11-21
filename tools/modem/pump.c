/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the fast modems (pump.h).
 */
#include "pump.h"

#include <math.h>
#include <string.h>

int rate_bps(rate_t r) {
    static const int bps[RATE_COUNT] = {2400, 4800, 7200, 9600};
    return r < RATE_COUNT ? bps[r] : 0;
}

const char *rate_name(rate_t r) {
    static const char *names[RATE_COUNT] = {"V.27ter 2400", "V.27ter 4800", "V.29 7200", "V.29 9600"};
    return r < RATE_COUNT ? names[r] : "?";
}

static bool is_v29(rate_t r) {
    return r == RATE_7200 || r == RATE_9600;
}

static v29_rate_t v29_of(rate_t r) {
    return r == RATE_9600 ? V29_9600 : V29_7200;
}

void pump_init(pump_t *p, double fs, pump_bit_fn fn, void *ctx) {
    memset(p, 0, sizeof(*p));
    p->fs     = fs;
    p->on_bit = fn;
    p->ctx    = ctx;
    v27_tx_init(&p->v27, fs);
    v29_tx_init(&p->v29, fs);
}

void pump_free(pump_t *p) {
    if (p->rx_ready)
        qamrx_free(&p->rx);
    p->rx_ready = false;
}

void pump_tx_start(pump_t *p, rate_t r) {
    p->rate     = r;
    p->stopping = false;
    if (is_v29(r))
        v29_tx_start(&p->v29, v29_of(r));
    else
        v27_tx_start(&p->v27, r == RATE_4800, true);
}

bool pump_tx_push(pump_t *p, int bit) {
    return bitq_push(is_v29(p->rate) ? &p->v29.pending : &p->v27.pending, bit);
}

uint32_t pump_tx_pending(pump_t *p) {
    return bitq_len(is_v29(p->rate) ? &p->v29.pending : &p->v27.pending);
}

/* BinModem's stop() turns the carrier off from wherever the transmitter is, the turn-on included, so
 * it is called here only once the turn-on is over: what is queued goes out, then the turn-off */
void pump_tx_stop(pump_t *p) {
    p->stopping = true;
}

bool pump_tx_busy(pump_t *p) {
    return is_v29(p->rate) ? v29_tx_busy(&p->v29) : v27_tx_busy(&p->v27);
}

double pump_tx_sample(pump_t *p) {
    if (p->stopping) {
        if (is_v29(p->rate) && p->v29.stage == V29_DATA)
            v29_tx_stop(&p->v29);
        else if (!is_v29(p->rate) && p->v27.stage == V27_DATA)
            v27_tx_stop(&p->v27);
    }
    return is_v29(p->rate) ? v29_tx_sample(&p->v29) : v27_tx_sample(&p->v27);
}

double pump_train_seconds(rate_t r) {
    if (is_v29(r))
        return (V29_SILENCE + V29_ALTERNATIONS + V29_CONDITIONING + V29_ONES) / V29_BAUD;
    return (v27_reversals(true) + v27_conditioning(true) + V27_SCRAMBLED_ONES) / v27_baud(r == RATE_4800);
}

/* ---- receiving ---- */

static void out_bit(pump_t *p, int k, int bit) {
    if (k >= p->nknown) {
        p->bits_out++;
        if (p->on_bit)
            p->on_bit(p->ctx, bit);
    }
}

/* A symbol decided: as BinModem's v27ter Receiver::read and v29's carried, a change from the one
 * before, then descrambled. From the turn-on's coded symbols on, so the descrambler has its state by
 * the data; bits handed up from the data's first */
static void on_symbol(void *ctx, int k, int label, cplx y) {
    pump_t *p = ctx;
    (void)y;

    if (k < p->ref)
        return;
    if (k == p->ref || p->prev < 0) {
        p->prev = label;
        return;
    }
    if (is_v29(p->rate)) {
        v29_point_t now = p->v29pts[label], was = p->v29pts[p->prev];
        int         change = (now.eighths + 8 - was.eighths) & 7;
        int         t      = v27_turn_tribit[change];
        int         q[4]   = {now.outer, (t >> 2) & 1, (t >> 1) & 1, t & 1};
        for (int i = p->rate == RATE_9600 ? 0 : 1; i < 4; i++)
            out_bit(p, k, v32_descramble(&p->d32, q[i]));
    } else if (p->rate == RATE_4800) {
        int g = v27_turn_tribit[(label + 8 - p->prev) % 8];
        for (int i = 2; i >= 0; i--)
            out_bit(p, k, v27_descramble(&p->d27, (g >> i) & 1));
    } else {
        int g = v27_turn_dibit[(label + 4 - p->prev) % 4];
        for (int i = 1; i >= 0; i--)
            out_bit(p, k, v27_descramble(&p->d27, (g >> i) & 1));
    }
    p->prev = label;
}

void pump_rx_arm(pump_t *p, rate_t r) {
    qamrx_cfg_t c = {.fs = p->fs};
    int         n = 0;

    if (p->rx_ready && p->rate == r) {
        qamrx_reset(&p->rx);
        p->prev = -1;
        return;
    }
    if (p->rx_ready)
        qamrx_free(&p->rx);
    p->rate = r;
    if (is_v29(r)) {
        v29_tx_t   t;
        v29_point_t pt;
        v29_tx_init(&t, p->fs);
        v29_tx_start(&t, v29_of(r));
        double rms = v29_rms(v29_of(r));
        for (int i = 0; i < V29_SILENCE + V29_ALTERNATIONS + V29_CONDITIONING + V29_ONES; i++)
            if (v29_tx_symbol(&t, &pt))
                p->known[n++] = v29_xy(pt) / rms;
        c.baud       = V29_BAUD;
        c.carrier    = V29_CARRIER;
        c.rolloff    = V29_ROLLOFF;
        c.npts       = v29_constellation(v29_of(r), c.pts, p->v29pts);
        c.find_syms  = V29_ALTERNATIONS + 100;
        c.solve_from = V29_ALTERNATIONS + 8;
        c.solve_to   = V29_ALTERNATIONS + 330;
        c.accept_db  = 8.0;
        p->ref       = V29_ALTERNATIONS + V29_CONDITIONING - 1;
    } else {
        v27_tx_t t;
        bool     r48 = r == RATE_4800;
        v27_tx_init(&t, p->fs);
        v27_tx_start(&t, r48, true);
        int total = v27_reversals(true) + v27_conditioning(true) + V27_SCRAMBLED_ONES;
        for (int i = 0; i < total; i++)
            p->known[n++] = v27_tx_symbol(&t);
        c.baud    = v27_baud(r48);
        c.carrier = V27_CARRIER;
        c.rolloff = V27_ROLLOFF;
        c.npts    = r48 ? 8 : 4;
        for (int i = 0; i < c.npts; i++)
            c.pts[i] = cexp(I * TAU * i / c.npts);
        c.find_syms  = v27_reversals(true) + 150;
        c.solve_from = v27_reversals(true) + 10;
        c.solve_to   = v27_reversals(true) + 330;
        c.accept_db  = 8.0;
        p->ref       = v27_reversals(true) + v27_conditioning(true) - 1;
    }
    p->nknown = n;
    c.known   = p->known;
    c.nknown  = n;
    p->prev   = -1;
    qamrx_init(&p->rx, &c, on_symbol, p);
    p->rx_ready = true;
}

void pump_rx_feed(pump_t *p, double x) {
    if (p->rx_ready)
        qamrx_feed(&p->rx, x);
}
