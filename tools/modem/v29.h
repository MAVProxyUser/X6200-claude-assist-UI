/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: V.29, 9600 bit/s (sixteen points), 7200 (the inner eight) and 4800 (four), at 2400 baud
 *  on a 1700 Hz carrier. The transmitter is BinModem's datapump/src/v29.rs line for line: the
 *  synchronizing signal of 8.3, the conditioning generator, the V.32 caller's scrambler and Table 2.
 */
#ifndef X6X00_MODEM_V29_H
#define X6X00_MODEM_V29_H

#include "dsp.h"
#include "v21.h"

#define V29_CARRIER 1700.0
#define V29_BAUD    2400.0
#define V29_ROLLOFF 0.25
#define V29_SPAN    6

/* the synchronizing signal, in symbol intervals */
#define V29_SILENCE      48
#define V29_ALTERNATIONS 128
#define V29_CONDITIONING 384
#define V29_ONES         48
#define V29_TURN_OFF     (2 * V29_SPAN + 12)

typedef enum { V29_9600, V29_7200, V29_4800 } v29_rate_t;

typedef struct {
    uint8_t eighths;
    bool    outer;
} v29_point_t;

cplx   v29_xy(v29_point_t p);                  /* in the units of Figure 1 */
double v29_rms(v29_rate_t r);
int    v29_bits(v29_rate_t r);
/* every point the rate sends, unit power; their (eighths, outer) in pts. Returns the count */
int    v29_constellation(v29_rate_t r, cplx *unit, v29_point_t *pts);

/* the V.32 caller's scrambler, 1 + x^-18 + x^-23 */
typedef struct {
    uint32_t reg;
} v32_scrambler_t;

bool v32_scramble(v32_scrambler_t *s, bool bit);
bool v32_descramble(v32_scrambler_t *s, bool bit);

typedef enum { V29_SILENT, V29_QUIET, V29_ALT, V29_COND, V29_ONESEG, V29_DATA, V29_TURNOFF } v29_stage_t;

typedef struct {
    double          fs;
    v29_rate_t      rate;
    nco_t           nco;
    v32_scrambler_t scrambler;
    uint8_t         cond;           /* the conditioning generator, stage 1 in bit 6 */
    v29_stage_t     stage;
    uint32_t        left;
    v29_point_t     point;
    cplx            history[2 * V29_SPAN + 1], sent;
    double          phase;
    bitq_t          pending;
    rrc_table_t    *rrc;
} v29_tx_t;

void   v29_tx_init(v29_tx_t *t, double fs);
void   v29_tx_start(v29_tx_t *t, v29_rate_t rate);
void   v29_tx_stop(v29_tx_t *t);
/* the next symbol; false for one of no energy (segment 1) */
bool   v29_tx_symbol(v29_tx_t *t, v29_point_t *p);
double v29_tx_sample(v29_tx_t *t);
static inline bool v29_tx_busy(const v29_tx_t *t) {
    return t->stage != V29_SILENT;
}

#endif
