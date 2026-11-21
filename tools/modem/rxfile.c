/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: what V.21 makes of a recording (int16, 24 kHz, as x6x00-modem --record writes it): the bits of each
 *  stretch with energy, the HDLC flags found in them, and any frame.  rxfile FILE [offset_hz]
 */
#include "hdlc.h"
#include "v21.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    FILE     *f   = fopen(argv[1], "rb");
    double    off = argc > 2 ? atof(argv[2]) : 0.0, ph = 0.0;
    v21_rx_t  rx;
    hdlc_rx_t h  = {0};
    int16_t   s;
    long      n  = 0, nb = 0;
    char      bits[4096];
    onepole_t lev;
    uint8_t   fr[1100];

    if (!f)
        return 1;
    v21_rx_init(&rx, 24000.0);
    onepole_init(&lev, 0.02, 24000.0);
    /* an offset to take out: the line shifted by -off with a Hilbert pair would be right; a plain mix is enough
     * to see whether the tones decode once moved */
    (void)ph;
    while (fread(&s, 2, 1, f) == 1) {
        double x = s / 32768.0;
        double l = onepole_run(&lev, fabs(x));
        int    b = v21_rx_feed(&rx, x);
        n++;
        if (b >= 0 && l > 0.06) {
            if (nb < (long)sizeof(bits) - 1)
                bits[nb++] = '0' + b;
            int k = hdlc_rx_bit(&h, b, fr, sizeof(fr));
            if (k > 0) {
                printf("%.2f s: frame of %d bytes:", n / 24000.0, k);
                for (int i = 0; i < k; i++)
                    printf(" %02x", fr[i]);
                printf("\n");
            }
        } else if (nb > 0 && l <= 0.06) {
            bits[nb] = 0;
            printf("%.2f s: %ld bits, flags back to back at most %d: %.*s%s\n", n / 24000.0, nb, h.run,
                   nb > 200 ? 200 : (int)nb, bits, nb > 200 ? "..." : "");
            nb = 0;
        }
    }
    printf("frames good %u bad %u (offset %.0f Hz not taken out)\n", h.good, h.bad, off);
    return 0;
}
