/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the fast modems behind one face, as T.30 uses them: V.29 at 9600 and 7200, V.27 ter
 *  at 4800 and 2400, each burst a full turn-on (V.27 ter's long one) and then data.
 */
#ifndef X6X00_MODEM_PUMP_H
#define X6X00_MODEM_PUMP_H

#include "qamrx.h"
#include "v27ter.h"
#include "v29.h"

typedef enum { RATE_2400, RATE_4800, RATE_7200, RATE_9600, RATE_COUNT } rate_t;

int         rate_bps(rate_t r);
const char *rate_name(rate_t r);        /* "V.29 9600" */

typedef void (*pump_bit_fn)(void *ctx, int bit);

typedef struct {
    double   fs;
    rate_t   rate;
    v27_tx_t v27;
    v29_tx_t v29;
    bool     stopping;

    qamrx_t     rx;
    bool        rx_ready;
    cplx        known[1200];
    int         nknown, ref;            /* ref: the last symbol before the first coded one */
    v29_point_t v29pts[16];
    int         prev;                   /* the previous symbol's label, -1 for none */
    v27_scrambler_t d27;
    v32_scrambler_t d32;
    pump_bit_fn on_bit;
    void       *ctx;
    uint64_t    bits_out;
} pump_t;

void   pump_init(pump_t *p, double fs, pump_bit_fn fn, void *ctx);
void   pump_free(pump_t *p);

/* transmitting */
void   pump_tx_start(pump_t *p, rate_t r);
bool   pump_tx_push(pump_t *p, int bit);
uint32_t pump_tx_pending(pump_t *p);
void   pump_tx_stop(pump_t *p);
bool   pump_tx_busy(pump_t *p);
double pump_tx_sample(pump_t *p);
double pump_train_seconds(rate_t r);    /* the turn-on's length */

/* receiving: set the rate the next burst is at (the DCS named it), then feed the line */
void   pump_rx_arm(pump_t *p, rate_t r);
void   pump_rx_feed(pump_t *p, double x);
static inline qam_state_t pump_rx_state(const pump_t *p) {
    return p->rx_ready ? p->rx.state : QAM_IDLE;
}

#endif
