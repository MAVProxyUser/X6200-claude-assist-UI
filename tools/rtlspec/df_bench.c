/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - what x6x00-df's engine costs (docs/plans/kraken_df_plan.md, Phase 0, test 0c): five channels of
 *  bytes at each sample rate, 0.1 s blocks, CPU time per second of signal, i.e. the share of one core.
 *  On the Mac:   cc -O3 -o /tmp/df_bench tools/rtlspec/df_bench.c tools/rtlspec/df_{cal,chan,doa,engine}.c -lm
 *  For the radio (as x6x00-rtlspec is built, tools/rtlsdr/build.sh):
 *      arm-linux-gnueabihf-gcc --sysroot=build/x6200/sysroot -isystem build/x6200/sysroot/usr/include/arm-linux-gnueabihf \
 *          -O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -o df_bench tools/rtlspec/df_bench.c \
 *          tools/rtlspec/df_{cal,chan,doa,engine}.c -lm
 *      df_bench [seconds]
 */

#include "df.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NCH 5

#ifdef DF_PROFILE
extern double df_prof[4];
#endif

static double cpu(void) {
    struct timespec t;

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    double   secs = argc > 1 ? atof(argv[1]) : 3;
    double   rates[] = {250000, 1024000, 2400000};
    uint32_t r = 12345;

    printf("df_bench: %d channels, NFM 12.5 kHz target, 0.1 s blocks; %% of one core\n", NCH);
    printf("%10s %8s %8s %8s %8s %8s %8s\n", "rate", "filter", "engine", "DoA+LOO", "per est", "cal", "recal");
    for (int k = 0; k < 3; k++) {
        double   fs = rates[k];
        int      n = (int)(fs / 10), blocks = (int)(secs * 10);
        uint8_t *buf[NCH];

        for (int m = 0; m < NCH; m++) {
            buf[m] = malloc((size_t)n * 2);
            for (int i = 0; i < 2 * n; i++) r = r * 1664525u + 1013904223u, buf[m][i] = (uint8_t)(127 + ((r >> 24) & 31) - 16);
        }
        const uint8_t *iq[NCH] = {buf[0], buf[1], buf[2], buf[3], buf[4]};

        /* the channel filter alone */
        df_chan_t      ch;
        float complex *out[NCH];

        df_chan_init(&ch, NCH, fs, 150000 < fs / 2 ? 150000 : fs / 4, 12500, n);
        for (int m = 0; m < NCH; m++) out[m] = malloc(sizeof(float complex) * (size_t)(n + 8));
#ifdef DF_PROFILE
        memset(df_prof, 0, sizeof(df_prof));
#endif
        double t0 = cpu();

        for (int b = 0; b < blocks; b++) df_chan_run(&ch, iq, n, out);
        double t_filter = cpu() - t0;
#ifdef DF_PROFILE
        printf("  filter at %.3f MS/s: d1 %d (%d taps) d2 %d (%d taps); oscillator %.1f%%, mix %.1f%%, stage 1 %.1f%%, stage 2 %.1f%%\n",
               fs / 1e6, ch.d1, ch.t1, ch.d2, ch.t2, 100 * df_prof[0] / secs, 100 * df_prof[1] / secs, 100 * df_prof[2] / secs,
               100 * df_prof[3] / secs);
#endif

        /* the whole engine block: level, filter, R, DoA, leave-one-out */
        df_engine_t e;
        df_cal_t    cal;

        memset(&e, 0, sizeof(e));
        memset(&cal, 0, sizeof(cal));
        e.nch = NCH;
        e.fs = fs;
        e.fc = 441e6;
        e.f = 150000 < fs / 2 ? 150000 : fs / 4;
        e.bw = 12500;
        e.method = DF_MUSIC;
        df_array_uca(&e.arr, NCH, 0.29);
        df_engine_init(&e, n);
        cal.nch = NCH;
        cal.ok = true;
        for (int m = 0; m < NCH; m++) cal.gain[m] = 1;
        df_engine_set_cal(&e, &cal);
        df_est_t est;

        t0 = cpu();
        for (int b = 0; b < blocks; b++) df_engine_block(&e, iq, n, &est);
        double t_engine = cpu() - t0;

        /* DoA and leave-one-out alone, on a made-up R */
        double complex R[NCH * NCH], v[NCH];
        df_doa_t       d;
        double         fw[NCH];

        df_steer_at(&e.arr, e.steer.lambda, 40, v);
        for (int i = 0; i < NCH; i++)
            for (int j = 0; j < NCH; j++) R[i * NCH + j] = v[i] * conj(v[j]) * 10 + (i == j ? 1 : 0);
        t0 = cpu();
        for (int i = 0; i < 200; i++) {
            df_doa(R, &e.arr, &e.steer, DF_MUSIC, 1, false, &d);
            df_fit_without(R, &e.arr, &e.steer, fw);
        }
        double t_doa = (cpu() - t0) / 200;

        /* one calibration: 65,536 samples a channel */
        float complex *x[NCH];

        for (int m = 0; m < NCH; m++) {
            x[m] = malloc(sizeof(float complex) * DF_CAL_N);
            for (int i = 0; i < DF_CAL_N; i++) {
                r = r * 1664525u + 1013904223u;
                float a = ((r >> 16) & 255) / 128.0f - 1;

                r = r * 1664525u + 1013904223u;
                x[m][i] = CMPLXF(a, ((r >> 16) & 255) / 128.0f - 1);
            }
        }
        for (int m = 1; m < NCH; m++) memcpy(x[m] + 7 * m, x[0], sizeof(float complex) * (DF_CAL_N - 7 * m));
        t0 = cpu();
        df_cal_run(x, NCH, DF_CAL_N, 3000, NULL, &cal);
        double t_cal = cpu() - t0;
        int    kk[NCH];

        memcpy(kk, cal.k, sizeof(kk));
        t0 = cpu();
        df_cal_run(x, NCH, DF_CAL_N, 3000, kk, &cal);
        double t_cal2 = cpu() - t0;

        printf("%7.3f MS/s %7.1f%% %7.1f%% %7.2f%% %6.2f ms %6.0f ms %5.0f ms\n", fs / 1e6, 100 * t_filter / secs,
               100 * t_engine / secs, 100 * t_doa * 10, t_doa * 1e3, t_cal * 1e3, t_cal2 * 1e3);
        df_engine_free(&e);
        df_chan_free(&ch);
        for (int m = 0; m < NCH; m++) free(buf[m]), free(out[m]), free(x[m]);
    }
    return 0;
}
