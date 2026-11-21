/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: each fast modem through the simulated SSB link, its bits compared.
 *      test_pump [offset_hz ppm snr_db [ssb]]
 */
#include "channel.h"
#include "pump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS 24000.0
#define NBITS 20000

static uint8_t got[NBITS * 2];
static int     ngot;

static void on_bit(void *ctx, int bit) {
    (void)ctx;
    if (ngot < (int)sizeof(got))
        got[ngot++] = bit;
}

static int run(rate_t r, double off, double ppm, double snr, bool ssb, uint8_t *sent) {
    pump_t    tx, rx;
    channel_t ch;
    double    out[4];

    pump_init(&tx, FS, NULL, NULL);
    pump_init(&rx, FS, on_bit, NULL);
    channel_init(&ch, FS, ssb, off, ppm, snr);
    pump_rx_arm(&rx, r);
    ngot = 0;
    for (int i = 0; i < (int)(0.3 * FS); i++)         /* the quiet line first */
        pump_rx_feed(&rx, channel_noise(&ch));
    pump_tx_start(&tx, r);
    for (int i = 0; i < NBITS; i++)
        pump_tx_push(&tx, sent[i]);
    pump_tx_stop(&tx);
    while (pump_tx_busy(&tx)) {
        int n = channel_run(&ch, 0.5 * pump_tx_sample(&tx), out);
        for (int j = 0; j < n; j++)
            pump_rx_feed(&rx, out[j]);
    }
    for (int i = 0; i < (int)(0.3 * FS); i++) {
        int n = channel_run(&ch, 0, out);
        for (int j = 0; j < n; j++)
            pump_rx_feed(&rx, out[j]);
    }
    int best = -1, bo = 0;
    for (int o = 0; o < 32 && o < ngot; o++) {
        int e = 0;
        for (int i = 0; i < NBITS && i + o < ngot; i++)
            e += got[i + o] != sent[i];
        e += NBITS - (ngot - o < NBITS ? ngot - o : NBITS);
        if (best < 0 || e < best) {
            best = e;
            bo   = o;
        }
    }
    printf("%-13s off %+5.1f Hz %+4.0f ppm snr %4.0f %s: trained %5.1f dB, snr %5.1f dB, offset %+6.2f Hz, "
           "ppm %+5.0f, shifts %d, fails %d, bits %d (at %d), errors %d%s\n",
           rate_name(r), off, ppm, snr, ssb ? "ssb" : "flat", rx.rx.trained_snr_db, rx.rx.snr_db, rx.rx.offset_hz,
           rx.rx.rate_err * 1e6, rx.rx.shifts, rx.rx.fails, ngot, bo, best < 0 ? NBITS : best, best == 0 ? "" : "  <-- ERRORS");
    pump_free(&rx);
    pump_free(&tx);
    channel_free(&ch);
    return best;
}

int main(int argc, char **argv) {
    static uint8_t sent[NBITS];
    uint64_t       s = 99;
    double         off = argc > 1 ? atof(argv[1]) : 0, ppm = argc > 2 ? atof(argv[2]) : 0;
    double         snr = argc > 3 ? atof(argv[3]) : 300;
    bool           ssb = argc > 4 ? atoi(argv[4]) : 0;
    int            bad = 0;

    for (int i = 0; i < NBITS; i++) {
        s       = s * 6364136223846793005ULL + 1442695040888963407ULL;
        sent[i] = (s >> 33) & 1;
    }
    for (int r = getenv("RATE") ? atoi(getenv("RATE")) : 0; r < (getenv("RATE") ? atoi(getenv("RATE")) + 1 : (int)RATE_COUNT); r++)
        bad += run(r, off, ppm, snr, ssb, sent) != 0;
    return bad ? 1 : 0;
}
