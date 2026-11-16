/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - dmr.c's test: a repeater's (BS) DMR made here, a CACH and a burst every 30 ms: a group call on TS1
 *  (three voice headers, two voice superframes A-F with the embedded LC in B-E, two terminators) while TS2 is idle
 *  and then has a private call of its own. 4FSK (+-1944/+-648 Hz) through a root raised cosine, as a discriminator
 *  (+-1 for +-5 kHz) at 44.1 kHz, with noise, a DC offset (the channel 300 Hz off) and the symbol clock 20 ppm off.
 *  Every AMBE frame sent must come out, in order, in its slot; each call's IDs from its header and its embedded LC;
 *  the calls end. Then noise alone and an FM tone: no sync.
 *
 *      cc -O2 -o dmr_test dmr_test.c dmr.c -lm && ./dmr_test
 */

#include "dmr.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100.0

static int fails, checks;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) fails++;
    printf("%s %s\n", ok ? "  ok   " : "  WRONG", what);
}

/* ---- The transmitter ---- */

static uint8_t *dibits;
static int      ndib, cap;

static void put_dibit(uint8_t d) {
    if (ndib == cap) dibits = realloc(dibits, (size_t)(cap = cap ? cap * 2 : 4096));
    dibits[ndib++] = d & 3;
}

static void put_bits(const uint8_t *b, int n) {       /* one bit a byte, MSB of each dibit first */
    for (int i = 0; i < n; i += 2) put_dibit((uint8_t)(b[i] << 1 | b[i + 1]));
}

static void put_hex48(uint64_t v) {
    for (int k = 0; k < 24; k++) put_dibit((uint8_t)(v >> (46 - 2 * k)));
}

static void cach(int slot) {
    uint8_t b[24] = {0};

    b[4] = (uint8_t)(slot - 1);         /* TC */
    put_bits(b, 24);
}

static void bits_of(const uint8_t *bytes, int nbits, uint8_t *out) {
    for (int i = 0; i < nbits; i++) out[i] = bytes[i / 8] >> (7 - i % 8) & 1;
}

/* A data burst: its 196 bits, the slot type (colour code 1, data type dt) round the data sync */
static void data_burst(int slot, const uint8_t info[196], int dt) {
    uint8_t st[20] = {0};

    for (int i = 0; i < 4; i++) st[i] = (uint8_t)(1 >> (3 - i) & 1);
    for (int i = 0; i < 4; i++) st[4 + i] = (uint8_t)(dt >> (3 - i) & 1);
    cach(slot);
    put_bits(info, 98);
    put_bits(st, 10);
    put_hex48(0xDFF57D75DF5DULL);
    put_bits(st + 10, 10);
    put_bits(info + 98, 98);
}

static void lc_burst(int slot, const uint8_t lc[9], uint8_t mask, int dt) {
    uint8_t b[12], bits[96], info[196], par[3];

    memcpy(b, lc, 9);
    dmr_rs129(lc, par);
    for (int i = 0; i < 3; i++) b[9 + i] = par[i] ^ mask;
    bits_of(b, 96, bits);
    dmr_bptc_encode(bits, info);
    data_burst(slot, info, dt);
}

static void idle_burst(int slot) {
    uint8_t info[196];

    for (int i = 0; i < 196; i++) info[i] = (uint8_t)(rand() & 1);
    data_burst(slot, info, 9);
}

/* The frames sent, by slot, to be found again */
#define MAXF 256
static uint8_t sent[3][MAXF][9], got[3][MAXF][9];
static int     nsent[3], ngot[3];

/* A voice burst: three random frames; A its sync, B-F the EMB (colour code 1, lcss) and 32 bits of the embedded LC */
static void voice_burst(int slot, int vf, const uint8_t elc[128]) {
    uint8_t fr[3][72], emb[16] = {0};

    for (int f = 0; f < 3; f++) {
        uint8_t *s = sent[slot][nsent[slot]];

        for (int i = 0; i < 9; i++) s[i] = (uint8_t)rand();
        bits_of(s, 72, fr[f]);
        if (nsent[slot] < MAXF - 1) nsent[slot]++;
    }
    cach(slot);
    put_bits(fr[0], 72);
    put_bits(fr[1], 36);
    if (vf == 0) {
        put_hex48(0x755FD7DF75F7ULL);
    } else {
        int lcss = vf == 1 ? 1 : vf == 4 ? 2 : vf < 4 ? 3 : 0;

        emb[3] = 1;                     /* colour code 1 */
        emb[5] = (uint8_t)(lcss >> 1);
        emb[6] = (uint8_t)(lcss & 1);
        put_bits(emb, 8);
        if (vf <= 4) put_bits(elc + (vf - 1) * 32, 32);
        else for (int i = 0; i < 16; i++) put_dibit(0);
        put_bits(emb + 8, 8);
    }
    put_bits(fr[1] + 36, 36);
    put_bits(fr[2], 72);
}

/* ---- The channel ---- */

static double gauss(void) {
    double u = (rand() + 1.0) / (RAND_MAX + 2.0), v = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static double rrc(double t) {
    const double a = 0.2;

    if (fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
    if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a)));
    return (sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - pow(4 * a * t, 2)));
}

/* The dibits as a discriminator at 44.1 kHz: noise sigma, dc, the symbol clock ppm off */
static float *modulate(int *n, double sigma, double dc, double ppm) {
    static const int sym_of[4] = {1, 3, -1, -3};
    double           sr = 4800.0 * (1 + ppm * 1e-6);
    int              m  = (int)((ndib + 10) / sr * RATE + 0.7 * RATE);  /* and 0.7 s of noise after */
    float           *x  = malloc(sizeof(float) * (size_t)m);

    for (int i = 0; i < m; i++) {
        double tau = i * sr / RATE - 5, v = 0;      /* five symbols of lead-in */
        int    k0 = (int)floor(tau) - 5;

        for (int k = k0; k <= k0 + 11; k++)
            if (k >= 0 && k < ndib) v += sym_of[dibits[k]] * rrc(tau - k);
        x[i] = (float)((v * 648 + dc) / 5000 + sigma * gauss());
    }
    *n = m;
    return x;
}

/* ---- The receiver's side ---- */

typedef struct {
    dmr_call_t c[64];
    int        n;
} calls_t;

static void on_ambe(void *ctx, int slot, const uint8_t f[9]) {
    (void)ctx;
    if (slot >= 0 && slot <= 2 && ngot[slot] < MAXF) memcpy(got[slot][ngot[slot]++], f, 9);
}

static void on_call(void *ctx, const dmr_call_t *c) {
    calls_t *cs = ctx;

    if (cs->n < 64) cs->c[cs->n++] = *c;
    printf("    TS%d CC%d %s%s %s %u -> %u%s\n", c->slot, c->cc, c->voice ? "VOICE" : "END", c->bs ? " (BS)" : "",
           c->lc ? (c->flco == 3 ? "PRIVATE" : "GROUP") : "-", c->src, c->dst, c->lc ? "" : " (no LC yet)");
}

static int matching(int slot) {
    int ok = 0;

    for (int i = 0; i < nsent[slot] && i < ngot[slot]; i++) ok += !memcmp(sent[slot][i], got[slot][i], 9);
    return ok;
}

static void run(const char *name, double sigma, double dc, double ppm) {
    static const uint8_t lc1[9] = {0x00, 0x00, 0x00, 0x00, 0x0C, 0x1C, 0x2F, 0x7D, 0x99};   /* group 3100 <- 3112345 */
    static const uint8_t lc2[9] = {0x03, 0x00, 0x00, 0x12, 0xD6, 0x87, 0x74, 0xCB, 0xB1};   /* 1234567 <- 7654321 */
    uint8_t              elc1[128], elc2[128];
    calls_t              cs = {0};
    char                 w[160];

    printf("%s\n", name);
    ndib = 0;
    memset(nsent, 0, sizeof(nsent));
    memset(ngot, 0, sizeof(ngot));
    dmr_elc_encode(lc1, elc1);
    dmr_elc_encode(lc2, elc2);

    /* TS1 and TS2 alternate, each burst 30 ms */
    for (int i = 0; i < 4; i++) idle_burst(1), idle_burst(2);
    for (int i = 0; i < 3; i++) lc_burst(1, lc1, 0x96, 1), idle_burst(2);
    for (int sf = 0; sf < 2; sf++)
        for (int vf = 0; vf < 6; vf++) {
            voice_burst(1, vf, elc1);
            if (sf == 1 && vf == 0) lc_burst(2, lc2, 0x96, 1);
            else if (sf == 1) voice_burst(2, vf - 1, elc2);
            else idle_burst(2);
        }
    /* TS2: F, then a burst where A should be with no sync (an A missed: its voice goes on) */
    for (int i = 0; i < 2; i++) lc_burst(1, lc1, 0x99, 2), voice_burst(2, 5 + i, elc2);
    for (int vf = 0; vf < 6; vf++) idle_burst(1), voice_burst(2, vf, elc2);
    for (int i = 0; i < 2; i++) idle_burst(1), lc_burst(2, lc2, 0x99, 2);
    for (int i = 0; i < 4; i++) idle_burst(1), idle_burst(2);

    int    n;
    float *x = modulate(&n, sigma, dc, ppm);
    dmr_t *d = dmr_new((float)RATE, on_ambe, on_call, &cs);

    for (int i = 0; i < n; i += 441) dmr_feed(d, x + i, (size_t)(n - i < 441 ? n - i : 441));
    uint32_t sy, bu, lcs, elcs;
    dmr_counts(d, &sy, &bu, &lcs, &elcs);
    printf("    %u syncs, %u bursts, %u full LCs, %u embedded LCs; TS1 %d of %d frames, TS2 %d of %d\n", sy, bu, lcs,
           elcs, ngot[1], nsent[1], ngot[2], nsent[2]);

    int m1 = matching(1), m2 = matching(2);
    snprintf(w, sizeof(w), "%s: TS1's %d AMBE frames out as sent, in order (%d right, %d out)", name, nsent[1], m1,
             ngot[1]);
    check(m1 == nsent[1] && ngot[1] == nsent[1], w);
    snprintf(w, sizeof(w), "%s: TS2's %d AMBE frames out as sent, in order (%d right, %d out)", name, nsent[2], m2,
             ngot[2]);
    check(m2 == nsent[2] && ngot[2] == nsent[2], w);
    snprintf(w, sizeof(w), "%s: the 4 headers and 4 terminators passed their RS(12,9), %u", name, lcs);
    check(lcs == 8, w);
    snprintf(w, sizeof(w), "%s: the embedded LCs (2 of TS1's superframes, 2 of TS2's), %u", name, elcs);
    check(elcs == 4, w);

    bool g = false, p = false, g_end = false, p_end = false;
    for (int i = 0; i < cs.n; i++) {
        const dmr_call_t *c = &cs.c[i];

        if (c->slot == 1 && c->voice && c->lc && c->flco == 0 && c->dst == 3100 && c->src == 3112345 && c->cc == 1 &&
            c->bs)
            g = true;
        if (c->slot == 2 && c->voice && c->lc && c->flco == 3 && c->dst == 1234567 && c->src == 7654321) p = true;
        if (c->slot == 1 && !c->voice && g) g_end = true;
        if (c->slot == 2 && !c->voice && p) p_end = true;
    }
    snprintf(w, sizeof(w), "%s: TS1's group call to 3100 from 3112345, colour code 1, a repeater's", name);
    check(g, w);
    snprintf(w, sizeof(w), "%s: TS2's private call to 1234567 from 7654321", name);
    check(p, w);
    snprintf(w, sizeof(w), "%s: both calls ended", name);
    check(g_end && p_end, w);
    snprintf(w, sizeof(w), "%s: not DMR any more half a second after", name);
    check(!dmr_active(d), w);
    dmr_free(d);
    free(x);
}

/* FM noise with no carrier, as the daemon makes it: complex noise at 264.6 kHz, its 9 kHz channel filter (63 taps,
 * Blackman windowed sinc), every sixth sample, the discriminator (+-1 for +-5 kHz). All clicks: it matched a sync's 24
 * signs every minute or two on the radio before the sync had to have its sizes alike and a second on its grid */
static float fm_noise(void) {
    static float  taps[63], hr[126], hi[126], pr = 1, pi = 0;
    static int    at, made;

    if (!made) {
        float sum = 0, fc = 9000.0f / 264600.0f;

        for (int i = 0; i < 63; i++) {
            int   m = i - 31;
            float s = m ? sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m) : 2 * fc;
            float w = 0.42f - 0.5f * cosf(2 * (float)M_PI * i / 62) + 0.08f * cosf(4 * (float)M_PI * i / 62);
            sum += taps[i] = s * w;
        }
        for (int i = 0; i < 63; i++) taps[i] /= sum;
        made = 1;
    }
    for (int k = 0; k < 6; k++) {
        hr[at] = hr[at + 63] = (float)gauss();
        hi[at] = hi[at + 63] = (float)gauss();
        at = at + 1 == 63 ? 0 : at + 1;
    }
    float yr = 0, yi = 0;

    for (int k = 0; k < 63; k++) yr += hr[at + k] * taps[k], yi += hi[at + k] * taps[k];
    float zr = yr * pr + yi * pi, zi = yi * pr - yr * pi;

    pr = yr;
    pi = yi;
    return atan2f(zi, zr) * (float)(RATE / (2 * M_PI * 5000));
}

static void no_dmr(const char *name, int tone) {
    calls_t cs = {0};
    dmr_t  *d  = dmr_new((float)RATE, on_ambe, on_call, &cs);
    float   x[441];
    double  ph = 0;
    char    w[128];

    memset(ngot, 0, sizeof(ngot));
    for (int b = 0; b < (tone == 2 ? 30000 : 3000); b++) {      /* 30 s; FM noise 5 minutes */
        for (int i = 0; i < 441; i++) {
            ph += 2 * M_PI * 1000 / RATE;
            x[i] = tone == 1 ? (float)(0.6 * sin(ph) + 0.02 * gauss()) : tone == 2 ? fm_noise() : (float)(0.5 * gauss());
        }
        dmr_feed(d, x, 441);
    }
    uint32_t sy;
    dmr_counts(d, &sy, NULL, NULL, NULL);
    snprintf(w, sizeof(w), "%s, %d s: no sync (%u), no call (%d), no frame (%d)", name, tone == 2 ? 300 : 30, sy, cs.n,
             ngot[1] + ngot[2]);
    check(sy == 0 && cs.n == 0 && ngot[1] + ngot[2] == 0, w);
    dmr_free(d);
}

int main(void) {
    srand(1);
    run("clean", 0.0, 0, 0);
    run("noisy (SNR about 20 dB), 300 Hz off, clock 20 ppm fast", 0.02, 300, 20);
    run("noisier (about 14 dB), 300 Hz off, clock 20 ppm slow", 0.04, -300, -20);
    no_dmr("noise alone", 0);
    no_dmr("an FM tone (1 kHz, 3 kHz deviation)", 1);
    no_dmr("FM noise with no carrier (the channel's filter and discriminator)", 2);
    printf("%d of %d right\n", checks - fails, checks);
    return fails != 0;
}
