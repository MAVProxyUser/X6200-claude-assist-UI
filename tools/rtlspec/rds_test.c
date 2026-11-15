/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - rds.c on a made multiplex: the 0A and 2A groups of a station "WOSU" (PI 0x7B88), PTY 10, name
 *  "TESTFM12" and a RadioText, at 4 % injection under mono audio, the 19 kHz pilot, a stereo difference signal, an
 *  SCA subcarrier at 67 kHz and noise, sampled 40 ppm off. Off the radio:
 *      cc -O2 -o /tmp/rds_test tools/rtlspec/rds_test.c -lm && /tmp/rds_test [noise] [ppm]
 */

#include "rds.c"

#include <stdio.h>

#define RATE 264600.0

static const char *PS = "TESTFM12", *RT = "HELLO FROM THE RDS TEST\r";

static uint32_t block(uint16_t info, int kind) {
    return ((uint32_t)info << 10) | (crc10(info) ^ OFFSET[kind]);
}

/* the groups: 0A with the name's four parts, then 2A with the text, round and round */
static int     bits[30000];
static size_t  nbits;

static void put_group(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    uint32_t w[4] = {block(a, 0), block(b, 1), block(c, 2), block(d, 4)};

    for (int k = 0; k < 4; k++)
        for (int i = 25; i >= 0; i--) bits[nbits++] = (w[k] >> i) & 1;
}

static double gauss(void) {
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

int main(int argc, char **argv) {
    double     noise = argc > 1 ? atof(argv[1]) : 0.02, ppm = argc > 2 ? atof(argv[2]) : 40;
    uint16_t   pi = 0x7B88, pty = 10;
    int        rtlen = (int)strlen(RT);
    rds_t     *r = rds_new(RATE);
    rds_info_t in;
    char       call[5];

    while (nbits + 2000 < sizeof(bits) / sizeof(bits[0])) {     /* a round of groups is ~1000 bits */
        for (int s = 0; s < 4; s++)
            put_group(pi, (0 << 12) | (pty << 5) | s, 0xE0CD, (uint16_t)(PS[s * 2] << 8 | PS[s * 2 + 1]));
        for (int s = 0; s * 4 < rtlen; s++) {
            uint8_t ch[4];

            for (int i = 0; i < 4; i++) ch[i] = s * 4 + i < rtlen ? (uint8_t)RT[s * 4 + i] : ' ';
            put_group(pi, (2 << 12) | (pty << 5) | s, (uint16_t)(ch[0] << 8 | ch[1]), (uint16_t)(ch[2] << 8 | ch[3]));
        }
    }

    /* differential, biphase, on 57 kHz three times the pilot's phase; 12 s */
    size_t n = (size_t)(RATE * 12);
    float  buf[441];
    int    k = 0, prev = 0, ok = 0;
    double sca_ph = 0;

    srand(1);
    for (size_t i = 0; i < n; i++) {
        double t = i / RATE * (1 + ppm * 1e-6);            /* the stick's clock off */
        double chips = t * 2375;
        size_t ci = (size_t)chips, bi = ci / 2;
        if (bi >= nbits) break;
        static size_t last_bi = (size_t)-1;
        static int    d = 0;
        if (bi != last_bi) {
            d = bits[bi] ^ prev;
            prev = d;
            last_bi = bi;
        }
        double sym = (ci & 1) ? (d ? -1 : 1) : (d ? 1 : -1);
        double pilot = 2 * M_PI * 19000 * t;
        double mono = 0.35 * sin(2 * M_PI * 1000 * t) + 0.1 * sin(2 * M_PI * 3100 * t) + 0.05 * sin(2 * M_PI * 9000 * t);
        double diff = 0.3 * sin(2 * M_PI * 700 * t) + 0.1 * sin(2 * M_PI * 4500 * t);
        sca_ph += 2 * M_PI * (67000 + 6000 * sin(2 * M_PI * 2000 * t)) / RATE;
        double x = mono + 0.09 * sin(pilot) + diff * sin(2 * pilot) + 0.04 * sym * sin(3 * pilot + 0.7) +
                   0.1 * cos(sca_ph) + noise * gauss();

        buf[k++] = (float)x;
        if (k == 441) {
            k = 0;
            if (rds_feed(r, buf, 441)) {
                rds_info(r, &in);
                rds_call_letters(in.pi, call);
                printf("%6.2f s  PI %04X %s  PTY %d %s  PS \"%s\"  RT \"%s\"  groups %u  blocks %u bad %u\n", t, in.pi,
                       call, in.pty, rds_pty_name(in.pty, true), in.ps, in.rt, in.groups, in.blocks, in.bad);
            }
        }
    }
    rds_info(r, &in);
    rds_call_letters(in.pi, call);
    ok = in.pi == pi && !strcmp(call, "WOSU") && in.pty == 10 && !strcmp(in.ps, PS) &&
         !strcmp(in.rt, "HELLO FROM THE RDS TEST");
    printf("%s: PI %04X %s, PTY %s, PS \"%s\", RT \"%s\", %u groups, %u of %u blocks bad\n", ok ? "PASS" : "FAIL",
           in.pi, call, rds_pty_name(in.pty, true), in.ps, in.rt, in.groups, in.bad, in.blocks);
    rds_free(r);
    return ok ? 0 : 1;
}
