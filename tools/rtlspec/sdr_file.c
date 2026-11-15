/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a test device for x6x00-rtlspec (sdr.h), -T FILE: a transmitter that writes what it is given (signed
 *  8-bit I/Q at the rate asked) into FILE at the pace a device would take it, and a receiver that hears nothing (zero
 *  samples, paced). Nothing goes near a radio: the daemon's transmitting (its refusals, its FM, its time-outs) is
 *  checked with it and the file looked at afterwards (tools/rtlspec/tx_check.py). Used only when asked for, under a
 *  socket name of its own (-S), beside the radio's own daemon.
 */

#include "sdr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

const char *sdr_file_path;              /* -T */

static uint32_t       rate = 2116800;
static volatile int   tx_end;

static int f_present(void) {
    return sdr_file_path != NULL;
}

static int f_open(char *tuner, size_t n, uint64_t *lo, uint64_t *hi) {
    snprintf(tuner, n, "TXFILE");
    *lo = 1000000;
    *hi = 6000000000ULL;
    return 0;
}

static int f_rate(uint32_t want, uint32_t *got) {
    rate = *got = want;
    return 0;
}

static int f_freq(uint64_t hz) {
    (void)hz;
    return 0;
}

static void f_gain(int tenths) {
    (void)tenths;
}

static double now_s(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Zero samples (the middle of the unsigned scale), 50 ms a transfer, at the rate's pace */
static int f_rx(sdr_rx_cb cb, volatile int *halt) {
    static unsigned char buf[2 * 211680];
    uint32_t             n = rate / 20 * 2;

    if (n > sizeof(buf)) n = sizeof(buf);
    memset(buf, 128, n);
    while (!*halt) {
        cb(buf, n);
        usleep(50000);
    }
    return 0;
}

static void f_rx_stop(void) {
}

static int f_tx(uint64_t hz, uint32_t r, int gain_db, bool amp, sdr_tx_fill fill) {
    static int8_t iq[2 * 65536];
    FILE         *f = fopen(sdr_file_path, "wb");
    double        t0 = now_s(), sent = 0;
    int           last = 0;

    if (!f) return -1;
    fprintf(stderr, "rtlspec: (test) transmitting into %s: %llu Hz, %u S/s, gain %d dB, amplifier %s\n",
            sdr_file_path, (unsigned long long)hz, r, gain_db, amp ? "on" : "off");
    tx_end = 0;
    while (!tx_end && !last) {
        last = fill(iq, 65536);
        fwrite(iq, 2, 65536, f);
        sent += 65536;
        double ahead = sent / r - (now_s() - t0);       /* a device's pace */

        if (ahead > 0) usleep((useconds_t)(ahead * 1e6));
    }
    fclose(f);
    return 0;
}

static void f_tx_stop(void) {
    tx_end = 1;
}

static void f_close(void) {
}

const sdr_ops_t sdr_file = {
    .kind        = "TXFILE",
    .present     = f_present,
    .open        = f_open,
    .rate        = f_rate,
    .freq        = f_freq,
    .gain        = f_gain,
    .rx          = f_rx,
    .rx_stop     = f_rx_stop,
    .tx          = f_tx,
    .tx_stop     = f_tx_stop,
    .tx_gain_max = 47,
    .close       = f_close,
};
