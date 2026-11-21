/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the signal processing every pump shares.
 *
 *  A C port of BinModem's dsp crate (github.com/CasualArclamp/BinModem, GPL-3.0-or-later, by
 *  CasualArclamp): the Butterworth sections, the one-pole smoother, the oscillator and the
 *  root-raised-cosine pulse, with the same formulas and the same constants, so a transmitter built
 *  on them puts out the samples BinModem's does.
 */
#ifndef X6X00_MODEM_DSP_H
#define X6X00_MODEM_DSP_H

#include <complex.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU (2.0 * M_PI)

typedef double complex cplx;

/* ---- filter.rs ---- */

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
} biquad_t;

#define CASCADE_MAX 8
typedef struct {
    int      n;
    biquad_t s[CASCADE_MAX];
} cascade_t;

void   butter_lowpass(cascade_t *c, int order, double fc, double fs);
void   butter_highpass(cascade_t *c, int order, double fc, double fs);
void   bandpass(cascade_t *c, int order, double lo, double hi, double fs);     /* high-pass, then low-pass */
double cascade_run(cascade_t *c, double x);
void   cascade_reset(cascade_t *c);

typedef struct {
    double a, y;
} onepole_t;

void          onepole_init(onepole_t *p, double tau, double fs);
static inline double onepole_run(onepole_t *p, double x) {
    p->y = p->a * p->y + (1.0 - p->a) * x;
    return p->y;
}

/* ---- nco.rs ---- */

typedef struct {
    double phase, step, fs;
} nco_t;

void nco_init(nco_t *n, double hz, double fs);
/* advance one sample; cos and sin of the new phase */
void nco_step(nco_t *n, double *c, double *s);

/* ---- shaping.rs ---- */

double rrc_at(double t, double beta);

/* The pulse a transmitter sums each sample, taken from a table of rrc_at made once per roll-off:
 * RRC_STEPS points a symbol, linear between them (within 1e-7 of rrc_at, and a tenth of the cost,
 * which on the radio's processor is the difference between a few percent of a core and a third). */
#define RRC_STEPS 1024
typedef struct {
    double beta;
    int    span;                    /* symbols each side of the centre */
    double t[(2 * 8 + 2) * RRC_STEPS + 2];     /* from -span-1 to span+1 symbols */
} rrc_table_t;

void   rrc_table_init(rrc_table_t *r, double beta, int span);
double rrc_table_at(const rrc_table_t *r, double t);    /* t in symbols from the centre */

/* ---- complex.rs: least squares by the normal equations, solved by Cholesky ---- */

/* rows[r * n + i], targets[r]; out[n]. False when the system is singular */
bool least_squares(const cplx *rows, const cplx *targets, int nrows, int n, double ridge, cplx *out);

#endif
