/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - a bench check for x6x00-df (docs/plans/kraken_df_plan.md, test 0a): how many samples a second each
 *  of several RTL sticks really delivers when they stream at once. rtl_test says "0 per million lost" for a stick that
 *  delivered nothing at all, so its slip count alone cannot tell a starved stick from a perfect one.
 *
 *      df_rate SECONDS RATE SERIAL [SERIAL ...]
 *
 *  Opens each serial in turn (1 s apart), streams (15 x 64 KiB transfers, as x6x00-df), and prints once a second
 *  every stick's samples in that second, then the totals against what the rate asks for. Receive only.
 */

#include <rtl-sdr.h>

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXS 8

typedef struct {
    rtlsdr_dev_t   *dev;
    const char     *sn;
    _Atomic long long n;
    pthread_t       th;
} stick_t;

static stick_t      s[MAXS];
static atomic_bool  stop;

static void cb(unsigned char *buf, uint32_t len, void *ctx) {
    stick_t *k = ctx;

    (void)buf;
    if (atomic_load(&stop)) {
        rtlsdr_cancel_async(k->dev);
        return;
    }
    atomic_fetch_add(&k->n, len / 2);
}

static void *rd(void *a) {
    stick_t *k = a;

    rtlsdr_reset_buffer(k->dev);
    rtlsdr_read_async(k->dev, cb, k, 15, 65536);
    return NULL;
}

static void on_sig(int x) {
    (void)x;
    atomic_store(&stop, true);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "df_rate SECONDS RATE SERIAL [SERIAL ...]\n");
        return 2;
    }
    int  secs = atoi(argv[1]), ns = argc - 3;
    long rate = atol(argv[2]);

    if (ns > MAXS) ns = MAXS;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    for (int i = 0; i < ns; i++) {
        int idx;

        s[i].sn = argv[3 + i];
        if ((idx = rtlsdr_get_index_by_serial(s[i].sn)) < 0 || rtlsdr_open(&s[i].dev, (uint32_t)idx)) {
            fprintf(stderr, "serial %s: would not open\n", s[i].sn);
            return 1;
        }
        rtlsdr_set_sample_rate(s[i].dev, (uint32_t)rate);
        rtlsdr_set_center_freq(s[i].dev, 914900000);
        rtlsdr_set_tuner_gain_mode(s[i].dev, 1);
        rtlsdr_set_tuner_gain(s[i].dev, 280);
        rtlsdr_set_agc_mode(s[i].dev, 0);
        pthread_create(&s[i].th, NULL, rd, &s[i]);
        sleep(1);
    }
    long long last[MAXS] = {0}, t0[MAXS];

    for (int i = 0; i < ns; i++) t0[i] = atomic_load(&s[i].n);
    for (int t = 0; t < secs && !atomic_load(&stop); t++) {
        sleep(1);
        printf("%3d s:", t + 1);
        for (int i = 0; i < ns; i++) {
            long long v = atomic_load(&s[i].n);

            printf("  %s %7lld", s[i].sn, t ? v - last[i] : v - t0[i]);
            last[i] = v;
        }
        printf("\n");
        fflush(stdout);
    }
    atomic_store(&stop, true);
    for (int i = 0; i < ns; i++) rtlsdr_cancel_async(s[i].dev);
    for (int i = 0; i < ns; i++) pthread_join(s[i].th, NULL);
    printf("totals over %d s (want %ld a second):", secs, rate);
    for (int i = 0; i < ns; i++) printf("  %s %.1f %%", s[i].sn, 100.0 * (atomic_load(&s[i].n) - t0[i]) / ((double)rate * secs));
    printf("\n");
    for (int i = 0; i < ns; i++) rtlsdr_close(s[i].dev);
    return 0;
}
