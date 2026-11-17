/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - a P25 phase 1 voice call as dibits, for cabled tests (a HackRF through an attenuator into the RTL
 *  stick, never over the air): header (HDU), LDU1/LDU2 pairs carrying IMBE frames, terminator (TDU). Built against
 *  dsd-fme's own encoders (its Hamming (10,6,3), Reed-Solomon (24,12,13), (24,16,9), (36,20,17) and Golay (24,6)
 *  classes, ISC), so each code is laid out as its decoder reads it; the NID's BCH (63,16) made here and checked by
 *  its syndromes. Status symbols after every 35 dibits from each frame sync, as dsd-fme skips them.
 *
 *  The IMBE frames are 18 bytes each, the 144 bits as P25 sends them (an AMBE-3000 in its P25 full-rate mode gives
 *  them so), cycled over the call.
 *
 *      p25_gen FRAMES.bin SUPERFRAMES [NAC TG SRC] > call.dibits      (one byte a dibit, 0-3)
 *
 *  Built on the Mac: c++ -O2 -I build/x6200/dsdfme/src/include tools/dsdfme/p25_gen.cpp
 *                    build/x6200/dsdfme/src/src/Hamming.cpp -o build/p25_gen
 */
#define register                        /* deprecated in C++11, in dsd-fme's ReedSolomon.hpp: as its sources do */
#include "Golay24.hpp"
#include "Hamming.hpp"
#include "ReedSolomon.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<uint8_t> out;
static int                  since;      /* data dibits since the frame sync or the last status symbol */

static void dibit(int d) {
    out.push_back((uint8_t)(d & 3));
    if (++since == 35) {                /* a status symbol: "01", a subscriber's idle (dsd-fme reads past it) */
        out.push_back(1);
        since = 0;
    }
}

static void bits(const char *b, int n) {       /* MSB first, two at a time */
    for (int i = 0; i < n; i += 2) dibit((b[i] & 1) << 1 | (b[i + 1] & 1));
}

static void value(uint64_t v, int n) {
    char b[64];

    for (int i = 0; i < n; i++) b[i] = (char)((v >> (n - 1 - i)) & 1);
    bits(b, n);
}

/* ---- the NID: NAC 12 bits, DUID 4, BCH (63,16) parity 47, a parity bit. GF(64) of x^6 + x + 1 (dsd-fme's
 * bchGFexp), the generator the product of the minimal polynomials of a^1 .. a^22 ---- */
static int      gexp[63], glog[64];
static uint64_t bch_g;                  /* the generator, bit i = x^i */

static void bch_init(void) {
    int x = 1;

    for (int i = 0; i < 63; i++) {
        gexp[i] = x;
        glog[x] = i;
        x <<= 1;
        if (x & 64) x ^= 0x43;
    }
    bool done[63] = {false};
    std::vector<int> g = {1};           /* coefficients over GF(64), low first */

    for (int i = 1; i <= 22; i++) {
        if (done[i]) continue;
        int c = i;

        do {                            /* the coset of i: multiply in (x + a^c) for each member */
            done[c] = true;
            std::vector<int> ng(g.size() + 1, 0);

            for (size_t k = 0; k < g.size(); k++) {
                ng[k + 1] ^= g[k];
                if (g[k]) ng[k] ^= gexp[(glog[g[k]] + c) % 63];
            }
            g = ng;
            c = (c * 2) % 63;
        } while (c != i);
    }
    bch_g = 0;
    for (size_t k = 0; k < g.size(); k++) {
        if (g[k] > 1) {
            fprintf(stderr, "p25_gen: the BCH generator is not binary\n");
            exit(1);
        }
        if (g[k]) bch_g |= 1ull << k;
    }
    if (g.size() != 48) {
        fprintf(stderr, "p25_gen: the BCH generator has degree %zu, not 47\n", g.size() - 1);
        exit(1);
    }
}

static void nid(int nac, int duid) {
    uint64_t d = (uint64_t)(nac << 4 | duid), r = d << 47;

    for (int i = 62; i >= 47; i--)
        if (r >> i & 1) r ^= bch_g << (i - 47);
    uint64_t cw = d << 47 | r;          /* bit i = x^i: sent from x^62 down */

    for (int s = 1; s <= 22; s++) {    /* its syndromes, as dsd-fme's bchDec takes them: none */
        int v = 0;

        for (int j = 0; j < 63; j++)
            if (cw >> j & 1) v ^= gexp[(s * j) % 63];
        if (v) {
            fprintf(stderr, "p25_gen: NID syndrome %d is %d\n", s, v);
            exit(1);
        }
    }
    value(cw >> 1, 62);                 /* 31 dibits; the 63rd bit and the parity in the 32nd */
    dibit((int)(cw & 1) << 1 | (duid == 5 || duid == 10));
}

static void sync(int nac, int duid) {
    since = 0;
    value(0x5575F5FF77FFull, 48);
    nid(nac, duid);
}

static void pad_to_status(void) {       /* zeros to the next status symbol: each unit ends on one */
    while (since) dibit(0);
}

static void hexwords(const char *hex, int from, int to, char *parity_of, int kind) {
    (void)parity_of;
    for (int i = from; i >= to; i--) {
        bits(hex + i * 6, 6);
        if (kind == 10) {
            static Hamming_10_6_3_TableImpl h;
            char p[4];

            h.encode((char *)hex + i * 6, p);
            bits(p, 4);
        } else {
            static DSDGolay24 g;
            char p[12];

            g.encode_6((char *)hex + i * 6, p);
            bits(p, 12);
        }
    }
}

static void to_hex(const char *b, int nbits, char *hex, int nwords) {     /* bits MSB first -> hex[nwords-1] first */
    for (int i = 0; i < nbits; i++) hex[(nwords - 1 - i / 6) * 6 + i % 6] = b[i];
}

static void field(char *b, int &at, uint64_t v, int n) {
    for (int i = 0; i < n; i++) b[at++] = (char)((v >> (n - 1 - i)) & 1);
}

static std::vector<uint8_t> frames;
static size_t               fi;

static void imbe(void) {
    const uint8_t *f = &frames[(fi++ % (frames.size() / 18)) * 18];
    char           b[144];

    for (int i = 0; i < 144; i++) b[i] = (char)((f[i / 8] >> (7 - i % 8)) & 1);
    bits(b, 144);
}

static void lsd(void) {                 /* two bytes of low speed data, zeros, each with its (16,8) parity: zeros */
    value(0, 32);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    int   sfs = atoi(argv[2]), nac = argc > 3 ? (int)strtol(argv[3], NULL, 16) : 0x123;
    int   tg = argc > 4 ? atoi(argv[4]) : 4321, src = argc > 5 ? atoi(argv[5]) : 1234567;
    int   c;

    if (!f) return 2;
    while ((c = fgetc(f)) != EOF) frames.push_back((uint8_t)c);
    if (frames.size() < 18) return 2;
    bch_init();

    /* HDU: MI 72, MFID 8, ALGID 8 (0x80: clear), KID 16, TGID 16 -> 20 hex words, RS (36,20,17), each Golay (24,6) */
    {
        char b[120], hex[20 * 6], par[16 * 6];
        int  at = 0;
        static DSDReedSolomon_36_20_17 rs;

        field(b, at, 0, 72);
        field(b, at, 0, 8);
        field(b, at, 0x80, 8);
        field(b, at, 0, 16);
        field(b, at, (uint64_t)tg, 16);
        to_hex(b, 120, hex, 20);
        rs.encode(hex, par);
        sync(nac, 0);
        hexwords(hex, 19, 0, NULL, 24);
        hexwords(par, 15, 0, NULL, 24);
        pad_to_status();
    }
    for (int s = 0; s < sfs; s++) {
        /* LDU1: the LC (group voice: LCF 0, MFID 0, service options 0, reserved, TG 16, source 24), RS (24,12,13) */
        {
            char b[72], hex[12 * 6], par[12 * 6];
            int  at = 0;
            static DSDReedSolomon_24_12_13 rs;

            field(b, at, 0, 8);
            field(b, at, 0, 8);
            field(b, at, 0, 8);
            field(b, at, 0, 8);
            field(b, at, (uint64_t)tg, 16);
            field(b, at, (uint64_t)src, 24);
            to_hex(b, 72, hex, 12);
            rs.encode(hex, par);
            sync(nac, 5);
            imbe(), imbe();
            hexwords(hex, 11, 8, NULL, 10), imbe();
            hexwords(hex, 7, 4, NULL, 10), imbe();
            hexwords(hex, 3, 0, NULL, 10), imbe();
            hexwords(par, 11, 8, NULL, 10), imbe();
            hexwords(par, 7, 4, NULL, 10), imbe();
            hexwords(par, 3, 0, NULL, 10), imbe();
            lsd(), imbe();
            pad_to_status();
        }
        /* LDU2: the ES (MI 72, ALGID 8 = 0x80, KID 16), RS (24,16,9) */
        {
            char b[96], hex[16 * 6], par[8 * 6];
            int  at = 0;
            static DSDReedSolomon_24_16_9 rs;

            field(b, at, 0, 72);
            field(b, at, 0x80, 8);
            field(b, at, 0, 16);
            to_hex(b, 96, hex, 16);
            rs.encode(hex, par);
            sync(nac, 10);
            imbe(), imbe();
            hexwords(hex, 15, 12, NULL, 10), imbe();
            hexwords(hex, 11, 8, NULL, 10), imbe();
            hexwords(hex, 7, 4, NULL, 10), imbe();
            hexwords(hex, 3, 0, NULL, 10), imbe();
            hexwords(par, 7, 4, NULL, 10), imbe();
            hexwords(par, 3, 0, NULL, 10), imbe();
            lsd(), imbe();
            pad_to_status();
        }
    }
    sync(nac, 3);                       /* TDU */
    pad_to_status();
    fwrite(out.data(), 1, out.size(), stdout);
    fprintf(stderr, "p25_gen: %zu dibits (%.2f s), %zu IMBE frames\n", out.size(), out.size() / 4800.0, fi);
    return 0;
}
