/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df's channel filter (df.h; docs/plans/kraken_df_plan.md 2.1): every channel's block mixed by
 *  the same oscillator so the target sits at 0 Hz, then two Blackman-windowed FIR stages decimate it, the first to
 *  about four times the output rate (its cut at its own Nyquist: what folds lands outside the band), the second to
 *  about 32 kS/s (or 2.56 x the bandwidth). The same filters on every channel.
 *
 *  For the R16's Cortex-A7 (GCC will not vectorise float loops for 32-bit NEON on its own): 512-sample chunks so
 *  everything stays in cache, bytes to floats and the mix four samples at a time, and the FIRs four channels at a
 *  time sharing each load of the taps (eight accumulators, enough to hide the multiply-accumulate's latency). Taps
 *  padded to fours. Each channel's mean is followed from chunk to chunk (the previous chunk's, for speed). Without
 *  NEON (x86) plain loops do the same.
 */

#include "df.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(DF_NO_NEON)  /* -DDF_NO_NEON: test the plain loops */
#include <arm_neon.h>
#define DF_NEON 1
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK 512

#ifdef DF_PROFILE                       /* df_bench -DDF_PROFILE: CPU seconds in the oscillator, the mix, each stage */
#include <time.h>
double df_prof[4];
static double now_(void) {
    struct timespec t;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
#define PROF(k, x) do { double t0_ = now_(); x; df_prof[k] += now_() - t0_; } while (0)
#else
#define PROF(k, x) x
#endif

/* windowed-sinc low-pass, cut fc (cycles per sample), t taps (odd), padded with zeros to tp (a multiple of 4) and
 * stored reversed so y = sum h[j] x[newest - tp + 1 + j] */
static float *lowpass(double fc, int t, int tp) {
    float  *h = calloc((size_t)tp, sizeof(float));
    double *g = malloc(sizeof(double) * (size_t)t), sum = 0;
    int     mid = (t - 1) / 2;

    if (!h || !g) {
        free(h);
        free(g);
        return NULL;
    }
    for (int i = 0; i < t; i++) {
        double x = i - mid, s = x == 0 ? 2 * fc : sin(2 * M_PI * fc * x) / (M_PI * x),
               w = t > 1 ? 0.42 - 0.5 * cos(2 * M_PI * i / (t - 1)) + 0.08 * cos(4 * M_PI * i / (t - 1)) : 1;

        g[i] = s * w;
        sum += g[i];
    }
    for (int i = 0; i < t; i++) h[tp - 1 - i] = (float)(g[i] / sum);
    free(g);
    return h;
}

static int taps_for(double rate, double transition) {
    int t = transition > 0 ? (int)ceil(5.5 * rate / transition) : 255;

    if (t > 255) t = 255;
    if (t < 3) t = 3;
    return t | 1;
}

void df_chan_free(df_chan_t *c) {
    free(c->h1);
    free(c->h2);
    free(c->nco);
    for (int m = 0; m < DF_MAXCH; m++)
        for (int k = 0; k < 2; k++) {
            free(c->x1[m][k]);
            free(c->x2[m][k]);
        }
    memset(c, 0, sizeof(*c));
}

int df_chan_init(df_chan_t *c, int nch, double fs, double f, double bw, int cap) {
    memset(c, 0, sizeof(*c));
    if (nch < 1 || nch > DF_MAXCH || fs <= 0 || bw <= 0 || cap < 1) return -1;
    double out_min = bw * 2.56 > 32000 ? bw * 2.56 : 32000;
    int    d = (int)floor(fs / out_min);

    if (d < 1) d = 1;
    c->d2 = d >= 16 ? 4 : d >= 4 ? 2 : 1;
    c->d1 = d / c->d2;
    if (c->d1 < 1) c->d1 = 1;
    c->nch = nch;
    c->fs = fs;
    c->f = f;
    c->bw = bw;
    c->cap = cap;
    double fs1 = fs / c->d1;

    c->fs_out = fs1 / c->d2;
    int t1 = c->d1 > 1 ? taps_for(fs, fs1 - bw) : 1, t2 = c->d2 > 1 ? taps_for(fs1, c->fs_out - bw) : 1;

    c->t1 = (t1 + 3) & ~3;
    c->t2 = (t2 + 3) & ~3;
    c->h1 = lowpass(0.5 / c->d1, t1, c->t1);
    c->h2 = lowpass(0.5 / c->d2, t2, c->t2);
    c->nco = malloc(sizeof(float) * 2 * CHUNK);
    if (!c->h1 || !c->h2 || !c->nco) goto bad;
    for (int m = 0; m < nch; m++) {
        for (int k = 0; k < 2; k++) {
            c->x1[m][k] = calloc((size_t)(c->t1 + c->d1 + CHUNK + 4), sizeof(float));
            c->x2[m][k] = calloc((size_t)(c->t2 + c->d2 + CHUNK / c->d1 + 8), sizeof(float));
            if (!c->x1[m][k] || !c->x2[m][k]) goto bad;
        }
        c->dc[m][0] = c->dc[m][1] = 127.5;
    }
    c->n1 = c->next1 = c->t1 - 1;               /* zero history; the next output at the first new sample */
    c->n2 = c->next2 = c->t2 - 1;
    return 0;
bad:
    df_chan_free(c);
    return -1;
}

void df_chan_retune(df_chan_t *c, double f) {
    c->f = f;
}

/* e^{-j 2 pi f t} for len samples from the exact phase, then the phase moved on */
static void nco(df_chan_t *c, int len) {
    double step = -c->f / c->fs, a0 = 2 * M_PI * c->ph, da = 2 * M_PI * step;
    double cr = cos(a0), ci = sin(a0), sr = cos(da), si = sin(da);

    for (int i = 0; i < len; i++) {
        c->nco[2 * i] = (float)cr;
        c->nco[2 * i + 1] = (float)ci;
        double t = cr * sr - ci * si;

        ci = cr * si + ci * sr;
        cr = t;
    }
    c->ph = fmod(c->ph + step * len, 1.0);
}

/* bytes less the channel's mean, times the oscillator, onto stage 1; the chunk's own mean kept for the next */
static void mix(df_chan_t *c, int m, const uint8_t *b, int len) {
    float       *xi = c->x1[m][0] + c->n1, *xq = c->x1[m][1] + c->n1;
    const float *o = c->nco;
    const float  k = 1 / 127.5f, mi = (float)(c->dc[m][0] * k), mq = (float)(c->dc[m][1] * k);
    unsigned long si = 0, sq = 0;
    int          i = 0;
#ifdef DF_NEON
    uint32x4_t   ai = vdupq_n_u32(0), aq = vdupq_n_u32(0);
    float32x4_t  vmi = vdupq_n_f32(mi), vmq = vdupq_n_f32(mq), vk = vdupq_n_f32(k);   /* kept in NEON registers: a
                                                    core-to-NEON move each time round stalls the A7 */

    for (; i + 8 <= len; i += 8) {
        uint8x8x2_t v = vld2_u8(b + 2 * i);
        uint16x8_t  wi = vmovl_u8(v.val[0]), wq = vmovl_u8(v.val[1]);

        ai = vpadalq_u16(ai, wi);
        aq = vpadalq_u16(aq, wq);
        for (int h = 0; h < 2; h++) {
            uint16x4_t  hi = h ? vget_high_u16(wi) : vget_low_u16(wi), hq = h ? vget_high_u16(wq) : vget_low_u16(wq);
            float32x4_t x = vsubq_f32(vmulq_f32(vcvtq_f32_u32(vmovl_u16(hi)), vk), vmi);
            float32x4_t y = vsubq_f32(vmulq_f32(vcvtq_f32_u32(vmovl_u16(hq)), vk), vmq);
            float32x4x2_t cs = vld2q_f32(o + 2 * (i + 4 * h));       /* cos, sin */

            vst1q_f32(xi + i + 4 * h, vmlsq_f32(vmulq_f32(x, cs.val[0]), y, cs.val[1]));
            vst1q_f32(xq + i + 4 * h, vmlaq_f32(vmulq_f32(x, cs.val[1]), y, cs.val[0]));
        }
    }
    uint64x2_t s2 = vpaddlq_u32(ai), q2 = vpaddlq_u32(aq);

    si = (unsigned long)(vgetq_lane_u64(s2, 0) + vgetq_lane_u64(s2, 1));
    sq = (unsigned long)(vgetq_lane_u64(q2, 0) + vgetq_lane_u64(q2, 1));
#endif
    for (; i < len; i++) {
        float x = b[2 * i] * k - mi, y = b[2 * i + 1] * k - mq;

        si += b[2 * i], sq += b[2 * i + 1];
        xi[i] = x * o[2 * i] - y * o[2 * i + 1];
        xq[i] = x * o[2 * i + 1] + y * o[2 * i];
    }
    c->dc[m][0] = (double)si / len;
    c->dc[m][1] = (double)sq / len;
}

/* one channel's I and Q dot products with the taps */
static inline void dot2(const float *h, const float *a, const float *b, int t, float *ra, float *rb) {
#ifdef DF_NEON
    float32x4_t sa = vdupq_n_f32(0), sb = vdupq_n_f32(0), ta = vdupq_n_f32(0), tb = vdupq_n_f32(0);
    int         j = 0;

    for (; j + 8 <= t; j += 8) {
        float32x4_t h0 = vld1q_f32(h + j), h1 = vld1q_f32(h + j + 4);

        sa = vmlaq_f32(sa, h0, vld1q_f32(a + j));
        sb = vmlaq_f32(sb, h0, vld1q_f32(b + j));
        ta = vmlaq_f32(ta, h1, vld1q_f32(a + j + 4));
        tb = vmlaq_f32(tb, h1, vld1q_f32(b + j + 4));
    }
    for (; j < t; j += 4) {
        float32x4_t h0 = vld1q_f32(h + j);

        sa = vmlaq_f32(sa, h0, vld1q_f32(a + j));
        sb = vmlaq_f32(sb, h0, vld1q_f32(b + j));
    }
    sa = vaddq_f32(sa, ta);
    sb = vaddq_f32(sb, tb);
    float32x2_t pa = vadd_f32(vget_low_f32(sa), vget_high_f32(sa)), pb = vadd_f32(vget_low_f32(sb), vget_high_f32(sb));
    float32x2_t ab = vpadd_f32(pa, pb);

    *ra = vget_lane_f32(ab, 0);
    *rb = vget_lane_f32(ab, 1);
#else
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;

    for (int j = 0; j < t; j += 4) {
        a0 += h[j] * a[j], a1 += h[j + 1] * a[j + 1], a2 += h[j + 2] * a[j + 2], a3 += h[j + 3] * a[j + 3];
        b0 += h[j] * b[j], b1 += h[j + 1] * b[j + 1], b2 += h[j + 2] * b[j + 2], b3 += h[j + 3] * b[j + 3];
    }
    *ra = (a0 + a1) + (a2 + a3);
    *rb = (b0 + b1) + (b2 + b3);
#endif
}

#ifdef DF_NEON
static inline float hsum(float32x4_t v) {
    float32x2_t p = vadd_f32(vget_low_f32(v), vget_high_f32(v));

    return vget_lane_f32(vpadd_f32(p, p), 0);
}

/* four channels at once, from offset `at` in each */
static inline void dot8(const float *h, float *x[][2], int m, int at, int t, float *ri, float *rq) {
    const float *i0 = x[m][0] + at, *i1 = x[m + 1][0] + at, *i2 = x[m + 2][0] + at, *i3 = x[m + 3][0] + at;
    const float *q0 = x[m][1] + at, *q1 = x[m + 1][1] + at, *q2 = x[m + 2][1] + at, *q3 = x[m + 3][1] + at;
    float32x4_t  a0 = vdupq_n_f32(0), a1 = a0, a2 = a0, a3 = a0, b0 = a0, b1 = a0, b2 = a0, b3 = a0;

    for (const float *he = h + t; h < he; h += 4) {     /* pointers moved on, not indexed: post-increment loads */
        float32x4_t hv = vld1q_f32(h);

        a0 = vmlaq_f32(a0, hv, vld1q_f32(i0)), i0 += 4;
        b0 = vmlaq_f32(b0, hv, vld1q_f32(q0)), q0 += 4;
        a1 = vmlaq_f32(a1, hv, vld1q_f32(i1)), i1 += 4;
        b1 = vmlaq_f32(b1, hv, vld1q_f32(q1)), q1 += 4;
        a2 = vmlaq_f32(a2, hv, vld1q_f32(i2)), i2 += 4;
        b2 = vmlaq_f32(b2, hv, vld1q_f32(q2)), q2 += 4;
        a3 = vmlaq_f32(a3, hv, vld1q_f32(i3)), i3 += 4;
        b3 = vmlaq_f32(b3, hv, vld1q_f32(q3)), q3 += 4;
    }
    ri[0] = hsum(a0), ri[1] = hsum(a1), ri[2] = hsum(a2), ri[3] = hsum(a3);
    rq[0] = hsum(b0), rq[1] = hsum(b1), rq[2] = hsum(b2), rq[3] = hsum(b3);
}
#endif

/* one decimating stage over every channel: x[m][0..1] hold `have` samples, the next output's newest sample at
 * `next`. Outputs to the next stage's buffers at to_at, or as complex to oc at oc_at. Returns how many per channel
 * and moves the held samples down */
static int stage(df_chan_t *c, float *x[][2], int *have, int *next, int t, int d, const float *h, float *to[][2],
                 int to_at, float complex *const oc[], int oc_at) {
    int k = 0, nx = *next;

    for (int p = nx; p < *have; p += d, k++) {
        int at = p - t + 1, m = 0;

#ifdef DF_NEON
        for (; m + 4 <= c->nch; m += 4) {
            float ri[4], rq[4];

            dot8(h, x, m, at, t, ri, rq);
            for (int r = 0; r < 4; r++) {
                if (oc) oc[m + r][oc_at + k] = CMPLXF(ri[r], rq[r]);
                else to[m + r][0][to_at + k] = ri[r], to[m + r][1][to_at + k] = rq[r];
            }
        }
#endif
        for (; m < c->nch; m++) {
            float i, q;

            dot2(h, x[m][0] + at, x[m][1] + at, t, &i, &q);
            if (oc) oc[m][oc_at + k] = CMPLXF(i, q);
            else to[m][0][to_at + k] = i, to[m][1][to_at + k] = q;
        }
    }
    nx += k * d;
    int drop = nx - t + 1;

    if (drop > 0) {
        for (int m = 0; m < c->nch; m++)
            for (int j = 0; j < 2; j++) memmove(x[m][j], x[m][j] + drop, sizeof(float) * (size_t)(*have - drop));
        *have -= drop;
        nx -= drop;
    }
    *next = nx;
    return k;
}

int df_chan_run(df_chan_t *c, const uint8_t *const iq[], int n, float complex *const out[]) {
    int nout = 0;

    if (n > c->cap) n = c->cap;
    for (int s = 0; s < n; s += CHUNK) {
        int len = n - s < CHUNK ? n - s : CHUNK;

        PROF(0, nco(c, len));
        PROF(1, for (int m = 0; m < c->nch; m++) mix(c, m, iq[m] + 2 * (size_t)s, len));
        c->n1 += len;
        if (c->d2 > 1) {
            int k1;

            PROF(2, k1 = stage(c, c->x1, &c->n1, &c->next1, c->t1, c->d1, c->h1, c->x2, c->n2, NULL, 0));
            c->n2 += k1;
            PROF(3, nout += stage(c, c->x2, &c->n2, &c->next2, c->t2, c->d2, c->h2, NULL, 0, out, nout));
        } else
            PROF(2, nout += stage(c, c->x1, &c->n1, &c->next1, c->t1, c->d1, c->h1, NULL, 0, out, nout));
    }
    return nout;
}
