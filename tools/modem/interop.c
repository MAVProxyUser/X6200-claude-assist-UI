/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the C pumps on raw files, for checking them against BinModem's own (interop.sh).
 *      interop tx RATE BITS.txt OUT.f64 FS
 *      interop rx RATE IN.f64 FS          (the bits as 0/1 on stdout)
 *  RATE: v27_2400 v27_4800 v29_7200 v29_9600
 */
#include "pump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rate_t rate_of(const char *s) {
    return !strcmp(s, "v27_2400") ? RATE_2400 : !strcmp(s, "v27_4800") ? RATE_4800 : !strcmp(s, "v29_7200") ? RATE_7200
                                                                                                            : RATE_9600;
}

static void put_bit(void *ctx, int bit) {
    (void)ctx;
    putchar('0' + bit);
}

int main(int argc, char **argv) {
    static pump_t p;
    if (argc < 5)
        return 2;
    double fs = atof(argv[argc - 1]);
    rate_t r  = rate_of(argv[2]);
    if (!strcmp(argv[1], "tx")) {
        FILE *in = fopen(argv[3], "r"), *out = fopen(argv[4], "wb");
        int   c;
        if (!in || !out)
            return 1;
        pump_init(&p, fs, NULL, NULL);
        pump_tx_start(&p, r);
        while ((c = fgetc(in)) == '0' || c == '1')
            pump_tx_push(&p, c == '1');
        pump_tx_stop(&p);
        while (pump_tx_busy(&p)) {
            double x = pump_tx_sample(&p);
            fwrite(&x, sizeof(x), 1, out);
        }
        fclose(in);
        fclose(out);
    } else {
        FILE  *in = fopen(argv[3], "rb");
        double x;
        if (!in)
            return 1;
        pump_init(&p, fs, put_bit, NULL);
        pump_rx_arm(&p, r);
        for (int i = 0; i < (int)(0.2 * fs); i++)
            pump_rx_feed(&p, 0.0);
        while (fread(&x, sizeof(x), 1, in) == 1)
            pump_rx_feed(&p, x);
        for (int i = 0; i < (int)(0.2 * fs); i++)
            pump_rx_feed(&p, 0.0);
        putchar('\n');
        fprintf(stderr, "trained %.1f dB\n", p.rx.trained_snr_db);
        fclose(in);
    }
    return 0;
}
