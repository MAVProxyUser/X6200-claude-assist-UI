/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df's engine on made-up KrakenSDR data (docs/plans/kraken_df_plan.md, Phase 0, test 0b). Each
 *  channel is made the way the hardware would hand it over: a far transmitter's NFM signal reaching each element of
 *  the array (positions from the Antenna Array Size Calculator's formulas, the time of flight from the distance, not
 *  the engine's steering), then that channel's own whole and fractional sample delay, phase and gain, its own noise,
 *  and 8-bit bytes. Calibration blocks are one noise shared by every channel through the same hardware.
 *  Off the radio:
 *      cc -O2 -Wall -Wextra -o /tmp/df_test tools/rtlspec/df_test.c tools/rtlspec/df_cal.c tools/rtlspec/df_chan.c \
 *          tools/rtlspec/df_doa.c tools/rtlspec/df_engine.c -lm && /tmp/df_test [-v]
 */

#include "df.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS      1024000.0
#define FC      441000000.0     /* the tuners */
#define FOFF    150000.0        /* the transmitter, from the tuners' centre */
#define BW      12500.0
#define BLOCK   102400          /* 0.1 s */
#define START   3200            /* read start: whole delays up to 3000 either way */
#define FDM     32              /* the fractional-delay filter: 2 FDM + 1 taps */

static int verbose, fails, checks;

static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(int ok, const char *fmt, ...) {
    va_list ap;

    checks++;
    if (!ok) fails++;
    if (!ok || verbose) {
        printf("%s ", ok ? "  ok  " : "  FAIL");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

/* ------------------------------------------------------------------------------------------------- random */

static uint64_t rs = 88172645463325252ULL;

static double urand(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (rs >> 11) * (1.0 / 9007199254740992.0);
}

static double gauss(void) {
    double u = urand(), v = urand();

    return sqrt(-2 * log(u + 1e-300)) * cos(2 * M_PI * v);
}

/* ------------------------------------------------------------------------------------------------- the hardware */

typedef struct {
    int    D;                   /* whole-sample delay */
    double tau, phi, g;         /* fractional delay, phase, gain */
} hw_t;

static void hw_random(hw_t *hw, int n) {
    for (int m = 0; m < n; m++) {
        hw[m].D = (int)(urand() * 3000);
        hw[m].tau = urand() - 0.5;
        hw[m].phi = 2 * M_PI * urand();
        hw[m].g = 0.7 + 0.7 * urand();
    }
}

typedef struct {
    double bearing, band_snr_db, f;     /* degrees, dB in BW, offset */
    double dev1, fm1, dev2, fm2, ph2;   /* NFM: two tones */
    int    flat;                        /* the same phase at every element (no direction in the plane) */
} src_t;

static uint8_t q8(double x, double scale) {
    long v = lround(127.5 + x * scale);

    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* channel streams of len samples: sources through the array and the hardware, noise of power 1 per channel */
static uint8_t **gen(const hw_t *hw, const df_array_t *arr, const src_t *src, int nsrc, int len, double t0) {
    int       n = arr->n;
    uint8_t **ch = malloc(sizeof(*ch) * (size_t)n);
    double    ps = 0;

    for (int s = 0; s < nsrc; s++) ps += pow(10, src[s].band_snr_db / 10) * BW / FS;
    double scale = 22 / sqrt((1 + ps) / 2);             /* about 22 counts rms per component */

    for (int m = 0; m < n; m++) {
        ch[m] = malloc((size_t)len * 2);
        double tg[4];

        for (int s = 0; s < nsrc; s++) {                /* how much earlier the wave reaches this element */
            double b = src[s].bearing * M_PI / 180, far = 1e4, fx = far * cos(b), fy = -far * sin(b);

            tg[s] = src[s].flat ? 0 : (far - hypot(fx - arr->x[m], fy - arr->y[m])) / DF_C;
        }
        for (int i = 0; i < len; i++) {
            double         t = (t0 + i - hw[m].D - hw[m].tau) / FS;
            double complex y = 0;

            for (int s = 0; s < nsrc; s++) {
                double tt = t + tg[s], A = sqrt(pow(10, src[s].band_snr_db / 10) * BW / FS);
                double ph = 2 * M_PI * src[s].f * tt + src[s].dev1 / src[s].fm1 * sin(2 * M_PI * src[s].fm1 * tt) +
                            src[s].dev2 / src[s].fm2 * sin(2 * M_PI * src[s].fm2 * tt + src[s].ph2) + 2 * M_PI * (FC * tg[s]);

                y += A * cexp(I * ph);
            }
            y = hw[m].g * cexp(I * hw[m].phi) * y + (gauss() + I * gauss()) * sqrt(0.5);
            ch[m][2 * i] = q8(creal(y), scale);
            ch[m][2 * i + 1] = q8(cimag(y), scale);
        }
    }
    return ch;
}

/* a calibration block: one white noise into every channel through its hardware, 25 dB over the channel's own */
static uint8_t **gen_cal(const hw_t *hw, int n, int len) {
    int             maxd = 0;
    uint8_t       **ch = malloc(sizeof(*ch) * (size_t)n);

    for (int m = 0; m < n; m++)
        if (hw[m].D > maxd) maxd = hw[m].D;
    int             pre = maxd + FDM + 2;
    double complex *w = malloc(sizeof(*w) * (size_t)(len + pre + FDM + 2));

    for (int i = 0; i < len + pre + FDM + 2; i++) w[i] = (gauss() + I * gauss()) * sqrt(0.5);
    for (int m = 0; m < n; m++) {
        double h[2 * FDM + 1];

        for (int k = 0; k <= 2 * FDM; k++) {
            double x = k - FDM - hw[m].tau, s = fabs(x) < 1e-12 ? 1 : sin(M_PI * x) / (M_PI * x);

            h[k] = s * (0.42 - 0.5 * cos(2 * M_PI * k / (2 * FDM)) + 0.08 * cos(4 * M_PI * k / (2 * FDM)));
        }
        ch[m] = malloc((size_t)len * 2);
        for (int i = 0; i < len; i++) {
            double complex y = 0;

            for (int k = 0; k <= 2 * FDM; k++) y += h[k] * w[pre + i - hw[m].D - k + FDM];
            y = hw[m].g * cexp(I * hw[m].phi) * y + (gauss() + I * gauss()) * sqrt(0.5 * pow(10, -2.5));
            ch[m][2 * i] = q8(creal(y), 25 / sqrt(0.5));
            ch[m][2 * i + 1] = q8(cimag(y), 25 / sqrt(0.5));
        }
    }
    free(w);
    return ch;
}

static void free_ch(uint8_t **ch, int n) {
    for (int m = 0; m < n; m++) free(ch[m]);
    free(ch);
}

static int calibrate_k(const hw_t *hw, int n, const int *known_k, df_cal_t *cal) {
    uint8_t      **c = gen_cal(hw, n, DF_CAL_N);
    float complex *x[DF_MAXCH];

    for (int m = 0; m < n; m++) {
        x[m] = malloc(sizeof(float complex) * DF_CAL_N);
        df_u8_to_cf(c[m], DF_CAL_N, x[m]);
    }
    int rc = df_cal_run(x, n, DF_CAL_N, 3100, known_k, cal);

    for (int m = 0; m < n; m++) free(x[m]);
    free_ch(c, n);
    return rc;
}

static int calibrate(const hw_t *hw, int n, df_cal_t *cal) {
    return calibrate_k(hw, n, NULL, cal);
}

static double angdiff(double a, double b) {
    return fabs(fmod(a - b + 540, 360) - 180);
}

static src_t nfm(double bearing, double snr) {
    src_t s = {bearing, snr, FOFF, 1500, 800, 1000, 1700, 1.0, 0};

    return s;
}

static void engine_for(df_engine_t *e, const df_array_t *arr) {
    memset(e, 0, sizeof(*e));
    e->nch = arr->n;
    e->fs = FS;
    e->fc = FC;
    e->f = FOFF;
    e->bw = BW;
    e->arr = *arr;
    e->method = DF_MUSIC;
    if (df_engine_init(e, BLOCK)) {
        printf("engine init failed\n");
        exit(2);
    }
}

/* the blocks of ch from block b0, each channel read k[m] later; the last estimate out */
static void run(df_engine_t *e, uint8_t **ch, const df_cal_t *cal, int b0, int nb, df_est_t *last) {
    for (int b = b0; b < b0 + nb; b++) {
        const uint8_t *iq[DF_MAXCH];

        for (int m = 0; m < e->nch; m++) iq[m] = ch[m] + 2 * (size_t)(START + cal->k[m] + b * BLOCK);
        df_engine_block(e, iq, BLOCK, last);
    }
}

/* ------------------------------------------------------------------------------------------------- tests */

static void test_calc(void) {
    df_array_t a;
    double     s, r;

    /* the sheet's own rows: Circular, 416 MHz, s 0.5: max radius 30.6725 cm, est. resolution 8.2173 */
    df_array_uca(&a, 5, 0.306725051);
    df_array_calc(&a, 416e6 * DF_C / 3e8, &s, &r);          /* the sheet's lambda is 300 / f */
    check(fabs(s - 0.5) < 1e-4 && fabs(r - 8.21734) < 1e-3, "calculator, circular: s %.4f (0.5), resolution %.4f (8.2173)", s, r);
    /* Linear, 440 MHz, s 0.5: spacing 34.0909 cm, est. resolution 3.4950 */
    df_array_ula(&a, 5, 0.340909091);
    df_array_calc(&a, 440e6 * DF_C / 3e8, &s, &r);
    check(fabs(s - 0.5) < 1e-4 && fabs(r - 3.49504) < 1e-3, "calculator, linear: s %.4f (0.5), resolution %.4f (3.4950)", s, r);
    /* ANT k of a circle at 360k/N clockwise */
    df_array_uca(&a, 5, 0.3);
    check(fabs(a.x[1] - 0.3 * cos(2 * M_PI / 5)) < 1e-12 && a.y[1] < 0, "calculator, ANT 1 at 72 degrees clockwise");
}

static void test_eigh(void) {
    double complex A[25], V[25];
    double         val[5], worst = 0;

    for (int i = 0; i < 5; i++)
        for (int j = i; j < 5; j++) {
            A[i * 5 + j] = i == j ? gauss() * 3 : gauss() + I * gauss();
            A[j * 5 + i] = conj(A[i * 5 + j]);
        }
    df_eigh(A, 5, val, V);
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < 5; i++) {
            double complex s = 0;

            for (int j = 0; j < 5; j++) s += A[i * 5 + j] * V[j * 5 + k];
            double e = cabs(s - val[k] * V[i * 5 + k]);

            if (e > worst) worst = e;
        }
    check(worst < 1e-9 && val[0] >= val[1] && val[3] >= val[4], "eigen: A v = l v within %.1e, largest first", worst);
}

static void test_cal(void) {
    double wd = 0, wp = 0, wg = 0;
    int    bad = 0;

    for (int run = 0; run < 10; run++) {
        hw_t     hw[5];
        df_cal_t cal;

        hw_random(hw, 5);
        if (calibrate(hw, 5, &cal)) {
            bad++;
            if (verbose) printf("        cal %d: %s\n", run, cal.why);
            continue;
        }
        for (int m = 1; m < 5; m++) {
            double want = (hw[m].D + hw[m].tau) - (hw[0].D + hw[0].tau), got = cal.k[m] + cal.tau[m];
            double dp = fabs(remainder(cal.phi[m] - (hw[m].phi - hw[0].phi), 2 * M_PI)) * 180 / M_PI;
            double dg = fabs(cal.gain[m] / (hw[0].g / hw[m].g) - 1);

            wd = fmax(wd, fabs(got - want));
            wp = fmax(wp, dp);
            wg = fmax(wg, dg);
        }
    }
    check(!bad, "calibration: %d of 10 random sets failed", bad);
    {
        hw_t     hw[5];
        df_cal_t a, b;

        hw_random(hw, 5);
        calibrate(hw, 5, &a);
        calibrate_k(hw, 5, a.k, &b);
        double dp = 0;

        for (int m = 1; m < 5; m++) dp = fmax(dp, fabs(remainder(a.phi[m] - b.phi[m], 2 * M_PI)) * 180 / M_PI);
        check(b.ok && !memcmp(a.k, b.k, sizeof(a.k)) && dp < 0.3, "calibration with the delays known: same delays, phases within %.3f", dp);
    }
    check(wd < 0.02, "calibration: delays within %.4f sample (0.02)", wd);
    check(wp < 0.5, "calibration: phases within %.3f degree (0.5)", wp);
    check(wg < 0.02, "calibration: gains within %.2f %% (2 %%)", wg * 100);
}

static void test_bearings(void) {
    struct { const char *name; df_kind_t kind; int n; double s; double b[6]; int nb; } cases[] = {
        {"UCA-5", DF_UCA, 5, 0.5, {0, 37, 72, 144, 200, 300}, 6},
        {"UCA-5 s 0.25", DF_UCA, 5, 0.25, {0, 37, 200}, 3},
        {"UCA-4", DF_UCA, 4, 0.5, {0, 37, 200}, 3},
        {"UCA-3", DF_UCA, 3, 0.5, {0, 37, 200}, 3},
        {"ULA-5", DF_ULA, 5, 0.5, {0, 37, 300, 80}, 4},
    };
    double lambda = DF_C / (FC + FOFF);

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
        for (int j = 0; j < cases[c].nb; j++) {
            df_array_t  arr;
            hw_t        hw[5];
            df_cal_t    cal;
            df_engine_t e;
            df_est_t    est;
            double      d = cases[c].s * lambda, want = cases[c].b[j];

            if (cases[c].kind == DF_UCA) df_array_uca(&arr, cases[c].n, d / (2 * sin(M_PI / cases[c].n)));
            else df_array_ula(&arr, cases[c].n, d);
            hw_random(hw, cases[c].n);
            if (calibrate(hw, cases[c].n, &cal)) {
                check(0, "%s at %.0f: calibration: %s", cases[c].name, want, cal.why);
                continue;
            }
            src_t     s = nfm(want, 20);
            uint8_t **ch = gen(hw, &arr, &s, 1, START * 2 + 6 * BLOCK, 0);

            engine_for(&e, &arr);
            df_engine_set_cal(&e, &cal);
            run(&e, ch, &cal, 0, 5, &est);
            double err = angdiff(est.doa.bearing, want);
            int    ok = est.gate && err < 1 && est.doa.fit > 0.9;

            if (cases[c].kind == DF_ULA) ok = ok && angdiff(est.doa.mirror, 180 - want) < 1;
            check(ok, "%s s %.2f at %5.1f: bearing %6.2f (error %.2f), fit %.3f, uncert %.1f, eig %.1f dB, level %.1f dB%s",
                  cases[c].name, cases[c].s, want, est.doa.bearing, err, est.doa.fit, est.uncert_deg, est.doa.eig_db,
                  est.level_db, est.gate ? "" : ", GATE SHUT");
            df_engine_free(&e);
            free_ch(ch, cases[c].n);
        }
}

static void test_methods(void) {
    df_method_t ms[] = {DF_BARTLETT, DF_CAPON, DF_MEM};
    const char *names[] = {"Bartlett", "Capon", "MEM"};
    df_array_t  arr;
    hw_t        hw[5];
    df_cal_t    cal;

    df_array_uca(&arr, 5, 0.5 * DF_C / (FC + FOFF) / (2 * sin(M_PI / 5)));
    hw_random(hw, 5);
    calibrate(hw, 5, &cal);
    src_t     s = nfm(123, 20);
    uint8_t **ch = gen(hw, &arr, &s, 1, START * 2 + 6 * BLOCK, 0);

    for (int k = 0; k < 3; k++) {
        df_engine_t e;
        df_est_t    est;

        engine_for(&e, &arr);
        e.method = ms[k];
        df_engine_set_cal(&e, &cal);
        run(&e, ch, &cal, 0, 5, &est);
        check(est.gate && angdiff(est.doa.bearing, 123) < 1.5, "%s at 123: bearing %.2f", names[k], est.doa.bearing);
        df_engine_free(&e);
    }
    free_ch(ch, 5);
}

static void test_interferer(void) {
    df_array_t  arr;
    hw_t        hw[5];
    df_cal_t    cal;
    df_engine_t e;
    df_est_t    est;
    src_t       s[2] = {nfm(37, 20), nfm(157, 10)};

    s[1].fm1 = 600, s[1].fm2 = 2100, s[1].ph2 = 2.2;
    df_array_uca(&arr, 5, 0.5 * DF_C / (FC + FOFF) / (2 * sin(M_PI / 5)));
    hw_random(hw, 5);
    calibrate(hw, 5, &cal);
    uint8_t **ch = gen(hw, &arr, s, 2, START * 2 + 6 * BLOCK, 0);

    engine_for(&e, &arr);
    df_engine_set_cal(&e, &cal);
    run(&e, ch, &cal, 0, 5, &est);
    check(angdiff(est.doa.bearing, 37) < 2, "a second transmitter 10 dB down at 157: bearing %.2f (37), fit %.2f, gate %d",
          est.doa.bearing, est.doa.fit, est.gate);
    df_engine_free(&e);
    free_ch(ch, 5);
}

static void test_no_bearing(void) {
    df_array_t  arr;
    hw_t        hw[5];
    df_cal_t    cal;
    df_engine_t e;
    df_est_t    est;
    int         gated = 0, slips = 0;

    df_array_uca(&arr, 5, 0.5 * DF_C / (FC + FOFF) / (2 * sin(M_PI / 5)));
    hw_random(hw, 5);
    calibrate(hw, 5, &cal);

    /* noise alone */
    uint8_t **ch = gen(hw, &arr, NULL, 0, START * 2 + 11 * BLOCK, 0);

    engine_for(&e, &arr);
    df_engine_set_cal(&e, &cal);
    for (int b = 0; b < 10; b++) {
        run(&e, ch, &cal, b, 1, &est);
        gated += est.gate;
        slips += est.cal_wanted == 1;
    }
    check(!gated && !slips, "noise alone: %d bearings claimed, %d slips (none), level %.1f dB", gated, slips, est.level_db);
    df_engine_free(&e);
    free_ch(ch, 5);

    /* the same phase at every element of a circle: no direction in its plane */
    src_t s = nfm(0, 20);

    s.flat = 1;
    ch = gen(hw, &arr, &s, 1, START * 2 + 6 * BLOCK, 0);
    engine_for(&e, &arr);
    df_engine_set_cal(&e, &cal);
    gated = 0;
    for (int b = 0; b < 5; b++) {
        run(&e, ch, &cal, b, 1, &est);
        gated += est.gate;
    }
    check(!gated, "equal phases on a circle: %d bearings claimed (none), fit %.2f", gated, est.doa.fit);
    df_engine_free(&e);
    free_ch(ch, 5);

    /* the timer: due after 1 s, taken in a gap */
    ch = gen(hw, &arr, NULL, 0, START * 2 + 16 * BLOCK, 0);
    engine_for(&e, &arr);
    e.cal_every_s = 1;
    df_engine_set_cal(&e, &cal);
    double when = -1;

    for (int b = 0; b < 15 && when < 0; b++) {
        run(&e, ch, &cal, b, 1, &est);
        if (est.cal_wanted == 2) when = (b + 1) * 0.1;
    }
    check(when >= 1.0 && when <= 1.5, "calibration timer (1 s) asked at %.1f s", when);
    df_engine_free(&e);
    free_ch(ch, 5);
}

/* a stick loses `lost` samples on channel 3 halfway: the slip must be caught, no wrong bearing shown meanwhile, and a
 * new calibration (the hardware's delay now `lost` shorter) brings the bearing back */
static void test_slip(int lost, int must_catch) {
    df_array_t  arr;
    hw_t        hw[5];
    df_cal_t    cal;
    df_engine_t e;
    df_est_t    est;
    int         nb = 30, at = 15, len = START * 2 + (nb + 2) * BLOCK + lost;
    src_t       s = nfm(37, 20);

    df_array_uca(&arr, 5, 0.5 * DF_C / (FC + FOFF) / (2 * sin(M_PI / 5)));
    hw_random(hw, 5);
    calibrate(hw, 5, &cal);
    uint8_t **ch = gen(hw, &arr, &s, 1, len, 0);
    size_t    cut = 2 * (size_t)(START + cal.k[3] + at * BLOCK + BLOCK / 2);

    memmove(ch[3] + cut, ch[3] + cut + 2 * (size_t)lost, (size_t)len * 2 - cut - 2 * (size_t)lost);
    engine_for(&e, &arr);
    df_engine_set_cal(&e, &cal);
    double caught = -1, worst = 0, minfit = 1;
    int    bad = -1;

    for (int b = 0; b < nb && caught < 0; b++) {
        run(&e, ch, &cal, b, 1, &est);
        if (verbose && b >= at - 1 && b <= at + 4)
            printf("        block %d: bearing %.1f fit %.2f gate %d bad %d level %.1f\n", b, est.doa.bearing, est.doa.fit,
                   est.gate, est.bad_ch, est.level_db);
        if (b >= at) {
            if (est.gate) worst = fmax(worst, angdiff(est.doa.bearing, 37));
            minfit = fmin(minfit, est.doa.fit);
            if (est.cal_wanted == 1) caught = (b + 1 - at - 0.5) * 0.1, bad = est.bad_ch;
        }
    }
    if (must_catch) {
        check(caught >= 0 && caught <= 1.0 && bad == 3, "slip of %d samples: caught %.2f s after (1 s), channel %d (3)",
              lost, caught, bad);
        check(worst < 5, "slip of %d samples: worst bearing shown meanwhile %.1f degrees off", lost, worst);
        hw[3].D -= lost;                                    /* what a new calibration sees */
        df_cal_t c2;
        int      stale = calibrate_k(hw, 5, cal.k, &c2);

        check(stale != 0, "slip of %d samples: the old whole-sample delays refused (%s)", lost, c2.why);
        calibrate(hw, 5, &c2);
        check(c2.ok && c2.k[3] == cal.k[3] - lost, "slip of %d samples: new search finds channel 3 %d samples sooner",
              lost, cal.k[3] - c2.k[3]);
        df_engine_set_cal(&e, &c2);
        int b = at + 12;

        run(&e, ch, &c2, b, 4, &est);
        check(est.gate && angdiff(est.doa.bearing, 37) < 1, "slip of %d samples: after calibrating, bearing %.2f (37)",
              lost, est.doa.bearing);
    } else
        printf("  info  slip of %d sample at %.0f kHz off centre: %s, worst bearing shown %.1f degrees off, lowest fit %.2f\n",
               lost, FOFF / 1e3, caught >= 0 ? "caught" : "not caught by the fit", worst, minfit);
    df_engine_free(&e);
    free_ch(ch, 5);
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");
    test_calc();
    test_eigh();
    test_cal();
    test_bearings();
    test_methods();
    test_interferer();
    test_no_bearing();
    test_slip(256, 1);
    test_slip(1, 0);
    printf("df_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
