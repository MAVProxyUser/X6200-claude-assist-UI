/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df's FFT and calibration (df.h; docs/plans/kraken_df_plan.md 1.5).
 *
 *  A calibration is one block of the KrakenSDR's noise source, the same noise in every channel. Against channel 0:
 *    1. the whole-sample delay: the peak of the cross-correlation (one FFT each, the first DF_CAL_XN samples), at
 *       least 20 dB over the rest; skipped when the last good calibration's delays are given;
 *    2. the cross-spectrum S(f) = X_m(f) X_0*(f), averaged over Hann-windowed 1024-point pieces of the aligned block;
 *    3. arg S(f) = phi - 2 pi f tau: a coarse tau from the turn between neighbouring bins (no unwrapping), phi from
 *       S turned back by it, then a weighted straight line through what is left; the residual says if it fits. A tau
 *       past half a sample moves the whole-sample delay;
 *    4. the gain from the channels' powers.
 */

#include "df.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------------------------------- FFT */

int df_fft_init(df_fft_t *f, int n) {
    int bits = 0;

    memset(f, 0, sizeof(*f));
    if (n < 2 || (n & (n - 1))) return -1;
    while ((1 << bits) < n) bits++;
    f->n = n;
    f->tw = malloc(sizeof(*f->tw) * (size_t)(n / 2));
    f->rev = malloc(sizeof(*f->rev) * (size_t)n);
    if (!f->tw || !f->rev) {
        df_fft_free(f);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        int r = 0;

        for (int b = 0; b < bits; b++)
            if (i >> b & 1) r |= 1 << (bits - 1 - b);
        f->rev[i] = r;
    }
    for (int k = 0; k < n / 2; k++) {
        double a = -2.0 * M_PI * k / n;

        f->tw[k] = CMPLXF((float)cos(a), (float)sin(a));
    }
    return 0;
}

void df_fft_free(df_fft_t *f) {
    free(f->tw);
    free(f->rev);
    memset(f, 0, sizeof(*f));
}

void df_fft(const df_fft_t *f, float complex *x, bool inverse) {
    int    n = f->n;
    float *v = (float *)x;                     /* re, im pairs */

    for (int i = 0; i < n; i++) {
        int r = f->rev[i];

        if (r > i) {
            float complex t = x[i];

            x[i] = x[r];
            x[r] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, step = n / len;

        for (int j = 0; j < half; j++) {
            float wr = crealf(f->tw[j * step]), wi = inverse ? -cimagf(f->tw[j * step]) : cimagf(f->tw[j * step]);

            for (int i = j; i < n; i += len) {
                float *u = v + 2 * i, *w = v + 2 * (i + half);
                float  vr = w[0] * wr - w[1] * wi, vi = w[0] * wi + w[1] * wr;

                w[0] = u[0] - vr;
                w[1] = u[1] - vi;
                u[0] += vr;
                u[1] += vi;
            }
        }
    }
}

void df_u8_to_cf(const uint8_t *iq, int n, float complex *out) {
    long si = 0, sq = 0;

    for (int i = 0; i < n; i++) {
        si += iq[2 * i];
        sq += iq[2 * i + 1];
    }
    float mi = (float)si / (float)n, mq = (float)sq / (float)n;

    for (int i = 0; i < n; i++)
        out[i] = CMPLXF((iq[2 * i] - mi) / 127.5f, (iq[2 * i + 1] - mq) / 127.5f);
}

/* ------------------------------------------------------------------------------------------------- calibration */

static double wrap(double a) {
    while (a > M_PI) a -= 2 * M_PI;
    while (a <= -M_PI) a += 2 * M_PI;
    return a;
}

/* the bins used for the fit: signed frequency index 3 <= |b| <= 0.4 N (the DC spike and the band edges left out) */
static bool fit_bin(int b, int n) {
    int a = b < 0 ? -b : b;

    return a >= 3 && a <= (int)(0.4 * n);
}

static double pw(float complex z) {
    return (double)crealf(z) * crealf(z) + (double)cimagf(z) * cimagf(z);
}

/* 1. whole-sample delays from the first xn samples; 0, or -1 if a peak is too low */
static int delays(float complex *const x[], int nch, int xn, int maxlag, df_cal_t *o) {
    df_fft_t       F;
    float complex *X0 = malloc(sizeof(*X0) * (size_t)xn), *Xm = malloc(sizeof(*Xm) * (size_t)xn);
    int            rc = 0;

    if (maxlag > xn / 4) maxlag = xn / 4;
    if (!X0 || !Xm || df_fft_init(&F, xn)) {
        free(X0);
        free(Xm);
        snprintf(o->why, sizeof(o->why), "out of memory");
        return -1;
    }
    memcpy(X0, x[0], sizeof(*X0) * (size_t)xn);
    df_fft(&F, X0, false);
    for (int m = 1; m < nch && !rc; m++) {
        double best = -1, sum = 0;
        int    at = 0, cnt = 0;

        memcpy(Xm, x[m], sizeof(*Xm) * (size_t)xn);
        df_fft(&F, Xm, false);
        for (int i = 0; i < xn; i++) {
            float complex a = Xm[i], b = X0[i];

            Xm[i] = CMPLXF(crealf(a) * crealf(b) + cimagf(a) * cimagf(b), cimagf(a) * crealf(b) - crealf(a) * cimagf(b));
        }
        df_fft(&F, Xm, true);                           /* r[l] = sum x_m[i + l] x_0*[i] */
        for (int l = -maxlag; l <= maxlag; l++) {
            double v = pw(Xm[(l + xn) % xn]);

            if (v > best) best = v, at = l;
        }
        for (int i = 0; i < xn; i++) {
            int l = i < xn / 2 ? i : i - xn;

            if (abs(l - at) > 3) sum += pw(Xm[i]), cnt++;
        }
        o->k[m] = at;
        o->margin_db[m] = (float)(10 * log10(best / (sum / cnt + 1e-30)));
        if (o->margin_db[m] < DF_CAL_MARGIN) {
            snprintf(o->why, sizeof(o->why), "channel %d: correlation peak only %.1f dB", m, o->margin_db[m]);
            rc = -1;
        }
    }
    free(X0);
    free(Xm);
    df_fft_free(&F);
    return rc;
}

int df_cal_run(float complex *const x[], int nch, int n, int maxlag, const int *known_k, df_cal_t *o) {
    df_fft_t       S;
    float complex *seg = NULL, *seg0 = NULL;
    double        *cs = NULL;                       /* re, im per bin per channel */
    float         *win = NULL;
    double         p[DF_MAXCH];
    int            rc = -1;

    memset(o, 0, sizeof(*o));
    o->nch = nch;
    o->gain[0] = 1;
    o->margin_db[0] = 99;
    if (nch < 2 || nch > DF_MAXCH) {
        snprintf(o->why, sizeof(o->why), "channels: 2 to 8");
        return -1;
    }
    if (df_fft_init(&S, DF_CAL_SEG)) return -1;

    /* levels */
    for (int m = 0; m < nch; m++) {
        double s = 0;

        for (int i = 0; i < n; i++) s += pw(x[m][i]);
        p[m] = s / n;
        o->level_db[m] = (float)(10 * log10(p[m] + 1e-30));
        if (p[m] <= 1e-12) {
            snprintf(o->why, sizeof(o->why), "channel %d: no signal", m);
            goto out;
        }
    }

    /* 1. */
    if (known_k) {
        for (int m = 1; m < nch; m++) o->k[m] = known_k[m], o->margin_db[m] = 0;
    } else if (delays(x, nch, n < DF_CAL_XN ? n : DF_CAL_XN, maxlag, o) && (n <= DF_CAL_XN || delays(x, nch, n, maxlag, o)))
        goto out;

    /* 2. the cross-spectra on the aligned block: each channel read k[m] - min k later */
    int kmin = 0, kmax = 0;

    for (int m = 1; m < nch; m++) {
        if (o->k[m] < kmin) kmin = o->k[m];
        if (o->k[m] > kmax) kmax = o->k[m];
    }
    int nseg = (n - (kmax - kmin)) / DF_CAL_SEG;

    if (nseg > DF_CAL_NSEG) nseg = DF_CAL_NSEG;
    if (nseg < 8) {
        snprintf(o->why, sizeof(o->why), "delays too long for the block");
        goto out;
    }
    seg = malloc(sizeof(*seg) * DF_CAL_SEG);
    seg0 = malloc(sizeof(*seg0) * DF_CAL_SEG);
    cs = calloc((size_t)(2 * DF_CAL_SEG * nch), sizeof(double));
    win = malloc(sizeof(*win) * DF_CAL_SEG);
    if (!seg || !seg0 || !cs || !win) {
        snprintf(o->why, sizeof(o->why), "out of memory");
        goto out;
    }
    for (int i = 0; i < DF_CAL_SEG; i++) win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / DF_CAL_SEG));
    for (int s = 0; s < nseg; s++) {
        for (int i = 0; i < DF_CAL_SEG; i++) seg0[i] = x[0][-kmin + s * DF_CAL_SEG + i] * win[i];
        df_fft(&S, seg0, false);
        for (int m = 1; m < nch; m++) {
            double *c = cs + 2 * DF_CAL_SEG * m;

            for (int i = 0; i < DF_CAL_SEG; i++) seg[i] = x[m][o->k[m] - kmin + s * DF_CAL_SEG + i] * win[i];
            df_fft(&S, seg, false);
            for (int i = 0; i < DF_CAL_SEG; i++) {             /* seg seg0* */
                float ar = crealf(seg[i]), ai = cimagf(seg[i]), br = crealf(seg0[i]), bi = cimagf(seg0[i]);

                c[2 * i] += ar * br + ai * bi;
                c[2 * i + 1] += ai * br - ar * bi;
            }
        }
    }

    /* 3, 4 */
    for (int m = 1; m < nch; m++) {
        const double *c = cs + 2 * DF_CAL_SEG * m;
#define CS(b) CMPLX(c[2 * (((b) + DF_CAL_SEG) % DF_CAL_SEG)], c[2 * (((b) + DF_CAL_SEG) % DF_CAL_SEG) + 1])
        double complex z = 0, zp = 0;

        for (int b = -DF_CAL_SEG / 2; b < DF_CAL_SEG / 2 - 1; b++)      /* S(b+1) S*(b) turns by -2 pi tau / N */
            if (fit_bin(b, DF_CAL_SEG) && fit_bin(b + 1, DF_CAL_SEG)) {
                double complex u = CS(b + 1), v = CS(b);

                z += CMPLX(creal(u) * creal(v) + cimag(u) * cimag(v), cimag(u) * creal(v) - creal(u) * cimag(v));
            }
        double tau = -carg(z) * DF_CAL_SEG / (2 * M_PI);

        for (int b = -DF_CAL_SEG / 2; b < DF_CAL_SEG / 2; b++)
            if (fit_bin(b, DF_CAL_SEG)) {
                double complex v = CS(b);
                double         a = 2 * M_PI * b * tau / DF_CAL_SEG, ca = cos(a), sa = sin(a);

                zp += CMPLX(creal(v) * ca - cimag(v) * sa, creal(v) * sa + cimag(v) * ca);
            }
        double phi = carg(zp);

        /* fine: residual phase r = a + c u (u = b / N), weighted by |S| */
        double sw = 0, su = 0, sr = 0, suu = 0, sur = 0, se = 0;

        for (int b = -DF_CAL_SEG / 2; b < DF_CAL_SEG / 2; b++) {
            if (!fit_bin(b, DF_CAL_SEG)) continue;
            double complex v = CS(b);
            double         u = (double)b / DF_CAL_SEG, w = cabs(v), r = wrap(carg(v) + 2 * M_PI * u * tau - phi);

            sw += w, su += w * u, sr += w * r, suu += w * u * u, sur += w * u * r;
        }
        double den = sw * suu - su * su, cc = den != 0 ? (sw * sur - su * sr) / den : 0, a = (sr - cc * su) / sw;

        tau -= cc / (2 * M_PI);
        phi = wrap(phi + a);
        for (int b = -DF_CAL_SEG / 2; b < DF_CAL_SEG / 2; b++) {
            if (!fit_bin(b, DF_CAL_SEG)) continue;
            double complex v = CS(b);
            double         u = (double)b / DF_CAL_SEG, w = cabs(v), r = wrap(carg(v) + 2 * M_PI * u * tau - phi);

            se += w * r * r;
        }
#undef CS
        int kk = (int)lround(tau);                  /* keep tau within +-0.5: the whole sample may sit either side */

        o->k[m] += kk;
        o->tau[m] = (float)(tau - kk);
        o->phi[m] = (float)phi;
        o->gain[m] = (float)sqrt(p[0] / p[m]);
        o->resid_deg[m] = (float)(sqrt(se / sw) * 180 / M_PI);
        if (o->resid_deg[m] > DF_CAL_RESID) {
            snprintf(o->why, sizeof(o->why), "channel %d: phase fit off by %.1f deg rms", m, o->resid_deg[m]);
            goto out;
        }
    }
    o->ok = true;
    rc = 0;

out:
    free(seg);
    free(seg0);
    free(cs);
    free(win);
    df_fft_free(&S);
    return rc;
}

float complex df_cal_corr(const df_cal_t *c, int m, double f, double fs) {
    if (m <= 0 || m >= c->nch) return 1;
    double a = -(c->phi[m] - 2 * M_PI * (f / fs) * c->tau[m]);

    return CMPLXF((float)(c->gain[m] * cos(a)), (float)(c->gain[m] * sin(a)));
}
