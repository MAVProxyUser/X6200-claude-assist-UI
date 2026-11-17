/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - DMR AMBE+2 frames (72 bits as sent, 9 bytes each: what the USB-3000 gives at rate 33) to the 49 voice
 *  bits in dsd-fme's order (ambe_d: through DMR's de-interleave and libmbe-dvsi's Golay and scrambling), one byte a bit:
 *  the voice for a YSF V/D type 2 or P25 phase 2 test transmission, which carry those 49 bits each in its own way.
 *
 *      dmr_to_ambed frames.ambe > voice.ambed
 *  Built on the Mac: cc -O2 -o build/dmr_to_ambed tools/dsdfme/ysf/dmr_to_ambed.c tools/dsdfme/mbe_dvsi/mbe_dvsi.c
 */
#include "../mbe_dvsi/mbelib.h"

#include <stdio.h>

/* DMR's AMBE de-interleave (dsd-fme's dmr_const.h, as mbe_dvsi_test.c has it) */
static const int rW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};
static const int rX[36] = {23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4};
static const int rY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};
static const int rZ[36] = {5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0};

int main(int argc, char **argv) {
    FILE         *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    unsigned char b[9];
    int           n = 0, bad = 0;

    if (!f) return 2;
    while (fread(b, 1, 9, f) == 9) {
        char fr[4][24] = {{0}}, d[49];

        for (int i = 0; i < 36; i++) {
            int b1 = b[(2 * i) / 8] >> (7 - (2 * i) % 8) & 1, b0 = b[(2 * i + 1) / 8] >> (7 - (2 * i + 1) % 8) & 1;

            fr[rW[i]][rX[i]] = (char)b1;
            fr[rY[i]][rZ[i]] = (char)b0;
        }
        bad += mbe_eccAmbe3600x2450C0(fr) > 0;
        mbe_demodulateAmbe3600x2450Data(fr);
        bad += mbe_eccAmbe3600x2450Data(fr, d) > 0;
        fwrite(d, 1, 49, stdout);
        n++;
    }
    fprintf(stderr, "dmr_to_ambed: %d frames (%d with a corrected bit: the chip's own should have none)\n", n, bad);
    return 0;
}
