/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: V.27 ter, 4800 bit/s (tribits on eight phases at 1600 baud) and 2400 bit/s (dibits
 *  on four at 1200), on an 1800 Hz carrier. The transmitter is BinModem's datapump/src/v27ter.rs,
 *  line for line: its turn-on sequence, scrambler, differential coding and pulse.
 */
#ifndef X6X00_MODEM_V27TER_H
#define X6X00_MODEM_V27TER_H

#include "dsp.h"
#include "v21.h"

#define V27_CARRIER        1800.0
#define V27_ROLLOFF        0.5
#define V27_SPAN           6
#define V27_SCRAMBLED_ONES 8
#define V27_TURN_OFF       10

extern const uint8_t v27_tribit_turn[8], v27_turn_tribit[8], v27_dibit_turn[4], v27_turn_dibit[4];

/* the 7-bit scrambler of 2.1, 1 + x^-6 + x^-7 */
typedef struct {
    uint8_t history;
} v27_scrambler_t;

void v27_scrambler_seed(v27_scrambler_t *s);
bool v27_scramble(v27_scrambler_t *s, bool bit);
bool v27_descramble(v27_scrambler_t *s, bool bit);

typedef enum { V27_SILENT, V27_REVERSALS, V27_CONDITIONING, V27_ONES, V27_DATA, V27_TURNOFF } v27_stage_t;

typedef struct {
    double          fs;
    bool            r4800, long_train;
    nco_t           nco;
    v27_scrambler_t scrambler;
    v27_stage_t     stage;
    uint32_t        left;
    uint8_t         eighths;
    cplx            history[2 * V27_SPAN + 1], sent;
    double          phase;
    bitq_t          pending;
    rrc_table_t    *rrc;
} v27_tx_t;

static inline double v27_baud(bool r4800) {
    return r4800 ? 1600.0 : 1200.0;
}
static inline uint32_t v27_reversals(bool long_train) {
    return long_train ? 50 : 14;
}
static inline uint32_t v27_conditioning(bool long_train) {
    return long_train ? 1074 : 58;
}

void   v27_tx_init(v27_tx_t *t, double fs);
void   v27_tx_start(v27_tx_t *t, bool r4800, bool long_train);
void   v27_tx_stop(v27_tx_t *t);       /* what is queued, then scrambled ones, then off */
cplx   v27_tx_symbol(v27_tx_t *t);     /* the next symbol (for the receiver's known sequence too) */
double v27_tx_sample(v27_tx_t *t);
static inline bool v27_tx_busy(const v27_tx_t *t) {
    return t->stage != V27_SILENT;
}

#endif
