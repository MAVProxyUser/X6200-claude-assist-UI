/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - libmbe-dvsi's test on the radio: DMR AMBE+2 frames (72 bits as sent, from a file) through dsd-fme's
 *  path (DMR's de-interleave into ambe_fr, then this library's error correction and the chip), clean and with up to
 *  three bit errors in each of C0 and C1: the same speech out of both (the codes put them right).
 *
 *      mbe_dvsi_test frames.ambe out_clean.s16 out_errors.s16
 */
#include "mbelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int rW[36] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2};
static const int rX[36] = {23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5, 17, 4, 16, 3, 15, 2, 14, 1, 13, 0, 12, 10, 11, 9, 10, 8, 9, 7, 8, 6, 7, 5, 6, 4};
static const int rY[36] = {0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3};
static const int rZ[36] = {5, 3, 4, 2, 3, 1, 2, 0, 1, 13, 0, 12, 22, 11, 21, 10, 20, 9, 19, 8, 18, 7, 17, 6, 16, 5, 15, 4, 14, 3, 13, 2, 12, 1, 11, 0};

int main(int argc, char **argv) {
    FILE         *f = fopen(argv[1], "rb");
    unsigned char b[9];
    int           n = 0, fixed = 0;
    mbe_parms     cur, prev, enh;

    if (argc < 4 || !f) return 2;
    FILE *o1 = fopen(argv[2], "wb"), *o2 = fopen(argv[3], "wb");
    srand(5);
    for (int pass = 0; pass < 2; pass++) {      /* each pass the decoder from fresh: the same speech out of both */
        rewind(f);
        n = 0;
        mbe_initMbeParms(&cur, &prev, &enh);
        while (fread(b, 9, 1, f) == 1) {
            char  fr[4][24], d[49];
            float out[160];
            short s[160];
            int   errs, errs2;

            memset(fr, 0, sizeof(fr));
            for (int i = 0; i < 36; i++) {
                int di = b[i / 4] >> (6 - 2 * (i % 4)) & 3;

                fr[rW[i]][rX[i]] = (char)(di >> 1);
                fr[rY[i]][rZ[i]] = (char)(di & 1);
            }
            if (pass) {                 /* up to three errors in C0 (bits 1-23) and in C1 (0-22, before descrambling) */
                int k0 = rand() % 4, k1 = rand() % 4;

                for (int k = 0; k < k0; k++) fr[0][1 + rand() % 23] ^= 1;
                for (int k = 0; k < k1; k++) fr[1][rand() % 23] ^= 1;
            }
            errs = mbe_eccAmbe3600x2450C0(fr);
            mbe_demodulateAmbe3600x2450Data(fr);
            errs2 = errs + mbe_eccAmbe3600x2450Data(fr, d);
            if (pass) fixed += errs2;
            mbe_processAmbe2450Dataf(out, &errs, &errs2, NULL, d, &cur, &prev, &enh, 3);
            mbe_floattoshort(out, s);
            fwrite(s, 2, 160, pass ? o2 : o1);
            n++;
        }
    }
    fclose(o1);
    fclose(o2);
    printf("%d frames; %d bit errors put right in the second pass\n", n, fixed);
    return 0;
}
