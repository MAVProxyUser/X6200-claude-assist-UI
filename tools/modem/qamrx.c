/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the shared phase and amplitude receiver (qamrx.h).
 */
#include "qamrx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ABS_ON        3e-4      /* the least level a burst is looked for at (-70 dBFS) */
#define RISE          2.5       /* over the quiet line's level: 8 dB (a 10 ms mean of noise varies a few dB) */
#define FIND_BEFORE   0.030     /* s before the rise the first symbol may be centred */
#define FIND_AFTER    0.012     /* and after */
#define PIECE         0.005     /* s, the pieces the turn-on is matched in */
#define FOUND         0.45      /* the match, against a perfect one, a burst is taken at */
#define QUIET_BELOW   0.25      /* of the burst's loudest: its carrier gone (v29.rs QUIET_BELOW) */
#define QUIET_NEAR    1.6       /* of the quiet line's level before the burst */
#define QUIET_OFF     0.020     /* s gone for, and the burst is over */
#define LOST_MSE      0.35      /* symbols this far off their points ... */
#define LOST_FOR      400       /* ... for this many: lost */
#define MU_TRAIN      0.03
#define MU_DATA       0.008
#define PLL_KP        0.02
#define PLL_KI_HZ     0.05      /* hertz a radian of error moves the carrier, a symbol */
#define TIME_KP       0.01
#define TIME_KI       5e-6
#define CENTRE_MOVE   0.7

static double hamming_sinc(int i, int n, double fc, double fs) {
    double t = i - (n - 1) / 2.0;
    double w = 0.54 - 0.46 * cos(TAU * i / (n - 1));
    double s = fabs(t) < 1e-9 ? 2.0 * fc / fs : sin(TAU * fc / fs * t) / (M_PI * t);
    return s * w;
}

int qamrx_init(qamrx_t *q, const qamrx_cfg_t *cfg, qam_symbol_fn fn, void *ctx) {
    memset(q, 0, sizeof(*q));
    q->cfg       = *cfg;
    q->on_symbol = fn;
    q->ctx       = ctx;
    q->sps       = cfg->fs / cfg->baud;
    q->hsp       = q->sps / 2.0;
    q->mix_step  = cfg->carrier / cfg->fs;

    double cutoff = 0.55 * cfg->baud + 300.0;
    if (cutoff > 0.45 * cfg->fs)
        cutoff = 0.45 * cfg->fs;
    q->nfir = (int)(3.3 * cfg->fs / 700.0) | 1;
    q->fir  = malloc(sizeof(double) * q->nfir);
    q->dl   = calloc((size_t)q->nfir * 2, sizeof(cplx));
    q->ring = calloc(QAM_RING, sizeof(cplx));
    if (!q->fir || !q->dl || !q->ring)
        return -1;
    /* twice: the mix took half the signal to the image */
    for (int i = 0; i < q->nfir; i++)
        q->fir[i] = 2.0 * hamming_sinc(i, q->nfir, cutoff, cfg->fs);
    onepole_init(&q->lev, 0.010, cfg->fs);
    q->tkp = getenv("QAM_TKP") ? atof(getenv("QAM_TKP")) : TIME_KP;
    q->tki = getenv("QAM_TKI") ? atof(getenv("QAM_TKI")) : TIME_KI;
    qamrx_reset(q);
    return 0;
}

void qamrx_free(qamrx_t *q) {
    free(q->fir);
    free(q->dl);
    free(q->ring);
    q->fir  = NULL;
    q->dl   = NULL;
    q->ring = NULL;
}

void qamrx_reset(qamrx_t *q) {
    /* the line's level as it last was is the quiet line's: the tail of a burst heard before is no new one */
    q->floor_    = q->lev.y;
    q->state     = QAM_IDLE;
    q->quiet_for = 0;
    q->loudest   = 0.0;
}

static inline cplx at(const qamrx_t *q, int64_t i) {
    return q->ring[(uint64_t)i % QAM_RING];
}

/* four-point Lagrange between baseband samples */
static cplx interp(const qamrx_t *q, double p) {
    int64_t i = (int64_t)floor(p);
    double  f = p - i;
    cplx    a = at(q, i - 1), b = at(q, i), c = at(q, i + 1), d = at(q, i + 2);
    return a * (-f * (f - 1.0) * (f - 2.0) / 6.0) + b * ((f + 1.0) * (f - 1.0) * (f - 2.0) / 2.0) +
           c * (-(f + 1.0) * f * (f - 2.0) / 2.0) + d * ((f + 1.0) * f * (f - 1.0) / 6.0);
}

static int slice(const qamrx_t *q, cplx y) {
    int    best = 0;
    double bd   = 1e30;
    for (int i = 0; i < q->cfg.npts; i++) {
        double d = creal((y - q->cfg.pts[i]) * conj(y - q->cfg.pts[i]));
        if (d < bd) {
            bd   = d;
            best = i;
        }
    }
    return best;
}

/* ---- finding the burst ---- */

/* The turn-on as it leaves the transmitter, from two symbols before the first's centre */
static cplx *make_template(const qamrx_t *q, int *m_out) {
    int    m   = (int)((q->cfg.find_syms + 2) * q->sps);
    cplx  *e   = calloc((size_t)m, sizeof(cplx));
    double rol = q->cfg.rolloff;

    if (!e)
        return NULL;
    for (int i = 0; i < m; i++) {
        double t  = i / q->sps - 2.0;
        int    k0 = (int)floor(t) - 8, k1 = (int)ceil(t) + 8;
        for (int k = k0 < 0 ? 0 : k0; k <= k1 && k < q->cfg.nknown; k++)
            e[i] += q->cfg.known[k] * rrc_at(t - k, rol);
    }
    *m_out = m;
    return e;
}

static double match_at(const qamrx_t *q, const cplx *e, int m, int64_t tau, int piece, cplx *pieces, double *norm) {
    double sum = 0.0, er = 0.0, ee = 0.0;
    int    w   = 0;
    for (int s = 0; s + piece <= m; s += piece, w++) {
        cplx c = 0;
        for (int i = s; i < s + piece; i++) {
            cplx r = at(q, tau + i);
            c += r * conj(e[i]);
            er += creal(r * conj(r));
            ee += creal(e[i] * conj(e[i]));
        }
        if (pieces)
            pieces[w] = c;
        sum += cabs(c);
    }
    *norm = sqrt(er * ee);
    return sum;
}

static bool acquire(qamrx_t *q) {
    int    m, piece = (int)lround(PIECE * q->cfg.fs);
    cplx  *e = make_template(q, &m);
    if (!e)
        return false;
    int     npieces = m / piece;
    cplx   *pieces  = calloc((size_t)npieces + 1, sizeof(cplx));
    int64_t lo      = (int64_t)q->trig - (int64_t)(FIND_BEFORE * q->cfg.fs) - (int64_t)(2 * q->sps);
    int64_t hi      = (int64_t)q->trig + (int64_t)(FIND_AFTER * q->cfg.fs) - (int64_t)(2 * q->sps);
    int64_t oldest  = (int64_t)q->n - QAM_RING + 4;
    double  best = -1.0, norm, bn = 1.0, cm1 = 0, cp1 = 0;
    int64_t bt   = lo;

    if (lo < oldest)
        lo = oldest;
    for (int64_t t = lo; t <= hi; t += 2) {
        double c = match_at(q, e, m, t, piece, NULL, &norm);
        if (c > best) {
            best = c;
            bt   = t;
        }
    }
    for (int64_t t = bt - 2; t <= bt + 2; t++) {
        double c = match_at(q, e, m, t, piece, NULL, &norm);
        if (c >= best) {
            best = c;
            bt   = t;
            bn   = norm;
        }
    }
    double c0 = match_at(q, e, m, bt, piece, pieces, &bn);
    cm1       = match_at(q, e, m, bt - 1, piece, NULL, &norm);
    cp1       = match_at(q, e, m, bt + 1, piece, NULL, &norm);
    double frac = 0.0, den = cm1 - 2 * c0 + cp1;
    if (den < 0)
        frac = 0.5 * (cm1 - cp1) / den;
    if (frac > 0.5)
        frac = 0.5;
    if (frac < -0.5)
        frac = -0.5;
    double found = bn > 0 ? c0 / bn : 0;
    if (getenv("QAM_DEBUG"))
        fprintf(stderr, "acquire: trig %llu best tau %lld (%+.0f samples from the rise) frac %.2f found %.3f\n",
                (unsigned long long)q->trig, (long long)bt, (double)(bt - (int64_t)q->trig), frac, found);
    free(e);
    if (found < FOUND) {
        free(pieces);
        return false;
    }

    /* the carrier: how the pieces turn, then a line through their phases */
    cplx d = 0;
    for (int w = 0; w + 1 < npieces; w++)
        d += pieces[w + 1] * conj(pieces[w]);
    double f1 = carg(d) * q->cfg.fs / (TAU * piece);
    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, prev = 0;
    for (int w = 0; w < npieces; w++) {
        double wt = cabs(pieces[w]);
        double ph = carg(pieces[w] * cexp(-I * TAU * f1 * w * piece / q->cfg.fs));
        if (w) {
            while (ph - prev > M_PI)
                ph -= TAU;
            while (ph - prev < -M_PI)
                ph += TAU;
        }
        prev = ph;
        sw += wt;
        sx += wt * w;
        sy += wt * ph;
        sxx += wt * w * w;
        sxy += wt * w * ph;
    }
    double slope = (sw * sxx - sx * sx) > 1e-12 ? (sw * sxy - sx * sy) / (sw * sxx - sx * sx) : 0;
    free(pieces);
    q->dfo = f1 + slope * q->cfg.fs / (TAU * piece);

    /* the taps: least squares over the known symbols, at the best of five alignments */
    double  p0    = bt + frac + 2 * q->sps;
    int     from  = q->cfg.solve_from, to = q->cfg.solve_to, rows = to - from;
    int64_t hbase = 2 * from - QAM_REACH - 4, hend = 2 * to + QAM_REACH + 4;
    int     nh    = (int)(hend - hbase);
    cplx   *h     = malloc(sizeof(cplx) * nh);
    cplx   *row   = malloc(sizeof(cplx) * rows * QAM_TAPS);
    cplx   *tgt   = malloc(sizeof(cplx) * rows);
    cplx    w[QAM_TAPS], bw[QAM_TAPS];
    double  bres = 1e30, pw = 0, power = 0;
    int     bd   = 0;

    for (int i = 0; i < nh; i++) {
        double p = p0 + (hbase + i) * q->hsp;
        h[i]     = interp(q, p) * cexp(-I * TAU * q->dfo * (p - p0) / q->cfg.fs);
        power += creal(h[i] * conj(h[i]));
    }
    power /= nh;
    for (int r = 0; r < rows; r++) {
        tgt[r] = q->cfg.known[from + r];
        pw += creal(tgt[r] * conj(tgt[r]));
    }
    for (int dl = -2; dl <= 2; dl++) {
        for (int r = 0; r < rows; r++)
            for (int i = 0; i < QAM_TAPS; i++)
                row[r * QAM_TAPS + i] = h[2 * (from + r) + dl - QAM_REACH + i - hbase];
        if (!least_squares(row, tgt, rows, QAM_TAPS, 1e-4 * rows * power, w))
            continue;
        double res = 0;
        for (int r = 0; r < rows; r++) {
            cplx y = 0;
            for (int i = 0; i < QAM_TAPS; i++)
                y += w[i] * row[r * QAM_TAPS + i];
            res += creal((tgt[r] - y) * conj(tgt[r] - y));
        }
        if (res < bres) {
            bres = res;
            bd   = dl;
            memcpy(bw, w, sizeof(w));
        }
    }
    free(h);
    free(row);
    free(tgt);
    q->trained_snr_db = bres < 1e29 ? 10.0 * log10(pw / (bres + 1e-30)) : -99.0;
    if (q->trained_snr_db < q->cfg.accept_db)
        return false;

    memcpy(q->taps, bw, sizeof(bw));
    q->hoff   = bd;
    q->k      = to;
    q->half   = 2 * to + bd - QAM_REACH - 2;
    q->pos    = p0 + q->half * q->hsp;
    q->ppre   = TAU * q->dfo * (q->half * q->hsp) / q->cfg.fs;
    q->theta  = 0.0;
    q->pin    = power;
    q->mse    = pow(10.0, -q->trained_snr_db / 10.0);
    q->shifts   = 0;
    q->lost     = 0;
    q->rate_err = 0;
    q->ted      = 0;
    q->slope    = 0.05;
    {
        double s = 0, si = 0;
        for (int i = 0; i < QAM_TAPS; i++) {
            double p = creal(q->taps[i] * conj(q->taps[i]));
            s += p;
            si += p * i;
        }
        q->where0 = bd + (s > 0 ? si / s - QAM_REACH : 0);
    }
    return true;
}

/* ---- following ---- */

static void symbol(qamrx_t *q) {
    cplx   row[QAM_TAPS], y = 0;
    int    k = q->k, label;
    cplx   d;
    bool   training = k < q->cfg.nknown;

    for (int i = 0; i < QAM_TAPS; i++) {
        row[i] = q->xh[(uint64_t)(2 * k + q->hoff - QAM_REACH + i) % QAM_HALVES];
        y += q->taps[i] * row[i];
    }
    y *= cexp(-I * q->theta);
    if (training) {
        d     = q->cfg.known[k];
        label = slice(q, d);
    } else {
        label = slice(q, y);
        d     = q->cfg.pts[label];
    }
    cplx   e  = d - y;
    double pe = cabs(d) > 1e-9 ? carg(y * conj(d)) : 0.0;
    q->mse    = 0.99 * q->mse + 0.01 * creal(e * conj(e));
    q->lost   = q->mse > LOST_MSE ? q->lost + 1 : 0;

    double g  = (training ? MU_TRAIN : MU_DATA) / (q->pin * QAM_TAPS + 1e-12);
    cplx   er = e * cexp(I * q->theta);
    for (int i = 0; i < QAM_TAPS; i++)
        q->taps[i] += g * er * conj(row[i]);
    q->theta += PLL_KP * pe;
    q->dfo += PLL_KI_HZ * pe;
    if (q->theta > M_PI)
        q->theta -= TAU;
    if (q->theta < -M_PI)
        q->theta += TAU;

    /* the clock, as BinModem's core has it: the error against the equaliser output's slope, the taps run a
     * half either side, which turns an error into how early or late the symbol was read. The grid is
     * moved after it and its spacing follows the far clock; the taps, whose own way of following a
     * drifting clock is the slowest thing they can learn, are left to equalise */
    cplx ym = 0, yp = 0;
    for (int i = 0; i < QAM_TAPS; i++) {
        ym += q->taps[i] * q->xh[(uint64_t)(2 * k + q->hoff - QAM_REACH + i - 1) % QAM_HALVES];
        yp += q->taps[i] * q->xh[(uint64_t)(2 * k + q->hoff - QAM_REACH + i + 1) % QAM_HALVES];
    }
    cplx dy  = (yp - ym) * cexp(-I * q->theta) * 0.5;
    q->slope = 0.99 * q->slope + 0.01 * creal(dy * conj(dy));
    double te = q->slope > 1e-12 ? -creal(e * conj(dy)) / q->slope : 0.0;
    if (te > 0.5)
        te = 0.5;
    if (te < -0.5)
        te = -0.5;
    q->pos -= q->tkp * te * q->hsp;
    q->rate_err -= q->tki * te;
    q->ted = 0.98 * q->ted + 0.02 * te;

    double s = 0, si = 0;
    for (int i = 0; i < QAM_TAPS; i++) {
        double p = creal(q->taps[i] * conj(q->taps[i]));
        s += p;
        si += p * i;
    }
    double cen       = s > 0 ? si / s - QAM_REACH : 0;
    q->timing_halves = cen;

    q->last = y;
    if (q->quiet_for == 0)
        q->snr_db = -10.0 * log10(q->mse + 1e-12);
    q->offset_hz = q->dfo;
    if (q->on_symbol)
        q->on_symbol(q->ctx, k, label, y);

    if (getenv("QAM_TRACE") && k % 250 == 0)
        fprintf(stderr, "k %5d mse %.4f cen %+.2f hoff %d te %+.3f ppm %+6.0f dfo %+.2f theta %+.2f\n", k, q->mse, cen,
                q->hoff, te, q->rate_err * 1e6, q->dfo, q->theta);
    if (cen > CENTRE_MOVE) {
        memmove(q->taps, q->taps + 1, sizeof(cplx) * (QAM_TAPS - 1));
        q->taps[QAM_TAPS - 1] = 0;
        q->hoff++;
        q->shifts++;
    } else if (cen < -CENTRE_MOVE) {
        memmove(q->taps + 1, q->taps, sizeof(cplx) * (QAM_TAPS - 1));
        q->taps[0] = 0;
        q->hoff--;
        q->shifts++;
    }
    q->k++;
}

static void track(qamrx_t *q) {
    while (q->state == QAM_TRACK && floor(q->pos) + 3 < (double)q->n) {
        cplx x = interp(q, q->pos) * cexp(-I * q->ppre);
        q->xh[(uint64_t)q->half % QAM_HALVES] = x;
        q->pin = 0.999 * q->pin + 0.001 * creal(x * conj(x));
        double step = q->hsp * (1.0 + q->rate_err);
        q->pos += step;
        q->ppre += TAU * q->dfo * step / q->cfg.fs;
        if (q->ppre > M_PI)
            q->ppre -= TAU;
        if (q->ppre < -M_PI)
            q->ppre += TAU;
        if (q->half == 2 * q->k + q->hoff + QAM_REACH)
            symbol(q);
        q->half++;
    }
}

void qamrx_feed(qamrx_t *q, double x) {
    double c = cos(TAU * q->mix_phase), s = sin(TAU * q->mix_phase);
    q->mix_phase += q->mix_step;
    q->mix_phase -= floor(q->mix_phase);

    cplx v              = x * (c - I * s);
    q->dl[q->dl_at]     = v;
    q->dl[q->dl_at + q->nfir] = v;
    q->dl_at            = (q->dl_at + 1) % q->nfir;
    const cplx *src     = q->dl + q->dl_at;
    double      re = 0, im = 0;
    for (int i = 0; i < q->nfir; i++) {
        re += q->fir[i] * creal(src[i]);
        im += q->fir[i] * cimag(src[i]);
    }
    cplx z                         = re + I * im;
    q->ring[q->n % QAM_RING]       = z;
    q->n++;
    double lev = onepole_run(&q->lev, cabs(z));
    q->level   = lev;

    switch (q->state) {
    case QAM_IDLE:
        if (q->n < (uint64_t)(0.05 * q->cfg.fs)) {
            q->floor_ = lev;
            break;
        }
        if (lev > RISE * q->floor_ && lev > ABS_ON) {
            q->trig    = q->n;
            q->loudest = lev;
            q->state   = QAM_COLLECT;
            break;
        }
        q->floor_ += (lev - q->floor_) / ((lev < q->floor_ ? 0.02 : 0.5) * q->cfg.fs);
        break;
    case QAM_COLLECT: {
        if (lev > q->loudest)
            q->loudest = lev;
        double need = (FIND_AFTER + 0.002) * q->cfg.fs + (2 * q->cfg.solve_to + QAM_REACH + 8) * q->hsp + 8;
        double tmpl = (FIND_AFTER + 0.002) * q->cfg.fs + (q->cfg.find_syms + 4) * q->sps;
        if (tmpl > need)
            need = tmpl;
        if ((double)(q->n - q->trig) >= need) {
            if (acquire(q)) {
                q->state     = QAM_TRACK;
                q->quiet_for = 0;
                track(q);
            } else {
                q->fails++;
                q->state = QAM_ENDED;
            }
        }
        break;
    }
    case QAM_TRACK:
        q->loudest = fmax(q->loudest, lev) * (1.0 - 1.0 / (2.0 * q->cfg.fs));
        /* gone: well under the burst's loudest, or back near the quiet line's level from before it (on a noisy
         * line the noise alone is more than a quarter of the burst) */
        q->quiet_for = lev < QUIET_BELOW * q->loudest || lev < QUIET_NEAR * q->floor_ ? q->quiet_for + 1 : 0;
        if (q->quiet_for > QUIET_OFF * q->cfg.fs || q->lost > LOST_FOR) {
            q->state = QAM_ENDED;
            break;
        }
        track(q);
        break;
    case QAM_ENDED:
        /* the line as it is now is the floor, so a burst is looked for only when the level next rises:
         * not again in the one that failed, and at once after one that ended */
        q->floor_    = lev;
        q->quiet_for = 0;
        q->state     = QAM_IDLE;
        break;
    }
}
