/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - libmbe-dvsi's P25 test, on the Mac or in CI: IMBE frames as an AMBE-3000 sent them in its P25
 *  full-rate mode (144 bits each, P25's order) through dsd-fme's path (sorted into imbe_fr, this library's Golay,
 *  scrambling and Hamming correction into the 88 voice bits), then coded again as the library hands them to the chip:
 *  the same 144 bits back, clean and with errors in each codeword the codes can put right.
 *
 *      p25_test frames.bin
 */
#include "mbelib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mbe_dvsi_imbe_encode(const char imbe_d[88], unsigned char b[18]);
void mbe_dvsi_imbe_sort(const unsigned char b[18], char imbe_fr[8][23]);

int main(int argc, char **argv) {
    FILE         *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    unsigned char b[18], b2[18];
    int           n = 0, same = 0, fixed = 0, clean_errs = 0;

    if (!f) return 2;
    srand(7);
    while (fread(b, 1, 18, f) == 18) {
        for (int pass = 0; pass < 2; pass++) {
            char fr[8][23], d[88];

            mbe_dvsi_imbe_sort(b, fr);
            if (pass) {                 /* errors: three in each Golay codeword, one in each Hamming one */
                for (int i = 0; i < 4; i++)
                    for (int e = 0; e < 3; e++) fr[i][(rand() % 23 + e * 7) % 23] ^= 1;
                for (int i = 4; i < 7; i++) fr[i][rand() % 15] ^= 1;
            }
            int e0 = mbe_eccImbe7200x4400C0(fr);

            mbe_demodulateImbe7200x4400Data(fr);
            int e1 = mbe_eccImbe7200x4400Data(fr, d);

            mbe_dvsi_imbe_encode(d, b2);
            if (!pass) {
                clean_errs += e0 + e1;
                same += !memcmp(b, b2, 18);
            } else {
                fixed += !memcmp(b, b2, 18);
            }
        }
        n++;
    }
    printf("p25_test: %d bits 'corrected' in clean frames; with errors %d of %d put right; %d of %d coded back the same\n",
           clean_errs, fixed, n, same, n);
    return n && same == n && clean_errs == 0 && fixed == n ? 0 : 1;
}
