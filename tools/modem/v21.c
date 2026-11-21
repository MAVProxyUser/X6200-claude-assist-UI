/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: V.21 channel 2 (v21.h). Ported from BinModem.
 */
#include "v21.h"

#include <math.h>
#include <string.h>

/* fsk.rs: an absolute level with hysteresis */
#define CARRIER_ON  1.0e-3
#define CARRIER_OFF 5.62e-4

/* v21.rs: how much of the error each transition takes out of the bit clock */
#define PULL 0.125

void v21_tx_init(v21_tx_t *t, double fs) {
    memset(t, 0, sizeof(*t));
    t->space     = V21_SPACE;
    t->mark      = V21_MARK;
    t->fs        = fs;
    t->countdown = fs / V21_BAUD;
    t->sps       = fs / V21_BAUD;
    t->current   = true;
}

void v21_tx_channel(v21_tx_t *t, int channel) {
    t->space = channel == 1 ? V21_CH1_SPACE : V21_SPACE;
    t->mark  = channel == 1 ? V21_CH1_MARK : V21_MARK;
}

void v21_tx_set(v21_tx_t *t, bool on) {
    t->transmitting = on;
}

/* Bell103Tx::next_sample: continuous phase across every shift, idle is mark */
double v21_tx_sample(v21_tx_t *t) {
    if (!t->transmitting)
        return 0.0;
    t->countdown -= 1.0;
    if (t->countdown <= 0.0) {
        bool b;
        t->countdown += t->sps;
        t->current = bitq_pop(&t->pending, &b) ? b : true;
    }
    double hz = t->current ? t->mark : t->space;
    t->phase += hz / t->fs;
    t->phase -= floor(t->phase);
    return cos(t->phase * TAU);
}

void fsk_init(fsk_t *f, double space, double mark, double baud, double fs) {
    double centre    = (space + mark) / 2.0;
    double deviation = (mark - space) / 2.0;
    double half      = fabs(deviation) + baud * 0.9;
    double lo        = centre - half;

    memset(f, 0, sizeof(*f));
    nco_init(&f->nco, centre, fs);
    bandpass(&f->band, 8, lo > 60.0 ? lo : 60.0, centre + half, fs);
    butter_lowpass(&f->lp_i, 4, baud * 1.3, fs);
    butter_lowpass(&f->lp_q, 4, baud * 1.3, fs);
    butter_lowpass(&f->post, 2, baud * 0.8, fs);
    f->fs        = fs;
    f->deviation = deviation;
    onepole_init(&f->fast_env, 0.005, fs);
    onepole_init(&f->slow_env, 0.250, fs);
}

double fsk_feed(fsk_t *f, double x) {
    double b = cascade_run(&f->band, x), c, s;

    nco_step(&f->nco, &c, &s);
    double i      = cascade_run(&f->lp_i, b * c);
    double q      = cascade_run(&f->lp_q, b * -s);
    double re     = i * f->prev_i + q * f->prev_q;
    double im     = q * f->prev_i - i * f->prev_q;
    f->prev_i     = i;
    f->prev_q     = q;
    double mag    = sqrt(i * i + q * q);
    double level  = onepole_run(&f->fast_env, mag);
    onepole_run(&f->slow_env, mag);
    f->carrier    = f->carrier ? level > CARRIER_OFF : level > CARRIER_ON;
    double hz     = atan2(im, re) * f->fs / TAU;
    return cascade_run(&f->post, hz) / f->deviation;
}

void v21_rx_init_channel(v21_rx_t *r, double fs, int channel) {
    memset(r, 0, sizeof(*r));
    fsk_init(&r->det, channel == 1 ? V21_CH1_SPACE : V21_SPACE, channel == 1 ? V21_CH1_MARK : V21_MARK, V21_BAUD, fs);
    r->sps = fs / V21_BAUD;
}

void v21_rx_init(v21_rx_t *r, double fs) {
    v21_rx_init_channel(r, fs, 2);
}

/* Receiver::feed: the clock starts half a bit after the first crossing and is nudged by every
 * transition after it */
int v21_rx_feed(v21_rx_t *r, double x) {
    double level = fsk_feed(&r->det, x);

    if (!r->det.carrier) {
        r->running = false;
        r->last    = level;
        return -1;
    }
    if (!r->running) {
        if ((level > 0.0) == (r->last > 0.0)) {
            r->last = level;
            return -1;
        }
        r->running   = true;
        r->countdown = r->sps / 2.0;
    }
    if ((level > 0.0) != (r->last > 0.0))
        r->countdown += PULL * (r->sps / 2.0 - r->countdown);
    r->last = level;
    r->countdown -= 1.0;
    if (r->countdown > 0.0)
        return -1;
    r->countdown += r->sps;
    r->level = level;
    return level > 0.0;
}
