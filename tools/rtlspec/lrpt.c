/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Meteor-M LRPT: the demodulator, the frames, the packets and the pictures (lrpt.h says what each is).
 *
 *  The demodulator: the channel's samples turned by an NCO (the carrier), through a root raised cosine matched filter
 *  computed only at the instants wanted (a polyphase bank, PHASES steps between two samples: the filter and the
 *  timing's interpolation in one), two a symbol: on time and between. Gardner's timing detector on them, a Costas loop
 *  for the carrier on the symbols (QPSK: I and Q on time; offset QPSK: I on time, Q half a symbol later), both second
 *  order loops. Before its loop locks, the carrier is looked for with the fourth power of the on-time samples (the
 *  modulation gone, a line at four times the offset), a spectrum of it now and then (acquire()). The soft bits out: I then Q of
 *  each symbol, scaled to about +-40.
 *
 *  The bits: the 80k mode's marker found (0x27 each 80 bits, under each of the 8 ways the constellation may be
 *  turned or mirrored), the markers taken out and the interleaver undone; the newer satellites' differential coding
 *  undone (each bit against the one two before: its rail's last); then the frames' sync word, coded, looked for bit by
 *  bit (offset QPSK may pair its rails either way) under the 8 ways again. Each frame then decoded by the Viterbi from
 *  a little before its sync word to a little into the next one's, unscrambled, its four RS codewords corrected (in
 *  the conventional basis: Meteor-M2-2's frames of 2019 have their syndromes 0 so and not in CCSDS's dual basis; the
 *  dual basis tried as well), and its VCDU's packets put together. The imager's packets' MCUs decoded into each channel's picture.
 */

#include "lrpt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define ALPHA      0.6f         /* the root raised cosine's roll-off */
#define RRC_SPAN   4            /* symbols each side of the matched filter's centre */
#define PHASES     128          /* the filter's fractional steps between two samples */
#define SOFT       40.0f        /* a soft bit's size for a clean symbol */

#define F_BITS     16384        /* coded bits a frame (CADU): 1024 bytes, rate 1/2 */
#define PRE        64           /* bit pairs decoded before a frame's start and after its end, for the Viterbi */
#define POST       64
#define SB_CAP     (4 * F_BITS + 4 * (PRE + POST))
#define SYNC_SRCH  12           /* coded sync word: its 52 sure bits wrong at most, found by a search */
#define SYNC_LOCK  18           /*                                              ..., where the last frame says */
#define MISSES     6            /* frames without their sync word in a row before it is looked for again */

#define IL_MARK    0x27         /* 80k: the marker byte */
#define IL_STRIDE  80           /* bits from one marker to the next, 8 of them the marker's */
#define IL_N       36           /* the interleaver's branches */
#define IL_M       2048         /* each branch's delay more than the last, in its own bits */
#define IL_DELAY   ((IL_N - 1) * IL_M * IL_N)   /* bits each spends in the interleaver and deinterleaver together */
#define IL_SRCH    (IL_STRIDE * 128)            /* bits looked through for the marker */
#define IL_BACK    (4 * IL_M * IL_N)            /* the deinterleaver's output from so far before its first bit */
#define IL_RING    (IL_DELAY + IL_BACK + IL_STRIDE)

#define PKT_MAX    8192         /* a packet's bytes at most (the imager's are under 1100) */
#define STRIP_CAP  64           /* strips a picture grows by */
#define MAX_STRIPS 1500         /* strips a picture has at most: 31 minutes, a pass is 15 */

static const struct {
    float rate;                 /* symbols a second */
    bool  oq;                   /* offset QPSK */
    bool  diff;                 /* differentially coded */
    bool  il;                   /* interleaved, with markers */
} modes[LRPT_MODES] = {
    [LRPT_72K_OQPSK] = {72000, true, true, false},
    [LRPT_80K_OQPSK] = {80000, true, true, true},
    [LRPT_72K_QPSK]  = {72000, false, false, false},
    [LRPT_80K_QPSK]  = {80000, false, false, true},
};

/* ---- The demodulator ---- */

typedef struct {
    float  fs, rs, sps;
    bool   oq;
    int    L;                   /* the filter's taps at each phase */
    float *taps;                /* [PHASES][L], each phase's taps oldest sample first */
    float *xr, *xi;             /* the last L samples, twice over: a window in one piece */
    int    at;
    long   n;                   /* the latest sample's number */
    double t;                   /* the next output's instant, in samples */
    float  half;                /* samples a half symbol, the timing loop's rate in it */
    float  half0;
    float  tk1, tk2;            /* the timing loop's gains */
    float  ck1, ck2;            /* the carrier loop's */
    float  pr, pi;              /* the NCO: e^-j(phase) */
    float  fr;                  /* its step a sample, radians */
    float  fr_avg;              /* and its average over about a tenth of a second (what is said of it) */
    float  sr, si;              /* e^-j(fr) */
    int    next_half;           /* 0: the next output is on time, 1: between */
    unsigned nsym;              /* symbols so far */
    float  on_r[2], on_i[2];    /* on time: the symbol before [0] and this one [1] */
    float  mid_r[2], mid_i[2];  /* between: before this symbol [0] and after it [1] */
    float  pow;                 /* the symbols' power (the AGC) */
    float  lk_num, lk_den;      /* the carrier's lock: -Re(z^4) and |z|^4 of the symbols, averaged; their ratio 1
                                 * locked (QPSK's points at 45 degrees: z^4 negative real), 0 turning or noise */
    /* the carrier looked for: the outputs' fourth power's spectrum */
    float *fbuf;                /* FFT_N complex */
    float *fpow;                /* FFT_N */
    int    fn, fblocks;
    bool   acquire;
    float *cbuf, *cpow;         /* the input's own spectrum (CEN_N complex), its power averaged */
    int    cn, cblocks;
} demod_t;

#define LOCKED   0.4f           /* the lock above which the carrier is left to its loop */
#define FFT_N    4096
#define FFT_AVG  4
#define CEN_N    1024

static bool debug_dm;           /* LRPT_DEBUG: the carrier's estimates on stderr */

static float rrc(float t) {     /* t in symbols */
    const float a = ALPHA, pi = (float)M_PI;

    if (fabsf(t) < 1e-6f) return 1 - a + 4 * a / pi;
    if (fabsf(fabsf(t) - 1 / (4 * a)) < 1e-5f)
        return a / sqrtf(2) * ((1 + 2 / pi) * sinf(pi / (4 * a)) + (1 - 2 / pi) * cosf(pi / (4 * a)));
    return (sinf(pi * t * (1 - a)) + 4 * a * t * cosf(pi * t * (1 + a))) / (pi * t * (1 - 16 * a * a * t * t));
}

/* A second order loop's gains for a noise bandwidth bn (of the update rate) and a detector's gain kp */
static void loop_gains(float bn, float kp, float *k1, float *k2) {
    const float z  = 0.7071f;
    float       th = bn / (z + 1 / (4 * z)), d = 1 + 2 * z * th + th * th;

    *k1 = 4 * z * th / d / kp;
    *k2 = 4 * th * th / d / kp;
}

static void demod_free(demod_t *d) {
    free(d->taps);
    free(d->xr);
    free(d->xi);
    free(d->fbuf);
    free(d->fpow);
    free(d->cbuf);
    free(d->cpow);
    memset(d, 0, sizeof(*d));
}

static bool demod_init(demod_t *d, float fs, float rs, bool oq) {
    demod_free(d);
    d->fs  = fs;
    d->rs  = rs;
    d->sps = fs / rs;
    d->oq  = oq;
    d->L   = 2 * (int)ceilf(RRC_SPAN * d->sps) + 2;
    d->taps = calloc((size_t)(PHASES * d->L), sizeof(float));
    d->xr   = calloc((size_t)(2 * d->L), sizeof(float));
    d->xi   = calloc((size_t)(2 * d->L), sizeof(float));
    d->fbuf = calloc(2 * FFT_N, sizeof(float));
    d->fpow = calloc(FFT_N, sizeof(float));
    d->cbuf = calloc(2 * CEN_N, sizeof(float));
    d->cpow = calloc(CEN_N, sizeof(float));
    if (!d->taps || !d->xr || !d->xi || !d->fbuf || !d->fpow || !d->cbuf || !d->cpow) return false;
    debug_dm = getenv("LRPT_DEBUG") != NULL;

    /* y(n + mu) = sum over k of x[n - k] h((k + mu - D) / sps), D the filter's middle; a Hann window over its span */
    float centre = (d->L - 1) / 2.0f, sum = 0;
    for (int p = 0; p < PHASES; p++) {
        float mu = (float)p / PHASES;
        for (int k = 0; k < d->L; k++) {
            float tt = (k + mu - centre) / d->sps, w = 0;
            if (fabsf(tt) < RRC_SPAN + 0.5f) w = 0.5f + 0.5f * cosf((float)M_PI * tt / (RRC_SPAN + 0.5f));
            d->taps[p * d->L + (d->L - 1 - k)] = rrc(tt) * w;
            if (p == 0) sum += rrc(tt) * w;
        }
    }
    for (int i = 0; i < PHASES * d->L; i++) d->taps[i] /= sum;
    d->n      = -1;
    d->t      = d->L;
    d->half0  = d->half = d->sps / 2;
    d->pr     = 1;
    d->sr     = 1;
    d->pow    = 0;

    d->acquire = true;
    loop_gains(0.006f, 0.6f, &d->tk1, &d->tk2);
    d->tk1 *= d->sps;           /* the timing's correction in samples */
    d->tk2 *= d->sps;
    loop_gains(0.005f, 2.0f, &d->ck1, &d->ck2);
    return true;
}

/* A complex FFT in place, n a power of two */
static void fft(float *x, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = x[2 * i], ti = x[2 * i + 1];
            x[2 * i] = x[2 * j], x[2 * i + 1] = x[2 * j + 1];
            x[2 * j] = tr, x[2 * j + 1] = ti;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a  = -2 * M_PI / len;
        float  wr = (float)cos(a), wi = (float)sin(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                float *u = x + 2 * (i + k), *v = x + 2 * (i + k + len / 2);
                float  tr = v[0] * cr - v[1] * ci, ti = v[0] * ci + v[1] * cr;
                v[0] = u[0] - tr, v[1] = u[1] - ti;
                u[0] += tr, u[1] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static void nco_step(demod_t *d) {
    d->sr = cosf(d->fr);
    d->si = -sinf(d->fr);
}

/* The carrier while its loop has not locked: two spectra. The fourth power of the on-time samples (one a symbol:
 * the fourth power of all the outputs, two a symbol, has lines a symbol rate either side of the carrier's own, and at
 * two samples a symbol one of those won, a -4 kHz carrier taken for +14 kHz) has a line at four times the carrier's
 * offset: precise, but only modulo a quarter of the symbol rate (18 kHz at 72k). The input's own power spectrum, its
 * middle (the signal's spectrum is symmetric about its carrier; the noise's floor taken off), says which of those it
 * is: coarse but unambiguous */
static void centre_in(demod_t *d, float r, float i) {
    d->cbuf[2 * d->cn]     = r;
    d->cbuf[2 * d->cn + 1] = i;
    if (++d->cn < CEN_N) return;
    d->cn = 0;
    fft(d->cbuf, CEN_N);
    for (int k = 0; k < CEN_N; k++) d->cpow[k] += d->cbuf[2 * k] * d->cbuf[2 * k] + d->cbuf[2 * k + 1] * d->cbuf[2 * k + 1];
    d->cblocks++;
}

/* The input spectrum's middle, Hz from the NCO's frequency: the power over the floor's (its lowest fifth's mean)
 * weighted by frequency, within the symbol rate either side; NAN when there is not enough of it */
static float centre_hz(demod_t *d) {
    static float sorted[CEN_N];
    float        floor = 0, num = 0, den = 0, bin = d->fs / CEN_N;
    int          span = (int)(d->rs / bin);

    if (d->cblocks < 4) return NAN;
    if (span > CEN_N / 2 - 1) span = CEN_N / 2 - 1;
    memcpy(sorted, d->cpow, sizeof(sorted));
    for (int a = 0; a < CEN_N / 5; a++) {               /* the lowest fifth, found by selection: once a second */
        int m = a;
        for (int b = a + 1; b < CEN_N; b++)
            if (sorted[b] < sorted[m]) m = b;
        float t = sorted[a];
        sorted[a] = sorted[m], sorted[m] = t;
        floor += sorted[a];
    }
    floor /= CEN_N / 5;
    for (int k = -span; k <= span; k++) {
        float p = d->cpow[(k + CEN_N) % CEN_N] - floor;
        if (p > 0) num += p * k * bin, den += p;
    }
    memset(d->cpow, 0, CEN_N * sizeof(float));
    d->cblocks = 0;
    return den > 3 * floor * span ? num / den : NAN;      /* a signal clearly over the floor's spread */
}

/* An on-time output into the fourth power's spectrum; after FFT_AVG of them, the carrier set */
static void acquire(demod_t *d, float yr, float yi) {
    float r2 = yr * yr - yi * yi, i2 = 2 * yr * yi;         /* y^2, then y^4, scaled by |y|^-3 */
    float r4 = r2 * r2 - i2 * i2, i4 = 2 * r2 * i2, m = sqrtf(yr * yr + yi * yi);

    m = m > 1e-9f ? 1 / (m * m * m) : 0;
    d->fbuf[2 * d->fn]     = r4 * m;
    d->fbuf[2 * d->fn + 1] = i4 * m;
    if (++d->fn < FFT_N) return;
    d->fn = 0;
    fft(d->fbuf, FFT_N);
    for (int k = 0; k < FFT_N; k++) d->fpow[k] += d->fbuf[2 * k] * d->fbuf[2 * k] + d->fbuf[2 * k + 1] * d->fbuf[2 * k + 1];
    if (++d->fblocks < FFT_AVG) return;
    d->fblocks = 0;

    /* the peak, against the spectrum's mean */
    float best = 0, mean = 0;
    int   kb = 0;
    for (int k = 0; k < FFT_N; k++) mean += d->fpow[k];
    mean /= FFT_N;
    for (int k = -FFT_N / 2 + 1; k < FFT_N / 2; k++) {
        float p = d->fpow[(k + FFT_N) % FFT_N];
        if (p > best) best = p, kb = k;
    }
    if (best > 12 * mean) {
        float a = d->fpow[(kb - 1 + FFT_N) % FFT_N], b = best, c = d->fpow[(kb + 1 + FFT_N) % FFT_N];
        float dk = (a - c) / (2 * (a - 2 * b + c) + 1e-20f);
        float df = (kb + (fabsf(dk) < 1 ? dk : 0)) * d->rs / FFT_N / 4;     /* Hz the carrier is still off */
        float cf = centre_hz(d), q = d->rs / 4;
        int   n  = isnan(cf) ? 0 : (int)lrintf((cf - df) / q);              /* which quarter of the symbol rate */

        if (n > 2) n = 2;
        if (n < -2) n = -2;
        df += n * q;
        if (debug_dm)
            fprintf(stderr, "lrpt: carrier %+.1f Hz more (the spectrum's middle %+.0f Hz; peak %.0f x the mean)\n", df,
                    cf, best / mean);
        d->fr += 2 * (float)M_PI * df / d->fs;
        d->fr_avg = d->fr;
        nco_step(d);
    }
    memset(d->fpow, 0, FFT_N * sizeof(float));
}

static inline float clip1(float x) { return x > 1 ? 1 : x < -1 ? -1 : x; }
static inline float sgnf(float x) { return x < 0 ? -1.0f : 1.0f; }
static inline int8_t soft8(float x) {
    float v = x * SOFT;
    return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : lrintf(v));
}

/* The loops on a symbol whose outputs are all in; its soft bits into out (2), the count returned */
static int demod_symbol(demod_t *d, int8_t *out) {
    float g = d->pow > 0 ? sqrtf(2 / d->pow) : 1;
    float pr = d->on_r[0] * g, cr = d->on_r[1] * g, mr = d->mid_r[0] * g;
    float pi = d->on_i[0] * g, ci = d->on_i[1] * g, mi = d->mid_i[0] * g;
    float te, pe, si, sq;

    if (!d->oq) {
        si = cr, sq = ci;
        te = mr * (pr - cr) + mi * (pi - ci);
        pe = sgnf(cr) * ci - sgnf(ci) * cr;
    } else {                    /* I on time, Q half a symbol later: its own on-time and between points */
        float nr = d->mid_r[1] * g, ni = d->mid_i[1] * g;
        si = cr, sq = ni;
        te = mr * (pr - cr) + ci * (mi - ni);
        pe = sgnf(cr) * ci - sgnf(ni) * nr;
    }
    /* the AGC and the quality on the symbol's two rails */
    float p = (si * si + sq * sq) / (g * g);
    d->pow = d->pow > 0 ? d->pow + 0.002f * (p - d->pow) : p;
    {
        float a2 = si * si - sq * sq, b2 = 2 * si * sq, m2 = si * si + sq * sq;
        d->lk_num += 0.001f * (-(a2 * a2 - b2 * b2) - d->lk_num);
        d->lk_den += 0.001f * (m2 * m2 - d->lk_den);
    }

    /* timing: late gives a negative error, the next instants drawn in */
    te = clip1(te);
    d->t += d->tk1 * te;
    d->half += d->tk2 * te / 2;
    if (d->half > d->half0 * 1.01f) d->half = d->half0 * 1.01f;
    if (d->half < d->half0 * 0.99f) d->half = d->half0 * 0.99f;
    /* carrier */
    pe = clip1(pe);
    {   /* the phase's step is small (under 0.02 rad): e^-ja as 1 - a^2/2 - ja; the NCO's step, its frequency, made
         * again every 16 symbols (sinf and cosf each symbol were a tenth of the demodulator's time on the radio) */
        float a = d->ck1 * pe, cr2 = 1 - a * a / 2, nr = d->pr * cr2 + d->pi * a;
        d->pi = d->pi * cr2 - d->pr * a;
        d->pr = nr;
        d->fr += d->ck2 * pe / d->sps;
        d->fr_avg += 1e-4f * (d->fr - d->fr_avg);
        if (d->fr > 0.5f) d->fr = 0.5f;
        if (d->fr < -0.5f) d->fr = -0.5f;
        if (!(++d->nsym & 15)) {
            nco_step(d);
            float m = 1 / sqrtf(d->pr * d->pr + d->pi * d->pi);      /* the NCO kept on the unit circle */
            d->pr *= m, d->pi *= m;
        }
    }
    out[0] = soft8(si);
    out[1] = soft8(sq);
    return 2;
}

/* One complex sample in; soft bits out (2 a symbol, at most 4 a sample), the count returned */
static int demod_sample(demod_t *d, float xr, float xi, int8_t *out) {
    int L = d->L, got = 0;
    float r = xr * d->pr - xi * d->pi, i = xr * d->pi + xi * d->pr;
    float nr = d->pr * d->sr - d->pi * d->si;

    d->pi = d->pr * d->si + d->pi * d->sr;
    d->pr = nr;
    if (d->acquire) centre_in(d, r, i);
    d->xr[d->at] = d->xr[d->at + L] = r;
    d->xi[d->at] = d->xi[d->at + L] = i;
    d->at = d->at + 1 == L ? 0 : d->at + 1;
    if (++d->n >= (1L << 24)) {
        d->n -= 1L << 24;
        d->t -= 1L << 24;
    }
    while (d->t < d->n + 1) {
        int          ph = (int)((d->t - d->n) * PHASES);
        const float *tp = d->taps + (ph < 0 ? 0 : ph >= PHASES ? PHASES - 1 : ph) * L;
        const float *wr = d->xr + d->at, *wi = d->xi + d->at;
        float        yr = 0, yi = 0;
#if defined(__ARM_NEON)
        float32x4_t ar = vdupq_n_f32(0), ai = vdupq_n_f32(0);
        int         k = 0;
        for (; k + 4 <= L; k += 4) {
            float32x4_t tv = vld1q_f32(tp + k);
            ar = vmlaq_f32(ar, vld1q_f32(wr + k), tv);
            ai = vmlaq_f32(ai, vld1q_f32(wi + k), tv);
        }
        float32x2_t sr2 = vadd_f32(vget_low_f32(ar), vget_high_f32(ar)), si2 = vadd_f32(vget_low_f32(ai), vget_high_f32(ai));
        yr = vget_lane_f32(vpadd_f32(sr2, sr2), 0);
        yi = vget_lane_f32(vpadd_f32(si2, si2), 0);
        for (; k < L; k++) yr += wr[k] * tp[k], yi += wi[k] * tp[k];
#else
        for (int k = 0; k < L; k++) yr += wr[k] * tp[k], yi += wi[k] * tp[k];
#endif
        if (d->next_half == 0) {            /* on time */
            if (d->acquire) acquire(d, yr, yi);
            d->on_r[0] = d->on_r[1], d->on_i[0] = d->on_i[1];
            d->on_r[1] = yr, d->on_i[1] = yi;
            if (!d->oq) got += demod_symbol(d, out + got);
        } else {                            /* between */
            if (!d->oq) {
                d->mid_r[0] = yr, d->mid_i[0] = yi;
            } else {
                d->mid_r[1] = yr, d->mid_i[1] = yi;
                got += demod_symbol(d, out + got);
                d->mid_r[0] = yr, d->mid_i[0] = yi;
            }
        }
        d->next_half ^= 1;
        d->t += d->half;
    }
    return got;
}

/* ---- The convolutional code and its Viterbi ---- */

static inline int parity(unsigned x) { return __builtin_parity(x); }


int lrpt_conv_encode(const uint8_t *bits, size_t n, uint8_t *out, int state) {
    for (size_t i = 0; i < n; i++) {
        unsigned r = (((unsigned)state << 1) | (bits[i] & 1)) & 0x7f;
        out[2 * i]     = (uint8_t)parity(r & 0x4f);           /* G1 171 */
        out[2 * i + 1] = (uint8_t)parity(r & 0x6d);           /* G2 133 */
        state          = (int)(r & 0x3f);
    }
    return state;
}

/* The butterflies: states j and j + 32 lead to 2j and 2j + 1. Into 2j from j, the coded bits' signs (+1 a 0) on the
 * pair's two soft bits; from j + 32, both the other way. The same into 2j + 1 */
static int16_t vit_sa[2][32], vit_sb[2][32];    /* [even/odd successor][j] */

/* np bit pairs (positive a 0) decoded; the bits from pair `from` on, nbits of them, into out MSB first. dec: np words,
 * each step's decisions: bit j that of state 2j, bit 32 + j that of state 2j + 1 (from j + 32 rather than j).
 * 16-bit path metrics, the first state's taken off all each step (their spread stays within a few thousand) */
static void viterbi(const int8_t *s, int np, uint64_t *dec, uint8_t *out, int from, int nbits) {
    int16_t pm[64] = {0};

#if defined(__ARM_NEON)
    int16x8_t lo[4], hi[4], sa[2][4], sb[2][4];
    const uint8x8_t w = {1, 2, 4, 8, 16, 32, 64, 128};
    for (int q = 0; q < 4; q++) {
        lo[q] = vdupq_n_s16(0), hi[q] = vdupq_n_s16(0);
        for (int e = 0; e < 2; e++) sa[e][q] = vld1q_s16(&vit_sa[e][8 * q]), sb[e][q] = vld1q_s16(&vit_sb[e][8 * q]);
    }
    for (int k = 0; k < np; k++) {
        int16x8_t a = vdupq_n_s16(s[2 * k]), b = vdupq_n_s16(s[2 * k + 1]), n[2][4];
        uint16x8_t d[2][4];
        for (int e = 0; e < 2; e++)
            for (int q = 0; q < 4; q++) {
                int16x8_t bm = vmlaq_s16(vmulq_s16(sa[e][q], a), sb[e][q], b);
                int16x8_t m0 = vqaddq_s16(lo[q], bm), m1 = vqsubq_s16(hi[q], bm);
                d[e][q] = vcgtq_s16(m1, m0);
                n[e][q] = vmaxq_s16(m0, m1);
            }
        uint32_t de[2];
        for (int e = 0; e < 2; e++) {
            uint8x8_t b0 = vand_u8(vmovn_u16(d[e][0]), w), b1 = vand_u8(vmovn_u16(d[e][1]), w);
            uint8x8_t b2 = vand_u8(vmovn_u16(d[e][2]), w), b3 = vand_u8(vmovn_u16(d[e][3]), w);
            uint8x8_t t  = vpadd_u8(vpadd_u8(b0, b1), vpadd_u8(b2, b3));
            de[e]        = vget_lane_u32(vreinterpret_u32_u8(vpadd_u8(t, t)), 0);
        }
        dec[k] = (uint64_t)de[1] << 32 | de[0];
        /* the new metrics in order: 2j, 2j+1 side by side; the first state's off all */
        int16x8x2_t z0 = vzipq_s16(n[0][0], n[1][0]), z1 = vzipq_s16(n[0][1], n[1][1]);
        int16x8x2_t z2 = vzipq_s16(n[0][2], n[1][2]), z3 = vzipq_s16(n[0][3], n[1][3]);
        int16x8_t   ref = vdupq_n_s16(vgetq_lane_s16(z0.val[0], 0));
        lo[0] = vsubq_s16(z0.val[0], ref), lo[1] = vsubq_s16(z0.val[1], ref);
        lo[2] = vsubq_s16(z1.val[0], ref), lo[3] = vsubq_s16(z1.val[1], ref);
        hi[0] = vsubq_s16(z2.val[0], ref), hi[1] = vsubq_s16(z2.val[1], ref);
        hi[2] = vsubq_s16(z3.val[0], ref), hi[3] = vsubq_s16(z3.val[1], ref);
    }
    for (int q = 0; q < 4; q++) vst1q_s16(pm + 8 * q, lo[q]), vst1q_s16(pm + 32 + 8 * q, hi[q]);
#else
    for (int k = 0; k < np; k++) {
        int16_t  a = s[2 * k], b = s[2 * k + 1], nm[64];
        uint64_t d = 0;
        for (int e = 0; e < 2; e++)
            for (int j = 0; j < 32; j++) {
                int bm = vit_sa[e][j] * a + vit_sb[e][j] * b, m0 = pm[j] + bm, m1 = pm[j + 32] - bm;
                if (m1 > m0) d |= 1ULL << (32 * e + j);
                nm[2 * j + e] = (int16_t)(m1 > m0 ? m1 : m0);
            }
        dec[k] = d;
        for (int i = 0; i < 64; i++) pm[i] = (int16_t)(nm[i] - nm[0]);
    }
#endif
    int ns = 0;
    for (int i = 1; i < 64; i++)
        if (pm[i] > pm[ns]) ns = i;
    memset(out, 0, (size_t)(nbits + 7) / 8);
    for (int k = np - 1; k >= 0; k--) {
        int j = k - from;
        if (j >= 0 && j < nbits && (ns & 1)) out[j >> 3] |= (uint8_t)(0x80 >> (j & 7));
        ns = (ns >> 1) | (int)(((dec[k] >> ((ns & 1) * 32 + (ns >> 1))) & 1) << 5);
    }
}

/* ---- Reed-Solomon (255,223), CCSDS: GF(2^8) of x^8+x^7+x^2+x+1, the code's roots alpha^(11 j), j 112-143; its
 * symbols on the air in the conventional basis (Meteor) or CCSDS's dual basis (through the matrix of CCSDS 131.0-B
 * Annex F) ---- */

static uint8_t gf_exp[512], gf_log[256], tal[256], tal1[256];
static uint8_t rs_gen[33];      /* the generator, conventional, highest power first */
static bool    tables_done;

static inline uint8_t gf_mul(uint8_t a, uint8_t b) { return a && b ? gf_exp[gf_log[a] + gf_log[b]] : 0; }
static inline uint8_t gf_pow11(int e) { e %= 255; if (e < 0) e += 255; return gf_exp[(e * 11) % 255]; }  /* beta^e */

static const uint8_t jpeg_dc_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t jpeg_dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t jpeg_ac_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t jpeg_ac_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09,
    0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65,
    0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
    0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

typedef struct {
    int maxcode[18], valptr[17], mincode[17];
    const uint8_t *vals;
} huff_t;
static huff_t huff_dc, huff_ac;
static float  idct_c[8][8];     /* C(u)/2 cos((2x+1) u pi / 16) */

static void huff_build(huff_t *h, const uint8_t bits[16], const uint8_t *vals) {
    int code = 0, k = 0;

    h->vals = vals;
    for (int len = 1; len <= 16; len++) {
        h->valptr[len]  = k;
        h->mincode[len] = code;
        code += bits[len - 1];
        k += bits[len - 1];
        h->maxcode[len] = bits[len - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
}

static void tables(void) {
    if (tables_done) return;
    /* the field */
    unsigned x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = gf_exp[i + 255] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x187;
    }
    gf_exp[510] = gf_exp[0];
    /* the dual basis: each conventional bit's dual basis byte (CCSDS 131.0-B, Annex F) */
    static const uint8_t T[8] = {0x8d, 0xef, 0xec, 0x86, 0xfa, 0x99, 0xaf, 0x7b};
    for (int i = 0; i < 256; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++)
            if (i & (1 << k)) v ^= T[7 - k];
        tal[i]  = v;            /* conventional to dual */
        tal1[v] = (uint8_t)i;   /* dual to conventional */
    }
    /* the generator: the product of (x - beta^j), j 112..143 */
    uint8_t g[33] = {1};
    for (int j = 112; j < 144; j++) {
        uint8_t r = gf_pow11(j);
        for (int k = 32; k > 0; k--) g[k] = g[k] ^ gf_mul(g[k - 1], r);
    }
    memcpy(rs_gen, g, sizeof(g));
    /* the convolutional code's outputs: into state ns from ns >> 1 (its oldest bit 0), the register is ns */
    for (int ns = 0; ns < 64; ns++) {
        vit_sa[ns & 1][ns >> 1] = parity(ns & 0x4f) ? -1 : 1;
        vit_sb[ns & 1][ns >> 1] = parity(ns & 0x6d) ? -1 : 1;
    }
    /* JPEG */
    huff_build(&huff_dc, jpeg_dc_bits, jpeg_dc_vals);
    huff_build(&huff_ac, jpeg_ac_bits, jpeg_ac_vals);
    for (int xx = 0; xx < 8; xx++)
        for (int u = 0; u < 8; u++)
            idct_c[xx][u] = (u ? 1.0f : (float)M_SQRT1_2) / 2 * cosf((2 * xx + 1) * u * (float)M_PI / 16);
    tables_done = true;
}

void lrpt_rs_encode(const uint8_t data[223], uint8_t parity_out[32], bool dual) {
    uint8_t p[32] = {0};

    tables();
    for (int i = 0; i < 223; i++) {
        uint8_t fb = (dual ? tal1[data[i]] : data[i]) ^ p[0];
        memmove(p, p + 1, 31);
        p[31] = 0;
        if (fb)
            for (int k = 0; k < 32; k++) p[k] ^= gf_mul(fb, rs_gen[k + 1]);
    }
    for (int k = 0; k < 32; k++) parity_out[k] = dual ? tal[p[k]] : p[k];
}

int lrpt_rs_decode(uint8_t cw[255], bool dual) {
    uint8_t c[255], S[32], lam[33] = {1}, B[33] = {1}, T[33], om[33], fixv[32];
    int     L = 0, m = 1, nerr = 0, any = 0, fix[32];
    uint8_t b = 1;

    tables();
    for (int i = 0; i < 255; i++) c[i] = dual ? tal1[cw[i]] : cw[i];
    for (int j = 0; j < 32; j++) {          /* the syndromes, byte 0 the highest power */
        uint8_t r = gf_pow11(112 + j), s = 0;
        for (int i = 0; i < 255; i++) s = gf_mul(s, r) ^ c[i];
        S[j] = s;
        any |= s;
    }
    if (!any) return 0;
    for (int n = 0; n < 32; n++) {          /* Berlekamp-Massey */
        uint8_t dlt = S[n];
        for (int i = 1; i <= L; i++) dlt ^= gf_mul(lam[i], S[n - i]);
        if (!dlt) {
            m++;
            continue;
        }
        uint8_t coef = gf_exp[gf_log[dlt] + 255 - gf_log[b]];
        memcpy(T, lam, sizeof(T));
        for (int i = m; i <= 32; i++) lam[i] ^= gf_mul(coef, B[i - m]);
        if (2 * L <= n) {
            L = n + 1 - L;
            memcpy(B, T, sizeof(B));
            b = dlt;
            m = 1;
        } else m++;
    }
    if (L > 16) return -1;
    /* omega = S lambda mod x^32 */
    memset(om, 0, sizeof(om));
    for (int i = 0; i < 32; i++)
        for (int k = 0; k <= L && k <= i; k++) om[i] ^= gf_mul(S[i - k], lam[k]);
    /* Chien: an error at power p (byte 254 - p) where lambda(beta^-p) = 0; Forney its value */
    for (int p = 0; p < 255; p++) {
        uint8_t xinv = gf_pow11(-p), v = 0, xp = 1;
        for (int k = 0; k <= L; k++) {
            v ^= gf_mul(lam[k], xp);
            xp = gf_mul(xp, xinv);
        }
        if (v) continue;
        uint8_t num = 0, den = 0;
        xp = 1;
        for (int k = 0; k < 32; k++) {
            num ^= gf_mul(om[k], xp);
            xp = gf_mul(xp, xinv);
        }
        xp = 1;                             /* lambda'(x): the odd terms, x^(k-1) */
        for (int k = 1; k <= L; k += 2) {
            den ^= gf_mul(lam[k], xp);
            xp = gf_mul(xp, gf_mul(xinv, xinv));
        }
        if (!den || nerr == 16) return -1;
        /* e = X^(1-112) omega(X^-1) / lambda'(X^-1), X = beta^p */
        uint8_t e = gf_mul(gf_mul(num, gf_exp[(255 - gf_log[den]) % 255]), gf_pow11(p * (1 - 112)));
        c[254 - p] ^= e;
        fix[nerr]   = 254 - p;
        fixv[nerr++] = dual ? tal[e] : e;
    }
    if (nerr != L) return -1;
    for (int i = 0; i < nerr; i++) cw[fix[i]] ^= fixv[i];     /* only once it is sure */
    return nerr;
}

void lrpt_pn(uint8_t *b, size_t n) {
    static uint8_t seq[255];
    static bool    done;

    if (!done) {
        uint8_t a[255 * 8];
        for (int i = 0; i < 8; i++) a[i] = 1;
        for (int i = 8; i < 255 * 8; i++) a[i] = a[i - 1] ^ a[i - 3] ^ a[i - 5] ^ a[i - 8];
        for (int i = 0; i < 255; i++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) v = (uint8_t)(v << 1 | a[8 * i + k]);
            seq[i] = v;
        }
        done = true;
    }
    for (size_t i = 0; i < n; i++) b[i] ^= seq[i % 255];
}

/* ---- The imager's JPEG ---- */

const uint8_t lrpt_zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t std_q[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                  14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                  18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                  49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

void lrpt_jpeg_tables(const uint8_t **dc_bits, const uint8_t **dc_vals, const uint8_t **ac_bits, const uint8_t **ac_vals) {
    *dc_bits = jpeg_dc_bits;
    *dc_vals = jpeg_dc_vals;
    *ac_bits = jpeg_ac_bits;
    *ac_vals = jpeg_ac_vals;
}

void lrpt_quant(int q, int t[64]) {
    float f = q < 1 ? 5000.0f : q < 50 ? 5000.0f / q : 200.0f - 2 * q;

    for (int i = 0; i < 64; i++) {
        int v = (int)lrintf(std_q[i] * f / 100);
        t[i] = v < 1 ? 1 : v;
    }
}

typedef struct {
    const uint8_t *p;
    size_t         n, pos;      /* bits */
} bits_t;

static inline int get_bit(bits_t *b) {
    if (b->pos >= b->n) return -1;
    int v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static int get_bits(bits_t *b, int k) {
    int v = 0;
    for (int i = 0; i < k; i++) {
        int bit = get_bit(b);
        if (bit < 0) return -1;
        v = v << 1 | bit;
    }
    return v;
}

static int huff(bits_t *b, const huff_t *h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int bit = get_bit(b);
        if (bit < 0) return -1;
        code = code << 1 | bit;
        if (h->maxcode[len] >= 0 && code <= h->maxcode[len] && code >= h->mincode[len])
            return h->vals[h->valptr[len] + code - h->mincode[len]];
    }
    return -1;
}

static inline int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

int lrpt_mcus(const uint8_t *data, size_t n, int q, uint8_t px[8][112]) {
    bits_t b = {data, n * 8, 0};
    int    qt[64], dc = 0, done = 0;

    tables();
    lrpt_quant(q, qt);
    for (int m = 0; m < 14; m++) {
        float co[64] = {0}, tmp[64];
        int   s = huff(&b, &huff_dc);
        if (s < 0 || s > 11) break;
        if (s) {
            int v = get_bits(&b, s);
            if (v < 0) break;
            dc += extend(v, s);
        }
        co[0] = (float)(dc * qt[0]);
        int k = 1, bad = 0;
        while (k < 64) {
            int rs = huff(&b, &huff_ac);
            if (rs < 0) {
                bad = 1;
                break;
            }
            int r = rs >> 4, sz = rs & 15;
            if (!sz) {
                if (r == 15) {
                    k += 16;
                    continue;
                }
                break;          /* end of block */
            }
            k += r;
            int v = get_bits(&b, sz);
            if (k > 63 || v < 0) {
                bad = 1;
                break;
            }
            co[lrpt_zigzag[k]] = (float)(extend(v, sz) * qt[lrpt_zigzag[k]]);
            k++;
        }
        if (bad) break;
        /* the IDCT: rows (v) then columns (u); co[v * 8 + u] */
        for (int y = 0; y < 8; y++)
            for (int u = 0; u < 8; u++) {
                float a = 0;
                for (int v = 0; v < 8; v++) a += idct_c[y][v] * co[v * 8 + u];
                tmp[y * 8 + u] = a;
            }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                float a = 128;
                for (int u = 0; u < 8; u++) a += idct_c[x][u] * tmp[y * 8 + u];
                int v = (int)lrintf(a);
                px[y][m * 8 + x] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);   /* 0 is kept for "not received" */
            }
        done++;
    }
    return done;
}

/* ---- The decoder ---- */

static int debug = -1;           /* LRPT_DEBUG in the environment: a line a frame on stderr (2: and a packet) */

typedef struct {
    bool     seen;
    uint32_t count;             /* the VCDU counter last */
    bool     sync;              /* a packet's start known */
    uint8_t  pkt[PKT_MAX];
    int      have;              /* its bytes so far */
} vc_t;

typedef struct {
    uint8_t *px;                /* rows x LRPT_W */
    int      strips, cap;       /* strips so far (8 rows each), and room */
    int      lo, hi;            /* the strips changed since lrpt_changed last asked, lo > hi none */
} pic_t;

struct lrpt_s {
    float    fs;
    int      mode_req, mode;
    demod_t  dm;
    double   now;               /* seconds of input since the start */
    double   mode_t0, sync_t;   /* this mode tried since; the last sign of it right (a frame or a marker) */
    double   good_t;            /* the last frame whose RS passed */
    double   frame_t;           /* the last frame found */
    bool     worked[LRPT_MODES];    /* AUTO: the ways of sending frames have come in */
    uint64_t soft_n;            /* soft bits in (lrpt_feed_soft's clock) */

    /* 80k: the markers and the deinterleaver */
    struct {
        bool     lock;
        int      t;             /* the constellation's turn */
        int8_t   win[IL_SRCH];
        int      wn;
        int      pos;           /* 0-79 since the marker's start */
        unsigned mk;
        int      bad;           /* markers missed in a row */
        int8_t  *ring;
        int64_t  p_in, q_out;   /* data bits in, out */
        int64_t  rx, win_at;    /* bits received; the place of the first in win */
        int64_t  mk_at, mk_p;   /* the last marker seen: its place, the data bits' count then */
        bool     mk_ok;
    } il;
    int8_t   pair[2];           /* a soft bit waiting for its pair */
    int      npair;
    int8_t   dprev[2];          /* the differential decoder's last bit of each rail */
    int      dpar;

    /* the frames */
    int8_t   sb[SB_CAP];
    int      sb_n;
    bool     locked;
    int      next;              /* the next frame's start in sb, locked */
    int      srch;              /* where the search goes on from, not locked */
    int      t;                 /* the constellation's turn of the frames */
    int      miss;
    bool     sure;              /* a frame's RS has passed since this lock: it is the signal's */
    bool     rs_ok;             /* the last frame's RS passed */
    uint64_t pat[8];            /* the coded sync word under each turn, first bit highest */
    int8_t   tmp[2 * (PRE + F_BITS / 2 + POST)];
    uint64_t dec[PRE + F_BITS / 2 + POST];
    uint8_t  cadu[1024];
    bool     dual;              /* RS in CCSDS's dual basis (Meteor's own is the conventional one) */

    vc_t     vc[64];
    pic_t    pic[LRPT_CHANS];
    bool     have_t;
    double   t_last, period;    /* the last strip's time (ms of the day), the strips' period (ms) */
    int      s_last;            /* its strip */

    lrpt_stat_t st;
};

/* The 8 ways a pair of soft bits may have been turned (the constellation by 0, 90, 180, 270 degrees, mirrored or
 * not), and the way back */
static inline void turn(int t, int a, int b, int *x, int *y) {
    switch (t) {
    default: *x = a, *y = b; break;
    case 1: *x = -b, *y = a; break;
    case 2: *x = -a, *y = -b; break;
    case 3: *x = b, *y = -a; break;
    case 4: *x = a, *y = -b; break;
    case 5: *x = b, *y = a; break;
    case 6: *x = -a, *y = b; break;
    case 7: *x = -b, *y = -a; break;
    }
}
static const int turn_back[8] = {0, 3, 2, 1, 4, 5, 6, 7};

/* The turns a signal can come with: QPSK all 8 (its symbols turned by 90 degrees, mirrored); offset QPSK only the
 * even ones, its rails' signs: turned by 90 degrees it is one bit later and signs (the demodulator's I half a symbol
 * from where it was), never its bits swapped in their pairs. A swap allowed, an 80k stream locked on its markers so
 * (they matched, 127 of 128) and its every bit then went through the wrong branch of the deinterleaver */
static inline int turn_step(const lrpt_t *l) { return modes[l->mode].oq ? 2 : 1; }

#define ASM       0x1ACFFC1Du
#define SYNC_MASK ((1ULL << 52) - 1)    /* the coded sync word's first 12 bits come of the frame before */

static void make_patterns(lrpt_t *l) {
    uint8_t bits[32], coded[64];

    for (int i = 0; i < 32; i++) bits[i] = (ASM >> (31 - i)) & 1;
    lrpt_conv_encode(bits, 32, coded, 0);
    for (int t = 0; t < 8; t++) {
        uint64_t w = 0;
        for (int i = 0; i < 32; i++) {
            int x, y;
            turn(t, coded[2 * i] ? -1 : 1, coded[2 * i + 1] ? -1 : 1, &x, &y);
            w = w << 2 | (uint64_t)(x < 0) << 1 | (uint64_t)(y < 0);
        }
        l->pat[t] = w;
    }
}

static void pipe_reset(lrpt_t *l) {
    l->il.lock = false;
    l->il.wn   = 0;
    l->il.bad  = 0;
    l->il.p_in  = 0;
    l->il.q_out = -IL_BACK;
    l->il.rx    = 0;
    l->il.mk_ok = false;
    if (l->il.ring) memset(l->il.ring, 0, IL_RING);
    l->npair  = 0;
    l->dprev[0] = l->dprev[1] = 0;
    l->dpar   = 0;
    l->sb_n   = 0;
    l->locked = false;
    l->srch   = 0;
    l->miss   = 0;
}

static bool set_mode(lrpt_t *l, int mode) {
    l->mode    = mode;
    l->st.mode = mode;
    l->mode_t0 = l->now;
    if (modes[mode].il && !l->il.ring && !(l->il.ring = calloc(IL_RING, 1))) return false;
    pipe_reset(l);
    if (l->fs > 0 && !demod_init(&l->dm, l->fs, modes[mode].rate, modes[mode].oq)) return false;
    return true;
}

lrpt_t *lrpt_new(float rate, int mode) {
    lrpt_t *l = calloc(1, sizeof(*l));

    if (!l) return NULL;
    tables();
    if (debug < 0) debug = getenv("LRPT_DEBUG") ? atoi(getenv("LRPT_DEBUG")) : 0;
    l->fs       = rate;
    l->mode_req = mode >= 0 && mode < LRPT_MODES ? mode : LRPT_AUTO;
    l->st.scid  = -1;
    l->period   = 1232;
    make_patterns(l);
    if (!set_mode(l, l->mode_req == LRPT_AUTO ? LRPT_72K_OQPSK : l->mode_req)) {
        lrpt_free(l);
        return NULL;
    }
    return l;
}

void lrpt_clear(lrpt_t *l) {
    for (int c = 0; c < LRPT_CHANS; c++) {
        free(l->pic[c].px);
        memset(&l->pic[c], 0, sizeof(l->pic[c]));
        l->st.rows[c] = 0;
    }
    l->have_t = false;
    l->period = 1232;
}

void lrpt_free(lrpt_t *l) {
    if (!l) return;
    lrpt_clear(l);
    demod_free(&l->dm);
    free(l->il.ring);
    free(l);
}

void lrpt_stat(const lrpt_t *l, lrpt_stat_t *s) {
    *s        = l->st;
    s->locked = l->now - l->sync_t < 2 && l->sync_t > 0;
    s->freq   = l->dm.fs > 0 ? l->dm.fr_avg * l->dm.fs / (2 * (float)M_PI) : 0;
    s->quality = l->dm.lk_den > 0 ? l->dm.lk_num / l->dm.lk_den : 0;
    if (s->quality < 0) s->quality = 0;
    if (s->quality > 1) s->quality = 1;
}

bool lrpt_changed(lrpt_t *l, int ch, int *from, int *to) {
    if (ch < 0 || ch >= LRPT_CHANS || !l->pic[ch].px || l->pic[ch].lo > l->pic[ch].hi) return false;
    *from = l->pic[ch].lo;
    *to   = l->pic[ch].hi;
    l->pic[ch].lo = 1;
    l->pic[ch].hi = 0;
    return true;
}

const uint8_t *lrpt_image(const lrpt_t *l, int ch, int *h) {
    if (ch < 0 || ch >= LRPT_CHANS || !l->pic[ch].px) {
        *h = 0;
        return NULL;
    }
    *h = l->pic[ch].strips * 8;
    return l->pic[ch].px;
}

/* An imager packet's MCUs into its channel's picture, at its strip. Every packet of a strip (14 of each channel sent,
 * and APID 70's) has the strip's time in it, the same to the microsecond; strips 1.232 s apart (Meteor-M2-2, 2019).
 * The strip from the time against the last strip's: a lost stretch leaves its strips empty, whichever channels are
 * sent and wherever reception starts. The period learned as the strips come one after another */
static void imager(lrpt_t *l, const uint8_t *p, int len) {
    int    apid = ((p[0] & 7) << 8) | p[1], seq = ((p[2] & 0x3f) << 8) | p[3];
    int    ch = apid - 64, mcu = p[14], q = p[19];
    double t  = ((uint32_t)p[8] << 24 | (uint32_t)p[9] << 16 | (uint32_t)p[10] << 8 | p[11]) + (p[12] << 8 | p[13]) / 1000.0;

    if (debug > 1)
        fprintf(stderr, "lrpt: packet apid %d seq %5d mcu %3d q %3d len %4d time %02x%02x %02x%02x%02x%02x %02x%02x\n",
                apid, seq, mcu, q, len, p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13]);
    if (len < 21 || mcu % 14 || mcu > 182 || q < 1 || q > 100) return;
    if (!l->have_t) {
        l->have_t = true;
        l->t_last = t;
        l->s_last = 0;
    } else if (t != l->t_last) {
        double dt = t - l->t_last;
        if (dt < -43200000) dt += 86400000;     /* midnight */
        if (dt > 43200000) dt -= 86400000;
        long k = lround(dt / l->period);
        if (k < 0 || k > 2000) return;          /* a time out of the way: this packet's alone wrong */
        if (k == 1) l->period += 0.05 * (dt - l->period);
        if (k) {
            l->s_last += (int)k;
            l->t_last = t;
        }
    }
    int strip = l->s_last;
    if (strip >= MAX_STRIPS) return;

    pic_t *pc = &l->pic[ch];
    if (!pc->px) pc->lo = 1, pc->hi = 0;
    if (strip >= pc->cap) {
        int      cap = (strip / STRIP_CAP + 1) * STRIP_CAP;
        uint8_t *np  = realloc(pc->px, (size_t)cap * 8 * LRPT_W);
        if (!np) return;
        memset(np + (size_t)pc->cap * 8 * LRPT_W, 0, (size_t)(cap - pc->cap) * 8 * LRPT_W);
        pc->px  = np;
        pc->cap = cap;
    }
    uint8_t px[8][112];
    int     got = lrpt_mcus(p + 20, (size_t)(len - 20), q, px);
    for (int y = 0; y < 8; y++) memcpy(pc->px + ((size_t)strip * 8 + y) * LRPT_W + mcu * 8, px[y], (size_t)got * 8);
    if (strip >= pc->strips) pc->strips = strip + 1;
    if (pc->lo > pc->hi) pc->lo = pc->hi = strip;
    else if (strip < pc->lo) pc->lo = strip;
    else if (strip > pc->hi) pc->hi = strip;
    l->st.rows[ch] = pc->strips * 8;
    l->st.packets++;
}

static void packet(lrpt_t *l, const uint8_t *p, int len) {
    int apid = ((p[0] & 7) << 8) | p[1];

    if (apid >= 64 && apid <= 69 && (p[0] & 0x08)) imager(l, p, len);
}

/* Bytes of a VCID's packet zone into its packets */
static void pkt_bytes(lrpt_t *l, vc_t *v, const uint8_t *b, int n) {
    while (n > 0) {
        int need = v->have < 6 ? 6 - v->have : 6 + ((v->pkt[4] << 8) | v->pkt[5]) + 1 - v->have;
        int take = need < n ? need : n;
        if (v->have + take > PKT_MAX) {
            v->have = 0;
            v->sync = false;
            return;
        }
        memcpy(v->pkt + v->have, b, (size_t)take);
        v->have += take;
        b += take;
        n -= take;
        if (v->have >= 6 && v->have == 6 + ((v->pkt[4] << 8) | v->pkt[5]) + 1) {
            packet(l, v->pkt, v->have);
            v->have = 0;
        }
    }
}

static void vcdu(lrpt_t *l, const uint8_t *f) {
    int      scid = ((f[0] & 0x3f) << 2) | (f[1] >> 6), vcid = f[1] & 0x3f;
    uint32_t cnt  = (uint32_t)f[2] << 16 | (uint32_t)f[3] << 8 | f[4];
    int      fhp  = ((f[8] & 7) << 8) | f[9];
    const uint8_t *zone = f + 10;
    vc_t          *v    = &l->vc[vcid];

    l->st.scid = scid;
    if (vcid == 63) return;                 /* fill */
    if (v->seen && cnt != ((v->count + 1) & 0xffffff)) {     /* one lost: its packet with it */
        v->sync = false;
        v->have = 0;
    }
    v->seen  = true;
    v->count = cnt;
    if (fhp == 0x7ff) {                     /* no packet starts in it */
        if (v->sync) pkt_bytes(l, v, zone, 882);
        return;
    }
    if (fhp >= 882) {
        v->sync = false;
        v->have = 0;
        return;
    }
    if (v->sync && v->have) {
        pkt_bytes(l, v, zone, fhp);
        if (v->have) v->have = 0;           /* it should have ended where the next one starts */
    }
    v->sync = true;
    v->have = 0;
    pkt_bytes(l, v, zone + fhp, 882 - fhp);
}

/* A frame decoded: its sync word checked, unscrambled, corrected, its VCDU read. A frame (true) when its sync word
 * decoded right or its RS passed: errors in the sync word's own bits (the 80k mode's first frames have erasures
 * spread through them) leave the rest of it as good */
static bool cadu(lrpt_t *l) {
    uint8_t *c = l->cadu, cw[255];
    uint32_t s = (uint32_t)c[0] << 24 | (uint32_t)c[1] << 16 | (uint32_t)c[2] << 8 | c[3];
    int      fixed = 0;
    bool     ok    = true;

    bool     sync  = __builtin_popcount(s ^ ASM) <= 4;

    l->rs_ok = false;
    {                                       /* LRPT_DUMP=file: each frame found as decoded (scrambled), for a look */
        static FILE *df;
        static int   tried;
        if (!tried++ && getenv("LRPT_DUMP")) df = fopen(getenv("LRPT_DUMP"), "wb");
        if (df) fwrite(c, 1, 1024, df);
    }
    lrpt_pn(c + 4, 1020);
    for (int k = 0; k < 4 && ok; k++) {
        for (int j = 0; j < 255; j++) cw[j] = c[4 + 4 * j + k];
        int r = lrpt_rs_decode(cw, l->dual);
        if (r < 0 && (r = lrpt_rs_decode(cw, !l->dual)) >= 0) l->dual = !l->dual;     /* the other basis */
        if (r < 0) ok = false;
        else {
            fixed += r;
            for (int j = 0; j < 255; j++) c[4 + 4 * j + k] = cw[j];
        }
    }
    l->rs_ok = ok;
    if (!sync && !ok) return false;
    l->st.frames++;
    l->sync_t = l->frame_t = l->now;
    l->worked[l->mode] = true;
    if (ok) {
        l->good_t = l->now;
        l->st.frames_ok++;
        l->st.rs_fixed += (uint32_t)fixed;
        vcdu(l, c + 4);
    }
    return true;
}


/* The frame starting at sb[o], turned t: decoded into l->cadu; its sync word found in it or not */
static bool frame_at(lrpt_t *l, int o, int t) {
    int pre = o / 2 < PRE ? o / 2 : PRE, np = pre + F_BITS / 2 + POST, b = turn_back[t];
    const int8_t *s = l->sb + o - 2 * pre;

    for (int k = 0; k < np; k++) {
        int x, y;
        turn(b, s[2 * k], s[2 * k + 1], &x, &y);
        l->tmp[2 * k]     = (int8_t)x;
        l->tmp[2 * k + 1] = (int8_t)y;
    }
    viterbi(l->tmp, np, l->dec, l->cadu, pre, 8192);
    uint32_t ok0 = l->st.frames_ok;
    bool     r   = cadu(l);
    if (debug < 0) debug = getenv("LRPT_DEBUG") ? atoi(getenv("LRPT_DEBUG")) : 0;
    if (debug)
        fprintf(stderr, "lrpt: %8.3f s %s at %6d turn %d sync %s RS %s, %+7.1f Hz q %.2f\n", l->now,
                l->locked ? "frame" : "found", o, t, r ? "yes" : "NO ", l->st.frames_ok > ok0 ? "ok " : "BAD",
                l->dm.fr_avg * l->dm.fs / (2 * (float)M_PI), l->dm.lk_den > 0 ? l->dm.lk_num / l->dm.lk_den : 0);
    return r;
}

/* The 64 hard bits from sb[o], the first highest */
static uint64_t sync_word(const lrpt_t *l, int o) {
    uint64_t w = 0;
    for (int i = 0; i < 64; i++) w = w << 1 | (uint64_t)(l->sb[o + i] < 0);
    return w;
}

/* The best place and turn for a sync word starting within [from, to): its wrong bits */
static int sync_search(const lrpt_t *l, int from, int to, int *at, int *tt) {
    uint64_t w = 0;
    int      best = 99;

    for (int i = from; i < to + 63; i++) {
        w = w << 1 | (uint64_t)(l->sb[i] < 0);
        int o = i - 63;
        if (o < from) continue;
        for (int t = 0; t < 8; t += turn_step(l)) {
            int e = __builtin_popcountll((w ^ l->pat[t]) & SYNC_MASK);
            if (e < best) best = e, *at = o, *tt = t;
        }
    }
    return best;
}

static void frames(lrpt_t *l) {
    for (;;) {
        if (!l->locked) {
            if (l->sb_n < l->srch + 2 * F_BITS + 2 * POST + 64) break;
            int o = 0, t = 0, e = sync_search(l, l->srch, l->srch + F_BITS, &o, &t);
            if (debug > 0 && e > SYNC_SRCH)
                fprintf(stderr, "lrpt: %8.3f s no sync word (best %d bits wrong of 52)\n", l->now, e);
            if (e <= SYNC_SRCH && frame_at(l, o, t)) {
                l->locked = true;
                l->sure   = l->rs_ok;
                l->t      = t;
                l->miss   = 0;
                l->next   = o + F_BITS;
            } else l->srch += F_BITS;
        } else {
            if (l->sb_n < l->next + 4 + F_BITS + 2 * POST + 64) break;
            /* where the last frame says, as it was turned, unless a place a symbol either side (a slip) or another
             * turn (the carrier's loop slipped) is clearly better */
            int o = 0, t = 0, e = sync_search(l, l->next - 2, l->next + 3, &o, &t);
            int here = __builtin_popcountll((sync_word(l, l->next) ^ l->pat[l->t]) & SYNC_MASK);
            if (e > SYNC_LOCK || e + 6 > here) o = l->next, t = l->t;
            if (frame_at(l, o, t)) {
                l->miss = 0;
                l->t    = t;
                l->sure |= l->rs_ok;
            } else if (++l->miss >= (l->sure ? MISSES : 2)) {
                l->locked = false;
                l->srch   = o + F_BITS - 64;
            }
            l->next = o + F_BITS;
        }
        /* what is behind kept only as far as the Viterbi's run-in before the next frame */
        int keep = (l->locked ? l->next : l->srch) - 2 * PRE - 8;
        if (keep > F_BITS) {
            memmove(l->sb, l->sb + keep, (size_t)(l->sb_n - keep));
            l->sb_n -= keep;
            l->next -= keep;
            l->srch -= keep;
        }
    }
}

/* A soft bit after the deinterleaver: the differential decoding, into the frames' buffer */
static void bit_out(lrpt_t *l, int8_t s) {
    if (modes[l->mode].diff) {
        int8_t p = l->dprev[l->dpar];
        l->dprev[l->dpar] = s;
        l->dpar ^= 1;
        int a = s < 0 ? -s : s, b = p < 0 ? -p : p, m = a < b ? a : b;
        s = (int8_t)(((s < 0) != (p < 0)) ? -m : m);
    }
    if (l->sb_n == SB_CAP) frames(l);
    if (l->sb_n < SB_CAP) l->sb[l->sb_n++] = s;
}

/* 80k: a data bit into the deinterleaver; out what is complete. Its output starts IL_BACK before the first bit in:
 * the bits sent then on the longer branches still come, so the frames before the markers locked are not all lost
 * (4 of the 36 branches missing at the earliest: erasures the Viterbi and RS take) */
static void il_data(lrpt_t *l, int8_t s) {
    int64_t p = l->il.p_in, q = p - (p % IL_N) * IL_M * IL_N;

    if (q >= -IL_BACK) l->il.ring[(q + IL_BACK) % IL_RING] = s;
    l->il.p_in++;
    while (l->il.q_out + IL_DELAY < l->il.p_in) {
        int8_t *c = &l->il.ring[(l->il.q_out + IL_BACK) % IL_RING];
        bit_out(l, *c);
        *c = 0;
        l->il.q_out++;
    }
}

/* 80k, locked: a bit (turned) after the marker's start; at: its place in all the bits received. Each marker seen
 * kept with the data bits' count then: after a slip (the carrier's loop by 90 degrees is a bit's slip in offset
 * QPSK) the count is set from the bits received since, so the deinterleaver's branches stay as the satellite's */
static void il_bit(lrpt_t *l, int8_t s, int64_t at) {
    if (l->il.pos < 8) {
        l->il.mk = (l->il.mk << 1) | (s < 0);
        if (l->il.pos == 7) {
            if (__builtin_popcount((l->il.mk ^ IL_MARK) & 0xff) <= 1) {
                l->il.bad    = 0;
                l->il.mk_at  = at - 7;
                l->il.mk_p   = l->il.p_in;
                l->il.mk_ok  = true;
                l->sync_t = l->now;
            } else if (++l->il.bad >= 16) {
                l->il.lock = false;
                if (debug > 0) fprintf(stderr, "lrpt: %8.3f s 80k markers lost\n", l->now);
            }
        }
    } else il_data(l, s);
    if (++l->il.pos == IL_STRIDE) l->il.pos = 0;
}

/* 80k, searching: the marker's place and turn in the bits gathered */
static void il_search(lrpt_t *l) {
    int best = 0, bo = 0, bt = 0, n = (l->il.wn < IL_SRCH ? l->il.wn : IL_SRCH) & ~1;

    for (int t = 0; t < 8; t += turn_step(l)) {
        static uint8_t hb[IL_SRCH];
        for (int k = 0; k + 1 < n; k += 2) {
            int x, y;
            turn(t, l->il.win[k], l->il.win[k + 1], &x, &y);
            hb[k] = x < 0, hb[k + 1] = y < 0;
        }
        for (int o = 0; o < IL_STRIDE; o++) {
            int c = 0;
            for (int k = o; k + 8 <= n; k += IL_STRIDE) {
                unsigned v = 0;
                for (int j = 0; j < 8; j++) v = v << 1 | hb[k + j];
                c += __builtin_popcount(v ^ IL_MARK) <= 1;
            }
            if (c > best) best = c, bo = o, bt = t;
        }
    }
    if (best < n / IL_STRIDE / 3) {         /* not there: these go */
        l->il.wn = 0;
        return;
    }
    if (debug > 0)
        fprintf(stderr, "lrpt: %8.3f s 80k markers at %d turn %d (%d of %d)%s\n", l->now, bo, bt, best,
                n / IL_STRIDE, l->il.mk_ok ? ", again" : "");
    l->il.lock = true;
    l->il.t    = bt;
    l->il.bad  = 0;
    l->il.pos  = 0;
    l->sync_t  = l->now;
    /* the data bits' count at this marker: from the last marker seen and the whole strides since, else the next
     * multiple of 72; nothing (zeros) for what was not received, or back to it when more was counted than came */
    int64_t at = l->il.win_at + bo, want = (l->il.p_in + IL_STRIDE - 9) / (IL_STRIDE - 8) * (IL_STRIDE - 8);
    if (l->il.mk_ok) want = l->il.mk_p + (IL_STRIDE - 8) * llround((double)(at - l->il.mk_at) / IL_STRIDE);
    while (l->il.p_in < want) il_data(l, 0);
    if (l->il.p_in > want) l->il.p_in = want;
    int k0 = bo & ~1, k = k0;
    for (; k + 1 < n && l->il.lock; k += 2) {
        int x, y;
        turn(bt, l->il.win[k], l->il.win[k + 1], &x, &y);
        if (k >= bo) il_bit(l, (int8_t)x, l->il.win_at + k);
        if (k + 1 >= bo) il_bit(l, (int8_t)y, l->il.win_at + k + 1);
    }
    /* lost again within them: the rest kept for the next search */
    memmove(l->il.win, l->il.win + k, (size_t)(l->il.wn - k));
    l->il.wn -= k;
    l->il.win_at += k;
}

/* A pair of soft bits from the demodulator (I, Q) */
static void pair_in(lrpt_t *l, int8_t a, int8_t b) {
    if (!modes[l->mode].il) {
        bit_out(l, a);
        bit_out(l, b);
        return;
    }
    int64_t at = l->il.rx;

    l->il.rx += 2;
    if (l->il.lock) {
        int x, y;
        turn(l->il.t, a, b, &x, &y);
        il_bit(l, (int8_t)x, at);
        il_bit(l, (int8_t)y, at + 1);
        return;
    }
    if (!l->il.wn) l->il.win_at = at;
    l->il.win[l->il.wn++] = a;
    l->il.win[l->il.wn++] = b;
    if (l->il.wn >= IL_SRCH) il_search(l);
}

/* AUTO: the next way of sending tried once this one has shown nothing for a while */
static void auto_mode(lrpt_t *l) {
    double since = l->now - (l->sync_t > l->mode_t0 ? l->sync_t : l->mode_t0);
    double dwell = l->worked[l->mode] ? 20 : modes[l->mode].il ? 5 : 7;    /* longer on one that has worked */

    if (l->mode_req != LRPT_AUTO || since < dwell) return;
    set_mode(l, l->mode % (LRPT_MODES - 1) + 1);
}

void lrpt_feed_soft(lrpt_t *l, const int8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        l->pair[l->npair++] = s[i];
        if (l->npair == 2) {
            pair_in(l, l->pair[0], l->pair[1]);
            l->npair = 0;
        }
        if (++l->soft_n % 4096 == 0) l->now = l->soft_n / (2.0 * modes[l->mode].rate);
    }
    frames(l);
}

void lrpt_feed(lrpt_t *l, const float *iq, size_t n) {
    int8_t out[8];

    for (size_t i = 0; i < n; i++) {
        int k = demod_sample(&l->dm, iq[2 * i], iq[2 * i + 1], out);
        for (int j = 0; j + 1 < k; j += 2) pair_in(l, out[j], out[j + 1]);
    }
    l->now += n / l->fs;
    frames(l);
    /* the carrier looked for again while its loop has not locked and no frame passes RS; left to its loop then */
    bool sure = (l->now - l->good_t < 1 && l->good_t > 0) || (l->dm.lk_den > 0 && l->dm.lk_num > LOCKED * l->dm.lk_den);
    sure |= l->now - l->frame_t < 1 && l->frame_t > 0;
    if (sure) {
        l->dm.acquire = false;
        l->dm.fn = l->dm.fblocks = 0;
        l->dm.cn = l->dm.cblocks = 0;
    } else if (!l->dm.acquire) {
        l->dm.acquire = true;
        memset(l->dm.fpow, 0, FFT_N * sizeof(float));
        memset(l->dm.cpow, 0, CEN_N * sizeof(float));
    }
    auto_mode(l);
}
