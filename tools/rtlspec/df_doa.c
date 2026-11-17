/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df's arrays and bearings (df.h; docs/plans/kraken_df_plan.md 2.2-2.4).
 *
 *  Steering: a plane wave from bearing t (clockwise from forward) reaches an element at p a time (u.p)/c early,
 *  u = (cos t, -sin t) in x forward, y left; at the target's wavelength that is a phase exp(j 2 pi u.p / lambda).
 *  R = E[x x^H] (5x5). Spectra on a one-degree grid, the peak refined by a parabola through its neighbours:
 *    Bartlett a^H R a; MUSIC 1 / (n - sum over the signal eigenvectors |e^H a|^2) (= 1 / a^H En En^H a);
 *    Capon 1 / a^H R^-1 a; MEM 1 / |a^H R^-1 e_0|^2 (R^-1 from the eigenvalues, loaded by 1e-3 trace / n).
 *  The fit: Bartlett at the bearing over n trace R, 1 for one clean plane wave from there.
 */

#include "df.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------------------------------- arrays */

void df_array_uca(df_array_t *a, int n, double r) {
    memset(a, 0, sizeof(*a));
    a->kind = DF_UCA;
    a->n = n;
    a->size = r;
    for (int k = 0; k < n; k++) {                    /* the calculator: x = r cos(2 pi k/N), y = -r sin(2 pi k/N) */
        a->x[k] = r * cos(2 * M_PI * k / n);
        a->y[k] = -r * sin(2 * M_PI * k / n);
    }
}

void df_array_ula(df_array_t *a, int n, double d) {
    memset(a, 0, sizeof(*a));
    a->kind = DF_ULA;
    a->n = n;
    a->size = d;
    for (int k = 0; k < n; k++) a->y[k] = -k * d;    /* ANT 0 on the left, the rest to the right; forward broadside */
}

void df_array_calc(const df_array_t *a, double f_hz, double *s, double *res_deg) {
    double lambda = DF_C / f_hz, d = 0, ap = 0;

    switch (a->kind) {
        case DF_UCA:
            d = 2 * a->size * sin(M_PI / a->n);
            ap = 2 * a->size;
            break;
        case DF_ULA:
            d = a->size;
            ap = (a->n - 1) * a->size;
            break;
        default:                                    /* custom: the nearest pair, the widest pair */
            d = 1e9;
            for (int i = 0; i < a->n; i++)
                for (int j = i + 1; j < a->n; j++) {
                    double e = hypot(a->x[i] - a->x[j], a->y[i] - a->y[j]);

                    if (e < d) d = e;
                    if (e > ap) ap = e;
                }
    }
    *s = d / lambda;
    *res_deg = ap > 0 ? (1.22 / (ap / lambda)) * 180 / M_PI / 10 : 0;
}

void df_steer_at(const df_array_t *a, double lambda, double deg, double complex *v) {
    double t = deg * M_PI / 180, ux = cos(t), uy = -sin(t);

    for (int k = 0; k < a->n; k++) v[k] = cexp(I * 2 * M_PI * (a->x[k] * ux + a->y[k] * uy) / lambda);
}

void df_steer_make(df_steer_t *s, const df_array_t *a, double lambda) {
    double complex v[DF_MAXCH];

    s->n = a->n;
    s->lambda = lambda;
    for (int g = 0; g < DF_GRID; g++) {
        df_steer_at(a, lambda, g * 360.0 / DF_GRID, v);
        for (int k = 0; k < a->n; k++) s->a[g][k] = (float complex)v[k];
    }
}

/* ------------------------------------------------------------------------------------------------- eigen */

void df_eigh(const double complex *A0, int n, double *val, double complex *V) {
    double complex A[DF_MAXCH * DF_MAXCH];
    double         norm = 0;

    memcpy(A, A0, sizeof(double complex) * (size_t)(n * n));
    for (int i = 0; i < n * n; i++) {
        V[i] = (i / n == i % n) ? 1 : 0;
        norm += creal(A[i]) * creal(A[i]) + cimag(A[i]) * cimag(A[i]);
    }
    for (int sweep = 0; sweep < 60; sweep++) {
        double off = 0;

        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++) off += creal(A[p * n + q]) * creal(A[p * n + q]) + cimag(A[p * n + q]) * cimag(A[p * n + q]);
        if (off <= 1e-26 * norm || off == 0) break;
        for (int p = 0; p < n; p++)
            for (int q = p + 1; q < n; q++) {
                double complex c = A[p * n + q];
                double         r = cabs(c);

                if (r <= 1e-300) continue;
                double complex e = c / r;
                double         a = creal(A[p * n + p]), b = creal(A[q * n + q]), th = 0.5 * atan2(2 * r, b - a);
                double         cs = cos(th), sn = sin(th);
                /* J = diag(1, conj e) * [[cs, sn], [-sn, cs]] */
                double complex jpp = cs, jpq = sn, jqp = -conj(e) * sn, jqq = conj(e) * cs;

                for (int k = 0; k < n; k++) {               /* A J, and V J */
                    double complex akp = A[k * n + p], akq = A[k * n + q];

                    A[k * n + p] = akp * jpp + akq * jqp;
                    A[k * n + q] = akp * jpq + akq * jqq;
                    double complex vkp = V[k * n + p], vkq = V[k * n + q];

                    V[k * n + p] = vkp * jpp + vkq * jqp;
                    V[k * n + q] = vkp * jpq + vkq * jqq;
                }
                for (int k = 0; k < n; k++) {               /* J^H (A J) */
                    double complex apk = A[p * n + k], aqk = A[q * n + k];

                    A[p * n + k] = conj(jpp) * apk + conj(jqp) * aqk;
                    A[q * n + k] = conj(jpq) * apk + conj(jqq) * aqk;
                }
                A[p * n + q] = A[q * n + p] = 0;
                A[p * n + p] = creal(A[p * n + p]);
                A[q * n + q] = creal(A[q * n + q]);
            }
    }
    for (int i = 0; i < n; i++) val[i] = creal(A[i * n + i]);
    for (int i = 0; i < n; i++) {                           /* largest first */
        int best = i;

        for (int j = i + 1; j < n; j++)
            if (val[j] > val[best]) best = j;
        if (best != i) {
            double t = val[i];

            val[i] = val[best];
            val[best] = t;
            for (int k = 0; k < n; k++) {
                double complex u = V[k * n + i];

                V[k * n + i] = V[k * n + best];
                V[k * n + best] = u;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------- DoA */

static double quad(const double complex *R, int n, const double complex *v) {          /* v^H R v, real */
    double complex s = 0;

    for (int i = 0; i < n; i++) {
        double complex t = 0;

        for (int j = 0; j < n; j++) t += R[i * n + j] * v[j];
        s += conj(v[i]) * t;
    }
    return creal(s);
}

static int wrapi(int g) {
    return ((g % DF_GRID) + DF_GRID) % DF_GRID;
}

static double wrapd(double d) {
    d = fmod(d, 360.0);
    return d < 0 ? d + 360 : d;
}

void df_fit_without(const double complex *R, const df_array_t *arr, const df_steer_t *st, double *fit) {
    int    n = arr->n;
    double tr = 0, best[DF_MAXCH] = {0};

    for (int i = 0; i < n; i++) tr += creal(R[i * n + i]);
    for (int g = 0; g < DF_GRID; g++) {
        double complex a[DF_MAXCH], t[DF_MAXCH], all = 0;

        for (int k = 0; k < n; k++) a[k] = st->a[g][k];
        for (int i = 0; i < n; i++) {
            t[i] = 0;
            for (int j = 0; j < n; j++) t[i] += R[i * n + j] * a[j];
            all += conj(a[i]) * t[i];
        }
        for (int m = 0; m < n; m++) {                       /* a^H R a less row m and column m */
            double complex s = all - conj(a[m]) * t[m];

            for (int i = 0; i < n; i++)
                if (i != m) s -= conj(a[i]) * R[i * n + m] * a[m];
            if (creal(s) > best[m]) best[m] = creal(s);
        }
    }
    for (int m = 0; m < n; m++) {
        double trm = tr - creal(R[m * n + m]);

        fit[m] = trm > 0 ? best[m] / ((n - 1) * trm) : 0;
    }
}

/* a line looks the same from t and from 180 - t: its front is 270..360 and 0..90 */
static bool in_front(int g) {
    return g <= DF_GRID / 4 || g >= 3 * DF_GRID / 4;
}

int df_doa(const double complex *R, const df_array_t *arr, const df_steer_t *st, df_method_t m, int nsrc,
           bool front_only, df_doa_t *o) {
    int            n = arr->n;
    double         val[DF_MAXCH], trace = 0, P[DF_GRID], B[DF_GRID];
    double complex V[DF_MAXCH * DF_MAXCH], Ri[DF_MAXCH * DF_MAXCH], a[DF_MAXCH];
    bool           line = arr->kind == DF_ULA;

    memset(o, 0, sizeof(*o));
    o->mirror = -1;
    if (n < 2 || n > DF_MAXCH || st->n != n) return -1;
    for (int i = 0; i < n; i++) trace += creal(R[i * n + i]);
    if (!(trace > 0)) return -1;
    if (nsrc < 1) nsrc = 1;
    if (nsrc > n - 1) nsrc = n - 1;

    df_eigh(R, n, val, V);
    double noise = 0;

    for (int i = nsrc; i < n; i++) noise += val[i];
    noise /= (n - nsrc);
    if (noise <= 0) noise = trace * 1e-12;
    o->eig_db = 10 * log10(val[0] / (val[1] > trace * 1e-12 ? val[1] : trace * 1e-12));
    double snr = (val[0] - noise) / (n * noise);

    o->snr_db = 10 * log10(snr > 1e-6 ? snr : 1e-6);

    if (m == DF_CAPON || m == DF_MEM) {                     /* R^-1, loaded */
        double load = 1e-3 * trace / n;

        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                double complex s = 0;

                for (int k = 0; k < n; k++) s += V[i * n + k] * conj(V[j * n + k]) / (val[k] + load);
                Ri[i * n + j] = s;
            }
    }

    for (int g = 0; g < DF_GRID; g++) {
        for (int k = 0; k < n; k++) a[k] = st->a[g][k];
        B[g] = quad(R, n, a);
        switch (m) {
            case DF_BARTLETT: P[g] = B[g]; break;
            case DF_CAPON: P[g] = 1 / fmax(quad(Ri, n, a), 1e-30); break;
            case DF_MEM: {
                double complex s = 0;

                for (int k = 0; k < n; k++) s += conj(a[k]) * Ri[k * n + 0];
                P[g] = 1 / fmax(creal(s) * creal(s) + cimag(s) * cimag(s), 1e-30);
                break;
            }
            default: {
                double proj = 0;

                for (int i = 0; i < nsrc; i++) {
                    double complex s = 0;

                    for (int k = 0; k < n; k++) s += conj(V[k * n + i]) * a[k];
                    proj += creal(s) * creal(s) + cimag(s) * cimag(s);
                }
                P[g] = 1 / fmax(n - proj, 1e-9);
            }
        }
    }

    /* the peak (a line: in front only, the mirror its reflection) */
    int    at = -1;
    double pmax = 0, sum = 0;

    for (int g = 0; g < DF_GRID; g++) {
        sum += P[g];
        if ((!line || in_front(g)) && (at < 0 || P[g] > pmax)) pmax = P[g], at = g;
    }
    double l = 10 * log10(P[wrapi(at - 1)] / pmax), c0 = 0, r = 10 * log10(P[wrapi(at + 1)] / pmax),
           den = l - 2 * c0 + r, delta = den < 0 ? 0.5 * (l - r) / den : 0;

    if (delta > 0.5) delta = 0.5;
    if (delta < -0.5) delta = -0.5;
    o->bearing = wrapd((at + delta) * 360.0 / DF_GRID);
    if (line) o->mirror = wrapd(180 - o->bearing);
    o->papr_db = 10 * log10(pmax / (sum / DF_GRID));
    for (int g = 0; g < DF_GRID; g++) {
        double d = 10 * log10(P[g] / pmax);

        if (line && front_only && !in_front(g)) d = -60;
        o->p_db[g] = (float)(d < -60 ? -60 : d);
    }

    /* fit and width, from Bartlett */
    df_steer_at(arr, st->lambda, o->bearing, a);
    o->fit = quad(R, n, a) / (n * trace);
    if (o->fit < 0) o->fit = 0;
    double half = 0.5 * B[at], w = 0;

    for (int dir = -1; dir <= 1; dir += 2) {
        int k = 1;

        while (k < DF_GRID / 2 && B[wrapi(at + dir * k)] > half) k++;
        double b0 = B[wrapi(at + dir * (k - 1))], b1 = B[wrapi(at + dir * k)];

        w += (k - 1) + (b0 > b1 ? (b0 - half) / (b0 - b1) : 0);
    }
    o->width_deg = w * 360.0 / DF_GRID;

    /* other peaks within 6 dB, away from the main one (and a line's mirror) */
    double keep = fmax(o->width_deg / 2, 10);

    for (int g = 0; g < DF_GRID && o->npeaks < 3; g++) {
        if (o->p_db[g] < -6 || o->p_db[g] < o->p_db[wrapi(g - 1)] || o->p_db[g] <= o->p_db[wrapi(g + 1)]) continue;
        double d0 = fabs(fmod(g * 360.0 / DF_GRID - o->bearing + 540, 360) - 180);
        double d1 = o->mirror >= 0 ? fabs(fmod(g * 360.0 / DF_GRID - o->mirror + 540, 360) - 180) : 999;

        if (d0 > keep && d1 > keep) o->peaks[o->npeaks++] = g * 360.0 / DF_GRID;
    }
    return 0;
}
