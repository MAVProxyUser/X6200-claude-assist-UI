/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the shared signal processing (dsp.h). Ported from BinModem's dsp crate.
 */
#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void biquad_set(biquad_t *q, double b0, double b1, double b2, double a0, double a1, double a2) {
    q->b0 = b0 / a0;
    q->b1 = b1 / a0;
    q->b2 = b2 / a0;
    q->a1 = a1 / a0;
    q->a2 = a2 / a0;
    q->z1 = q->z2 = 0.0;
}

static double clamp(double x, double lo, double hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

/* Even orders only, as BinModem's butterworth_zetas */
static void butter(cascade_t *c, int order, double fc, double fs, bool high) {
    double w0 = 2.0 * M_PI * clamp(fc / fs, 1e-6, 0.4999);
    double cw = cos(w0), sw = sin(w0);

    c->n = 0;
    for (int k = 0; k < order / 2 && c->n < CASCADE_MAX; k++) {
        double zeta  = sin(M_PI * (2 * k + 1) / (2.0 * order));
        double alpha = sw * zeta;
        double b1    = high ? 1.0 + cw : 1.0 - cw;

        if (high)
            biquad_set(&c->s[c->n++], b1 / 2.0, -b1, b1 / 2.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
        else
            biquad_set(&c->s[c->n++], b1 / 2.0, b1, b1 / 2.0, 1.0 + alpha, -2.0 * cw, 1.0 - alpha);
    }
}

void butter_lowpass(cascade_t *c, int order, double fc, double fs) {
    butter(c, order, fc, fs, false);
}

void butter_highpass(cascade_t *c, int order, double fc, double fs) {
    butter(c, order, fc, fs, true);
}

void bandpass(cascade_t *c, int order, double lo, double hi, double fs) {
    cascade_t lp;

    butter(c, order, lo, fs, true);
    butter(&lp, order, hi, fs, false);
    for (int i = 0; i < lp.n && c->n < CASCADE_MAX; i++)
        c->s[c->n++] = lp.s[i];
}

double cascade_run(cascade_t *c, double x) {
    for (int i = 0; i < c->n; i++) {
        biquad_t *q = &c->s[i];
        double    y = q->b0 * x + q->z1;

        q->z1 = q->b1 * x - q->a1 * y + q->z2;
        q->z2 = q->b2 * x - q->a2 * y;
        x     = y;
    }
    return x;
}

void cascade_reset(cascade_t *c) {
    for (int i = 0; i < c->n; i++)
        c->s[i].z1 = c->s[i].z2 = 0.0;
}

void onepole_init(onepole_t *p, double tau, double fs) {
    p->a = exp(-1.0 / (tau * fs));
    p->y = 0.0;
}

void nco_init(nco_t *n, double hz, double fs) {
    n->phase = 0.0;
    n->step  = hz / fs;
    n->fs    = fs;
}

void nco_step(nco_t *n, double *c, double *s) {
    n->phase += n->step;
    n->phase -= floor(n->phase);
    double r = n->phase * TAU;
    *c       = cos(r);
    *s       = sin(r);
}

double rrc_at(double t, double beta) {
    if (fabs(t) < 1e-9)
        return 1.0 + beta * (4.0 / M_PI - 1.0);
    if (beta > 0.0) {
        double edge = 1.0 / (4.0 * beta);
        if (fabs(fabs(t) - edge) < 1e-9) {
            double a = (1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * beta));
            double b = (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * beta));
            return beta / sqrt(2.0) * (a + b);
        }
    }
    double num = sin(M_PI * t * (1.0 - beta)) + 4.0 * beta * t * cos(M_PI * t * (1.0 + beta));
    double den = M_PI * t * (1.0 - (4.0 * beta * t) * (4.0 * beta * t));
    return num / den;
}

void rrc_table_init(rrc_table_t *r, double beta, int span) {
    int n = (2 * span + 2) * RRC_STEPS + 2;

    r->beta = beta;
    r->span = span;
    for (int i = 0; i < n; i++)
        r->t[i] = rrc_at((double)i / RRC_STEPS - span - 1, beta);
}

double rrc_table_at(const rrc_table_t *r, double t) {
    double x = (t + r->span + 1) * RRC_STEPS;
    if (x < 0.0 || x >= (2 * r->span + 2) * RRC_STEPS)
        return rrc_at(t, r->beta);
    int    i = (int)x;
    double f = x - i;
    return r->t[i] + (r->t[i + 1] - r->t[i]) * f;
}

/* A = L L*, as complex.rs solve_hermitian */
static bool solve_hermitian(const cplx *a, const cplx *b, int n, cplx *out) {
    cplx *l = calloc((size_t)n * n, sizeof(cplx));
    cplx *y = calloc((size_t)n, sizeof(cplx));
    bool  ok = l && y;

    for (int j = 0; ok && j < n; j++) {
        double diagonal = creal(a[j * n + j]);
        for (int k = 0; k < j; k++)
            diagonal -= creal(l[j * n + k] * conj(l[j * n + k]));
        if (diagonal <= 1e-12 * fabs(creal(a[j * n + j])) || diagonal <= 0.0 || !isfinite(diagonal)) {
            ok = false;
            break;
        }
        double root  = sqrt(diagonal);
        l[j * n + j] = root;
        for (int i = j + 1; i < n; i++) {
            cplx sum = a[i * n + j];
            for (int k = 0; k < j; k++)
                sum -= l[i * n + k] * conj(l[j * n + k]);
            l[i * n + j] = sum / root;
        }
    }
    if (ok) {
        /* L y = b, then L* x = y */
        for (int i = 0; i < n; i++) {
            cplx s = b[i];
            for (int k = 0; k < i; k++)
                s -= l[i * n + k] * y[k];
            y[i] = s / l[i * n + i];
        }
        for (int i = n - 1; i >= 0; i--) {
            cplx s = y[i];
            for (int k = i + 1; k < n; k++)
                s -= conj(l[k * n + i]) * out[k];
            out[i] = s / conj(l[i * n + i]);
        }
    }
    free(l);
    free(y);
    return ok;
}

bool least_squares(const cplx *rows, const cplx *targets, int nrows, int n, double ridge, cplx *out) {
    cplx *a = calloc((size_t)n * n, sizeof(cplx));
    cplx *b = calloc((size_t)n, sizeof(cplx));
    bool  ok;

    if (!a || !b) {
        free(a);
        free(b);
        return false;
    }
    for (int r = 0; r < nrows; r++) {
        const cplx *row = rows + (size_t)r * n;
        for (int j = 0; j < n; j++) {
            cplx xj = conj(row[j]);
            b[j] += xj * targets[r];
            for (int i = j; i < n; i++)
                a[j * n + i] += xj * row[i];
        }
    }
    for (int j = 0; j < n; j++) {
        a[j * n + j] += ridge;
        for (int i = 0; i < j; i++)
            a[j * n + i] = conj(a[i * n + j]);
    }
    ok = solve_hermitian(a, b, n, out);
    free(a);
    free(b);
    return ok;
}
