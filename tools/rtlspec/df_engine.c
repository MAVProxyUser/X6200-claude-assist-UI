/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df's engine (df.h; docs/plans/kraken_df_plan.md 1.6, 1.7, 2.1): one block of aligned bytes
 *  per channel in, one estimate out.
 *    - the target's level on every channel: its spectrum (Hann, 1024 points, 8 averaged on CH0, 4 on the rest),
 *      the strongest bin within the target's band over the median bin (that channel's floor); the squelch and the
 *      checks go by the median of the channels in use, so one dead or hot channel does not move it;
 *    - a channel the target is DF_LOW_DB (20) or more under the rest on for a second (a dead antenna, a loose cable)
 *      is left out of the bearing, and back once within DF_LOW_BACK_DB (12); the user can leave any out (user_off). The bearing comes from
 *      the elements left (3 or more on a circle, 2 on a line);
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
    e->sub_mask = 0;                    /* made again at the next block */
    e->sub.n = 0;
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
    for (int i = 0; i < NFFT; i++) e->win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / NFFT));
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

/* The target's strongest bin over the median bin (the floor), from one channel's spectrum: nf_max 1024-point pieces
 * averaged. keep: CH0's, kept for the picture (spec_db, floor_db) */
static double level(df_engine_t *e, const uint8_t *iq, int n, int nf_max, bool keep) {
    float pw[NFFT], tmp[NFFT];
    int   nf = n / NFFT;

    if (nf > nf_max) nf = nf_max;
    if (nf < 1) return 0;
    memset(pw, 0, sizeof(pw));
    for (int f = 0; f < nf; f++) {
        const uint8_t *p = iq + 2 * (size_t)(f * (n / nf));

        df_u8_to_cf(p, NFFT, e->fbuf);
        for (int i = 0; i < NFFT; i++) e->fbuf[i] *= e->win[i];
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
    if (keep) {                         /* dBFS: a full-scale sine through the Hann window is (NFFT / 2)^2 */
        double fs_ref = (double)nf * (NFFT / 2.0) * (NFFT / 2.0);

        for (int i = 0; i < NFFT; i++) e->spec_db[(i + NFFT / 2) % NFFT] = (float)(10 * log10(pw[i] / fs_ref + 1e-30));
        e->floor_db = 10 * log10(floor_ / fs_ref + 1e-30);
    }
    return 10 * log10(best / floor_ + 1e-30);
}

/* the share of bytes at 0 or 255, from every 4th sample */
static double clipped(const uint8_t *iq, int n) {
    int c = 0, t = 0;

    for (int i = 0; i + 1 < 2 * n; i += 8, t++) c += (iq[i] == 0 || iq[i] == 255) + (iq[i + 1] == 0 || iq[i + 1] == 255);
    return t ? c / (2.0 * t) : 0;
}

static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;

    return x < y ? -1 : x > y;
}

/* the elements in use as an array of their own, its steering made again only when they change */
static void sub_make(df_engine_t *e, unsigned use) {
    if (use == e->sub_mask && e->sub.n) return;
    e->sub_mask = use;
    e->sub.kind = e->arr.kind == DF_ULA ? DF_ULA : DF_CUSTOM;     /* a line less one is still a line */
    e->sub.size = e->arr.size;
    e->sub.n = 0;
    for (int m = 0; m < e->nch; m++)
        if (use & (1u << m)) {
            e->sub.x[e->sub.n] = e->arr.x[m];
            e->sub.y[e->sub.n] = e->arr.y[m];
            e->sub.n++;
        }
    if (e->sub.n) df_steer_make(&e->sub_steer, &e->sub, DF_C / (e->fc + e->f));
}

int df_engine_block(df_engine_t *e, const uint8_t *const iq[], int n, df_est_t *o) {
    int      nch = e->nch;
    unsigned all = (1u << nch) - 1, avail = all & ~e->user_off, use;
    double   dt;

    memset(o, 0, sizeof(*o));
    if (n > e->cap) n = e->cap;
    dt = n / e->fs;
    e->now += dt;
    e->since_cal += dt;

    /* every channel's level and clipping; the median of those in use */
    double lv[DF_MAXCH];
    int    nl = 0;

    for (int m = 0; m < nch; m++) {
        o->ch_db[m] = level(e, iq[m], n, m == 0 ? 8 : 4, m == 0);
        o->clip[m] = clipped(iq[m], n);
        if (avail & (1u << m)) lv[nl++] = o->ch_db[m];
    }
    qsort(lv, (size_t)nl, sizeof(double), dcmp);
    o->level_db = nl ? (nl & 1 ? lv[nl / 2] : (lv[nl / 2 - 1] + lv[nl / 2]) / 2) : 0;

    /* a dead antenna: the target well under the rest for a second while it is there */
    for (int m = 0; m < nch; m++) {
        if (o->level_db < e->sql_db) continue;      /* nothing to judge by: as it was */
        if (o->ch_db[m] < o->level_db - DF_LOW_DB) {
            if ((e->low_t[m] += dt) >= 1.0) e->low[m] = true;
        } else {
            e->low_t[m] = 0;
            if (o->ch_db[m] >= o->level_db - DF_LOW_BACK_DB) e->low[m] = false;
        }
    }
    use = avail;
    for (int m = 0; m < nch; m++)
        if (e->low[m]) use &= ~(1u << m);
    o->used = use;

    int k = df_chan_run(&e->chan, iq, n, e->dec);

    for (int m = 1; m < nch; m++)
        for (int i = 0; i < k; i++) e->dec[m][i] *= e->corr[m];

    /* R = E[x x^H], the elements in use */
    double complex R[DF_MAXCH * DF_MAXCH];
    int            idx[DF_MAXCH], nu = 0;

    for (int m = 0; m < nch; m++)
        if (use & (1u << m)) idx[nu++] = m;
    o->nused = nu;
    for (int i = 0; i < nu; i++)
        for (int j = i; j < nu; j++) {
            double sr = 0, si = 0;
            const float complex *a = e->dec[idx[i]], *b = e->dec[idx[j]];

            for (int t = 0; t < k; t++) {
                float ar = crealf(a[t]), ai = cimagf(a[t]), br = crealf(b[t]), bi = cimagf(b[t]);

                sr += ar * br + ai * bi;                /* a b* */
                si += ai * br - ar * bi;
            }
            R[i * nu + j] = (sr + I * si) / (k > 0 ? k : 1);
            R[j * nu + i] = conj(R[i * nu + j]);
        }
    o->k = k;

    const df_array_t *arr = &e->arr;
    const df_steer_t *st = &e->steer;
    int               need = e->arr.kind == DF_ULA ? 2 : 3;

    if (nu < nch && nu >= need) {
        sub_make(e, use);
        arr = &e->sub;
        st = &e->sub_steer;
    }
    bool doa_ok = nu >= need && k > nu && df_doa(R, arr, st, e->method, e->nsrc, e->front_only, &o->doa) == 0;

    o->have_doa = doa_ok;

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
    if (e->have_cal && doa_ok && nu >= 4 && o->level_db >= 12) {
        double fw[DF_MAXCH];
        int    b = 0;

        df_fit_without(R, arr, st, fw);
        for (int m = 1; m < nu; m++)
            if (fw[m] > fw[b]) b = m;
        /* the target well under the rest on that channel: a dead antenna (left out after a second), not a slip */
        if (fw[b] >= 0.9 && fw[b] - o->doa.fit >= 0.1 && o->ch_db[idx[b]] >= o->level_db - DF_LOW_DB)
            suspect = true, o->bad_ch = idx[b];
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
    if (!o->cal_wanted && e->cal_every_s > 0 && e->since_cal >= e->cal_every_s &&
        (e->sql_closed >= 0.3 || e->since_cal >= e->cal_every_s + 30))
        o->cal_wanted = 2;
    return 1;
}
