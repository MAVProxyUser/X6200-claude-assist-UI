/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df's engine (df.h; docs/plans/kraken_df_plan.md 1.6, 1.7, 2.1): one block of aligned bytes
 *  per channel in, one estimate out.
 *    - the target's level: CH0's spectrum (Hann, 1024 points, up to 8 averaged), the strongest bin within the target's
 *      band over the median bin (the floor);
 *    - every channel through the channel filter, times its calibration at the target's frequency; R from that;
 *    - DoA; the gate: calibrated, the squelch open three blocks running, one source across the array, the fit;
 *    - a slip: with 4 elements or more, one channel the rest fit well without (0.9, and 0.1 over all of them); or the
 *      target 15 dB up and the fit under 0.5. The gate shuts at once, a calibration after 0.3 s (once in 10 s at most).
 *      (One channel turned 180 degrees can still look like a plane wave from the other side: the fit alone misses it);
 *    - the timer: once the squelch has been shut 0.3 s, or 30 s past due.
 */

#include "df.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NFFT 1024

static void remake(df_engine_t *e) {
    for (int m = 0; m < e->nch; m++) e->corr[m] = e->have_cal ? df_cal_corr(&e->cal, m, e->f, e->fs) : 1;
    df_steer_make(&e->steer, &e->arr, DF_C / (e->fc + e->f));
}

int df_engine_init(df_engine_t *e, int cap) {
    if (e->nch < 2 || e->nch > DF_MAXCH || e->arr.n != e->nch) return -1;
    if (!e->sql_db) e->sql_db = 8;
    if (!e->eig_min_db) e->eig_min_db = 6;
    if (!e->fit_min) e->fit_min = 0.5;
    if (!e->cal_every_s) e->cal_every_s = 120;
    if (!e->nsrc) e->nsrc = 1;
    if (!e->bw) e->bw = 12500;
    e->cap = cap;
    if (df_chan_init(&e->chan, e->nch, e->fs, e->f, e->bw, cap)) return -1;
    if (df_fft_init(&e->fft, NFFT)) return -1;
    e->fbuf = malloc(sizeof(float complex) * NFFT);
    for (int m = 0; m < e->nch; m++) e->dec[m] = malloc(sizeof(float complex) * (size_t)(cap / (e->chan.d1 * e->chan.d2) + 4));
    e->last_slip_cal = -1e9;
    remake(e);
    return e->fbuf ? 0 : -1;
}

void df_engine_free(df_engine_t *e) {
    df_chan_free(&e->chan);
    df_fft_free(&e->fft);
    free(e->fbuf);
    for (int m = 0; m < DF_MAXCH; m++) free(e->dec[m]);
}

void df_engine_set_cal(df_engine_t *e, const df_cal_t *c) {
    e->cal = *c;
    e->have_cal = c->ok;
    e->since_cal = 0;
    e->low_fit = 0;
    e->nhist = 0;
    remake(e);
}

void df_engine_retarget(df_engine_t *e, double f) {
    e->f = f;
    df_chan_retune(&e->chan, f);
    remake(e);
}

static int fcmp(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return x < y ? -1 : x > y;
}

/* the strongest 1 kHz-ish bin of the target over the median bin, from CH0 */
static double level(df_engine_t *e, const uint8_t *iq, int n) {
    static float pw[NFFT], tmp[NFFT];
    int          nf = n / NFFT;

    if (nf > 8) nf = 8;
    if (nf < 1) return 0;
    memset(pw, 0, sizeof(pw));
    for (int f = 0; f < nf; f++) {
        const uint8_t *p = iq + 2 * (size_t)(f * (n / nf));

        df_u8_to_cf(p, NFFT, e->fbuf);
        for (int i = 0; i < NFFT; i++) e->fbuf[i] *= (float)(0.5 - 0.5 * cos(2 * M_PI * i / NFFT));
        df_fft(&e->fft, e->fbuf, false);
        for (int i = 0; i < NFFT; i++) pw[i] += crealf(e->fbuf[i]) * crealf(e->fbuf[i]) + cimagf(e->fbuf[i]) * cimagf(e->fbuf[i]);
    }
    memcpy(tmp, pw, sizeof(tmp));
    qsort(tmp, NFFT, sizeof(float), fcmp);
    double floor_ = tmp[NFFT / 2] > 0 ? tmp[NFFT / 2] : 1e-30, best = 0;

    for (int i = 0; i < NFFT; i++) {
        double fb = (i < NFFT / 2 ? i : i - NFFT) * e->fs / NFFT;

        if (fabs(fb - e->f) <= e->bw / 2 && pw[i] > best) best = pw[i];
    }
    return 10 * log10(best / floor_ + 1e-30);
}

int df_engine_block(df_engine_t *e, const uint8_t *const iq[], int n, df_est_t *o) {
    int    nch = e->nch;
    double dt;

    memset(o, 0, sizeof(*o));
    if (n > e->cap) n = e->cap;
    dt = n / e->fs;
    e->now += dt;
    e->since_cal += dt;
    o->level_db = level(e, iq[0], n);

    int k = df_chan_run(&e->chan, iq, n, e->dec);

    for (int m = 1; m < nch; m++)
        for (int i = 0; i < k; i++) e->dec[m][i] *= e->corr[m];

    /* R = E[x x^H] */
    double complex R[DF_MAXCH * DF_MAXCH];

    for (int i = 0; i < nch; i++)
        for (int j = i; j < nch; j++) {
            double sr = 0, si = 0;
            const float complex *a = e->dec[i], *b = e->dec[j];

            for (int t = 0; t < k; t++) {
                float ar = crealf(a[t]), ai = cimagf(a[t]), br = crealf(b[t]), bi = cimagf(b[t]);

                sr += ar * br + ai * bi;                /* a b* */
                si += ai * br - ar * bi;
            }
            R[i * nch + j] = (sr + I * si) / (k > 0 ? k : 1);
            R[j * nch + i] = conj(R[i * nch + j]);
        }
    o->k = k;
    bool doa_ok = k > nch && df_doa(R, &e->arr, &e->steer, e->method, e->nsrc, e->front_only, &o->doa) == 0;

    /* the gate */
    e->open_run = o->level_db >= e->sql_db ? e->open_run + 1 : 0;
    o->gate = e->have_cal && doa_ok && e->open_run >= 3 && o->doa.eig_db >= e->eig_min_db && o->doa.fit >= e->fit_min;

    /* uncertainty: the statistics of the peak, or the spread of the last bearings if wider; a degree at least */
    if (o->gate) {
        double snr = pow(10, o->doa.snr_db / 10), stat = o->doa.width_deg / sqrt(2 * k * (snr > 1e-6 ? snr : 1e-6));
        double sc = 0, ss = 0;

        e->hist[e->nhist++ % 10] = o->doa.bearing;
        int nh = e->nhist < 10 ? e->nhist : 10;

        for (int i = 0; i < nh; i++) sc += cos(e->hist[i] * M_PI / 180), ss += sin(e->hist[i] * M_PI / 180);
        double rbar = hypot(sc, ss) / nh, spread = nh > 1 && rbar < 1 ? sqrt(-2 * log(rbar)) * 180 / M_PI : 0;

        o->uncert_deg = fmax(1, fmax(stat, spread));
    }

    /* a slip: one channel that the rest fit well without (4 elements or more), or strong but no plane wave fits.
     * The gate shuts at once; a calibration after 0.3 s of it */
    bool suspect = false;

    o->bad_ch = -1;
    if (e->have_cal && doa_ok && nch >= 4 && o->level_db >= 12) {
        double fw[DF_MAXCH];
        int    b = 0;

        df_fit_without(R, &e->arr, &e->steer, fw);
        for (int m = 1; m < nch; m++)
            if (fw[m] > fw[b]) b = m;
        if (fw[b] >= 0.9 && fw[b] - o->doa.fit >= 0.1) suspect = true, o->bad_ch = b;
    }
    if (e->have_cal && doa_ok && o->level_db >= 15 && o->doa.fit < 0.5) suspect = true;
    if (suspect) {
        o->gate = false;
        e->low_fit += dt;
    } else
        e->low_fit = 0;
    if (e->low_fit >= 0.3 && e->now - e->last_slip_cal >= 10) {
        o->cal_wanted = 1;
        e->last_slip_cal = e->now;
        e->low_fit = 0;
    }

    /* the timer, in a gap */
    e->sql_closed = o->level_db < e->sql_db ? e->sql_closed + dt : 0;
    if (!o->cal_wanted && e->since_cal >= e->cal_every_s && (e->sql_closed >= 0.3 || e->since_cal >= e->cal_every_s + 30))
        o->cal_wanted = 2;
    return 1;
}
