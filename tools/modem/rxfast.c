/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the fast receiver over a radio end's recording (int16, 24 kHz): each burst found, what it trained to,
 *  how far off the carrier and the clock were, and the bits.  rxfast FILE RATE(0-3) [start_s]
 */
#include "hdlc.h"
#include "pump.h"

#include <stdio.h>
#include <stdlib.h>

static long      ones, bits;
static hdlc_rx_t h;

static void on_bit(void *ctx, int b) {
    uint8_t f[1100];
    (void)ctx;
    bits++;
    ones += b;
    int n = hdlc_rx_bit(&h, b, f, sizeof(f));
    if (n > 0)
        printf("   frame of %d bytes: %c block %d index %d of %d\n", n, f[0], f[1], n > 2 ? f[2] : -1, n > 3 ? f[3] : -1);
}

int main(int argc, char **argv) {
    static pump_t p;
    FILE         *f    = fopen(argv[1], "rb");
    rate_t        r    = (rate_t)atoi(argv[2]);
    double        from = argc > 3 ? atof(argv[3]) : 0.0;
    int16_t       s;
    long          n = 0;
    qam_state_t   was = QAM_IDLE;

    if (!f)
        return 1;
    pump_init(&p, 24000.0, on_bit, NULL);
    pump_rx_arm(&p, r);
    while (fread(&s, 2, 1, f) == 1) {
        if (n++ < from * 24000)
            continue;
        pump_rx_feed(&p, s / 32768.0);
        qam_state_t st = p.rx.state;
        if (st == QAM_TRACK && was != QAM_TRACK)
            printf("%.2f s: %s trained to %.1f dB, carrier %+.1f Hz\n", n / 24000.0, rate_name(r), p.rx.trained_snr_db,
                   p.rx.offset_hz);
        if (was == QAM_TRACK && st != QAM_TRACK) {
            printf("%.2f s: ended, symbols %d, snr %.1f dB, clock %+.0f ppm, shifts %d; %ld bits, %ld ones\n", n / 24000.0,
                   p.rx.k, p.rx.snr_db, -p.rx.rate_err * 1e6, p.rx.shifts, bits, ones);
            bits = ones = 0;
        }
        was = st;
    }
    printf("refused %d; HDLC frames good %u, bad %u\n", p.rx.fails, h.good, h.bad);
    return 0;
}
