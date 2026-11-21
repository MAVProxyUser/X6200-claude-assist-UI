/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: V.21 channel 2, the 300 bit/s FSK a fax call talks over before and between pages
 *  (1650 Hz a mark, 1850 Hz a space). Ported from BinModem's datapump/src/v21.rs, its 300 bit/s
 *  transmitter (bell103.rs Bell103Tx) and its FSK detector (dsp/src/fsk.rs).
 */
#ifndef X6X00_MODEM_V21_H
#define X6X00_MODEM_V21_H

#include "dsp.h"

#define V21_SPACE 1850.0                /* channel 2, which T.30 uses */
#define V21_MARK  1650.0
#define V21_CH1_SPACE 1180.0            /* channel 1 (V.21 clause 3, note 2: F_A 1180 Hz a ZERO, F_Z 980 Hz) */
#define V21_CH1_MARK  980.0
#define V21_BAUD  300.0

/* bits queued for the transmitter: a ring, a bit a byte */
#define BITQ_SIZE 65536
typedef struct {
    uint8_t  b[BITQ_SIZE];
    uint32_t head, tail;
} bitq_t;

static inline void bitq_clear(bitq_t *q) {
    q->head = q->tail = 0;
}
static inline uint32_t bitq_len(const bitq_t *q) {
    return q->head - q->tail;
}
static inline bool bitq_push(bitq_t *q, bool bit) {
    if (bitq_len(q) >= BITQ_SIZE)
        return false;
    q->b[q->head++ % BITQ_SIZE] = bit;
    return true;
}
static inline bool bitq_pop(bitq_t *q, bool *bit) {
    if (q->head == q->tail)
        return false;
    *bit = q->b[q->tail++ % BITQ_SIZE];
    return true;
}

typedef struct {
    double space, mark, fs;
    double phase, countdown, sps;
    bool   current, transmitting;
    bitq_t pending;
} v21_tx_t;

void   v21_tx_init(v21_tx_t *t, double fs);
void   v21_tx_channel(v21_tx_t *t, int channel);   /* 1 or 2 (the default) */
void   v21_tx_set(v21_tx_t *t, bool on);
double v21_tx_sample(v21_tx_t *t);

typedef struct {
    nco_t     nco;
    cascade_t band, lp_i, lp_q, post;
    double    prev_i, prev_q, fs, deviation;
    onepole_t fast_env, slow_env;
    bool      carrier;
} fsk_t;

void   fsk_init(fsk_t *f, double space, double mark, double baud, double fs);
double fsk_feed(fsk_t *f, double x);       /* +1 a mark, -1 a space */

typedef struct {
    fsk_t  det;
    double sps, countdown, last, level;
    bool   running;
} v21_rx_t;

void v21_rx_init(v21_rx_t *r, double fs);
void v21_rx_init_channel(v21_rx_t *r, double fs, int channel);
/* one sample in; 0 or 1 when a bit is due, -1 otherwise */
int  v21_rx_feed(v21_rx_t *r, double x);
static inline bool v21_rx_carrier(const v21_rx_t *r) {
    return r->det.carrier;
}

#endif
