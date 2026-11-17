/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - lrpt.c's test, with a transmitter of its own for each layer: pictures coded into MCUs as MSU-MR does
 *  (8x8 DCT, the quality's quantization, the standard Huffman tables), packed into imager packets (APID 64, 65, 68,
 *  14 a strip each, and one of APID 70), the packets into VCDUs, those into RS codewords (interleave 4, dual basis),
 *  scrambled, the sync word before, convolutionally coded; then differentially coded, interleaved with markers (80k),
 *  and as soft bits or as a QPSK / offset QPSK signal (a root raised cosine, the carrier off, the clock off, noise).
 *  Each layer checked on its own (RS to 16 errors, the scrambler's first bytes, the MCUs), then the whole: every
 *  channel's picture as the packets' MCUs decode, under each of the 8 turns of the constellation, the 80k mode, a
 *  frame lost on the way, and through the demodulator.
 *
 *      cc -O2 -o lrpt_test lrpt_test.c lrpt.c -lm && ./lrpt_test
 */

#include "lrpt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;

static void check(int ok, const char *what) {
    checks++;
    if (!ok) fails++;
    printf("%s %s\n", ok ? "  ok   " : "  WRONG", what);
}

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static float gauss(void) {
    float u = (rnd() + 1.0f) / 4294967297.0f, v = (rnd() + 1.0f) / 4294967297.0f;
    return sqrtf(-2 * logf(u)) * cosf(2 * (float)M_PI * v);
}

/* ---- A growing byte / bit buffer ---- */
typedef struct {
    uint8_t *b;
    size_t   n, cap;
} buf_t;
static void put(buf_t *b, uint8_t v) {
    if (b->n == b->cap) b->b = realloc(b->b, b->cap = b->cap ? b->cap * 2 : 65536);
    b->b[b->n++] = v;
}

/* ---- JPEG, as MSU-MR codes its MCUs ---- */
typedef struct {
    uint16_t code[256];
    uint8_t  len[256];
} henc_t;
static henc_t hdc, hac;

static void henc_build(henc_t *h, const uint8_t bits[16], const uint8_t *vals) {
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            h->code[vals[k]] = (uint16_t)code++;
            h->len[vals[k++]] = (uint8_t)len;
        }
        code <<= 1;
    }
}

typedef struct {
    buf_t   *b;
    uint32_t acc;
    int      n;
} bw_t;
static void bw_put(bw_t *w, uint32_t v, int len) {
    for (int i = len - 1; i >= 0; i--) {
        w->acc = w->acc << 1 | ((v >> i) & 1);
        if (++w->n == 8) {
            put(w->b, (uint8_t)w->acc);
            w->acc = 0;
            w->n   = 0;
        }
    }
}
static void bw_flush(bw_t *w) {
    while (w->n) bw_put(w, 1, 1);
}
static int cat(int v) {
    int a = v < 0 ? -v : v, s = 0;
    while (a) a >>= 1, s++;
    return s;
}
static void put_val(bw_t *w, int v, int s) { bw_put(w, (uint32_t)(v < 0 ? v + (1 << s) - 1 : v), s); }

/* 14 MCUs of the strip px (8 rows of 112) at quality q */
static void jpeg_mcus(const uint8_t px[8][112], int q, buf_t *out) {
    int  qt[64], prev = 0;
    bw_t w = {out, 0, 0};

    lrpt_quant(q, qt);
    for (int m = 0; m < 14; m++) {
        int co[64];
        for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++) {
                double a = 0;
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++)
                        a += (px[y][m * 8 + x] - 128.0) * cos((2 * x + 1) * u * M_PI / 16) * cos((2 * y + 1) * v * M_PI / 16);
                a *= (u ? 1 : M_SQRT1_2) * (v ? 1 : M_SQRT1_2) / 4;
                co[v * 8 + u] = (int)lrint(a / qt[v * 8 + u]);
            }
        int d = co[0] - prev, s = cat(d);
        prev = co[0];
        bw_put(&w, hdc.code[s], hdc.len[s]);
        if (s) put_val(&w, d, s);
        int run = 0;
        for (int k = 1; k < 64; k++) {
            int v = co[lrpt_zigzag[k]];
            if (!v) {
                run++;
                continue;
            }
            while (run > 15) {
                bw_put(&w, hac.code[0xf0], hac.len[0xf0]);
                run -= 16;
            }
            s = cat(v);
            bw_put(&w, hac.code[run << 4 | s], hac.len[run << 4 | s]);
            put_val(&w, v, s);
            run = 0;
        }
        if (run) bw_put(&w, hac.code[0], hac.len[0]);
    }
    bw_flush(&w);
}

/* ---- The pictures sent, and what the decoder should make of them ---- */
#define STRIPS 24
static const int apids[3] = {64, 65, 68};
static uint8_t   src[3][STRIPS * 8][LRPT_W];        /* the pictures */
static uint8_t   want[3][STRIPS * 8][LRPT_W];       /* their MCUs decoded */

static void make_pictures(void) {
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < STRIPS * 8; y++)
            for (int x = 0; x < LRPT_W; x++) {
                double v = 128 + 60 * sin(x / (40.0 + 25 * c)) * cos(y / (17.0 + 9 * c)) + (x * 50 / LRPT_W) - 25;
                if ((x / 64 + y / 32 + c) % 5 == 0) v = 230 - c * 40;
                src[c][y][x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
}

/* ---- Packets into VCDUs, those into CADUs ---- */
static buf_t    zone;           /* the packet stream for VCID 5 */
static int      seqc;
static buf_t    cadus;          /* 1024 a frame, as sent (scrambled, sync word first) */
static int      frames_made;

/* A packet of the strip s: its time the strip's (the day, ms of it, us), 1.232 s a strip as the satellite's */
static void packet(int apid, int strip, const uint8_t *data, int n) {
    uint32_t ms = 49138002u + (uint32_t)strip * 1232u, us = (uint32_t)(strip * 97) % 1000;
    int len = 8 + n - 1;        /* the time, then the data */

    put(&zone, (uint8_t)(0x08 | (apid >> 8)));
    put(&zone, (uint8_t)apid);
    put(&zone, (uint8_t)(0xc0 | (seqc >> 8 & 0x3f)));
    put(&zone, (uint8_t)seqc);
    put(&zone, (uint8_t)(len >> 8));
    put(&zone, (uint8_t)len);
    seqc = (seqc + 1) & 0x3fff;
    put(&zone, 0x1a), put(&zone, 0xe1);
    for (int i = 3; i >= 0; i--) put(&zone, (uint8_t)(ms >> (8 * i)));
    put(&zone, (uint8_t)(us >> 8)), put(&zone, (uint8_t)us);
    for (int i = 0; i < n; i++) put(&zone, data[i]);
}

static void make_packets(void) {
    buf_t j = {0};

    for (int s = 0; s < STRIPS; s++) {
        for (int c = 0; c < 3; c++) {
            int q = 70 + 10 * c;
            for (int k = 0; k < 14; k++) {
                uint8_t px[8][112];
                for (int y = 0; y < 8; y++) memcpy(px[y], &src[c][s * 8 + y][k * 112], 112);
                j.n = 0;
                put(&j, (uint8_t)(k * 14));
                put(&j, 0), put(&j, 0), put(&j, 0), put(&j, 0);
                put(&j, (uint8_t)q);
                jpeg_mcus(px, q, &j);
                packet(apids[c], s, j.b, (int)j.n);
                /* what the decoder will make of it */
                uint8_t got[8][112];
                memset(got, 0, sizeof(got));
                lrpt_mcus(j.b + 6, j.n - 6, q, got);
                for (int y = 0; y < 8; y++) memcpy(&want[c][s * 8 + y][k * 112], got[y], 112);
            }
        }
        uint8_t tlm[40] = {0};
        packet(70, s, tlm, sizeof(tlm));
    }
    free(j.b);
}

static void cadu_from(const uint8_t vcdu[892]) {
    uint8_t f[1024], par[32], cw[223];

    f[0] = 0x1a, f[1] = 0xcf, f[2] = 0xfc, f[3] = 0x1d;
    for (int k = 0; k < 4; k++) {
        for (int j = 0; j < 223; j++) cw[j] = vcdu[4 * j + k];
        lrpt_rs_encode(cw, par, false);
        for (int j = 0; j < 223; j++) f[4 + 4 * j + k] = cw[j];
        for (int j = 0; j < 32; j++) f[4 + 4 * (223 + j) + k] = par[j];
    }
    lrpt_pn(f + 4, 1020);
    for (int i = 0; i < 1024; i++) put(&cadus, f[i]);
    frames_made++;
}

static void make_cadus(int pad_fill) {
    uint8_t  v[892];
    uint32_t cnt = 1000, fcnt = 0;
    size_t   at = 0;

    for (int i = 0; i < pad_fill; i++) {         /* fill before: the receiver settles */
        memset(v, 0, sizeof(v));
        v[0] = 0x40 | (0x9e >> 2), v[1] = (uint8_t)((0x9e & 3) << 6 | 63);
        v[2] = (uint8_t)(fcnt >> 16), v[3] = (uint8_t)(fcnt >> 8), v[4] = (uint8_t)fcnt++;
        cadu_from(v);
    }
    while (at < zone.n) {
        memset(v, 0, sizeof(v));
        v[0] = 0x40 | (0x9e >> 2);
        v[1] = (uint8_t)((0x9e & 3) << 6 | 5);
        v[2] = (uint8_t)(cnt >> 16), v[3] = (uint8_t)(cnt >> 8), v[4] = (uint8_t)cnt;
        cnt++;
        /* the first header pointer: where a packet starts in this zone (packets are walked from the stream's
         * start), 0x7ff none */
        size_t p = 0, fhp = 0x7ff;
        while (p < zone.n) {
            size_t len = 6 + ((size_t)zone.b[p + 4] << 8 | zone.b[p + 5]) + 1;
            if (p >= at && p < at + 882) {
                fhp = p - at;
                break;
            }
            if (p >= at + 882) break;
            p += len;
        }
        v[8] = (uint8_t)(fhp >> 8), v[9] = (uint8_t)fhp;
        for (int i = 0; i < 882; i++) v[10 + i] = at + i < zone.n ? zone.b[at + i] : 0;
        if (at + 882 > zone.n) {    /* an idle packet fills the rest */
            size_t r = zone.n - at;
            if (882 - r >= 7) {
                int il = (int)(882 - r - 7);
                v[10 + r] = 0x07, v[10 + r + 1] = 0xff, v[10 + r + 2] = 0xc0, v[10 + r + 3] = 0;
                v[10 + r + 4] = (uint8_t)(il >> 8), v[10 + r + 5] = (uint8_t)il;
            }
        }
        at += 882;
        cadu_from(v);
    }
    for (int i = 0; i < 4; i++) {               /* and after: the last frames' Viterbi run-out */
        memset(v, 0, sizeof(v));
        v[0] = 0x40 | (0x9e >> 2), v[1] = (uint8_t)((0x9e & 3) << 6 | 63);
        v[2] = (uint8_t)(fcnt >> 16), v[3] = (uint8_t)(fcnt >> 8), v[4] = (uint8_t)fcnt++;
        cadu_from(v);
    }
}

/* ---- The coded bits ---- */
static uint8_t *coded;          /* 0/1, two a data bit */
static size_t   ncoded;

static void encode_all(void) {
    size_t   nb = cadus.n * 8;
    uint8_t *bits = malloc(nb);
    int      st = 0;

    for (size_t i = 0; i < nb; i++) bits[i] = (cadus.b[i / 8] >> (7 - i % 8)) & 1;
    coded  = malloc(2 * nb);
    ncoded = 2 * nb;
    st     = lrpt_conv_encode(bits, nb, coded, st);
    (void)st;
    free(bits);
}

/* The coded bits as sent: differential (each rail against its own last), interleaved with markers (80k) */
static uint8_t *on_air(bool diff, bool il, size_t *n) {
    uint8_t *d = malloc(ncoded);

    for (size_t i = 0; i < ncoded; i++) d[i] = diff ? (uint8_t)(coded[i] ^ (i >= 2 ? d[i - 2] : 0)) : coded[i];
    if (!il) {
        *n = ncoded;
        return d;
    }
    const size_t M = 2048, N = 36, D = (N - 1) * M * N;
    size_t       nd = ncoded + D, total = nd / 72 * 80 + 80;
    uint8_t     *x = calloc(nd, 1), *o = malloc(total);
    for (size_t q = 0; q < ncoded; q++) x[q + (q % N) * M * N] = d[q];
    size_t k = 0;
    for (size_t p = 0; p + 72 <= nd; p += 72) {
        for (int j = 0; j < 8; j++) o[k++] = (0x27 >> (7 - j)) & 1;
        for (int j = 0; j < 72; j++) o[k++] = x[p + j];
    }
    free(d);
    free(x);
    *n = k;
    return o;
}

static int turn_pair(int t, int a, int b, int *x, int *y) {
    switch (t) {
    default: *x = a, *y = b; break;
    case 1: *x = -b, *y = a; break;
    case 2: *x = -a, *y = -b; break;
    case 3: *x = b, *y = -a; break;
    case 4: *x = a, *y = -b; break;
    case 5: *x = b, *y = a; break;
    case 6: *x = -a, *y = b; break;
    case 7: *x = -b, *y = -a; break;
    }
    return 0;
}

/* Each channel's picture against what its MCUs decode to: the rows not as they should be (or not there). The picture
 * starts at the first strip received: lined up with the one sent first */
static long compare(lrpt_t *l, int strips_lost_ok) {
    long best = -1;
    for (int off = 0; off <= strips_lost_ok && off < STRIPS; off++) {
        long bad = 0;
        for (int c = 0; c < 3; c++) {
            int            h;
            const uint8_t *px = lrpt_image(l, apids[c] - 64, &h);
            if (!px) return -1;
            for (int y = 0; y < STRIPS * 8; y++) {      /* a row not there is wrong */
                int at = y - off * 8;
                bad += at < 0 || at >= h || memcmp(px + (size_t)at * LRPT_W, want[c][y], LRPT_W) != 0;
            }
        }
        if (best < 0 || bad < best) best = bad;
    }
    return best;
}

/* Each case's noise its own, from its name: the same alone (LRPT_ONLY) as among the others */
static void seed(const char *name) {
    rng = 2166136261u;
    for (; *name; name++) rng = (rng ^ (uint8_t)*name) * 16777619u;
    if (getenv("LRPT_SEED")) rng ^= (uint32_t)atoi(getenv("LRPT_SEED")) * 2654435761u;
    if (!rng) rng = 1;
}

static void soft_run(const char *name, int mode, bool diff, bool il, int t, int slip, float noise, int kill) {
    if (getenv("LRPT_ONLY") && !strstr(name, getenv("LRPT_ONLY"))) return;
    seed(name);
    size_t   n;
    uint8_t *b = on_air(diff, il, &n);
    int8_t  *s = malloc(n + (size_t)slip + 4);
    size_t   k = 0;
    char     what[200];

    /* the demodulator's pairs: `slip` bits before the first sent, the turn on its pairs (QPSK: the slip even, a
     * symbol's two bits together; offset QPSK: any slip, as its rails pair either way, and only the rails' signs:
     * its turns by 90 degrees are a bit's slip and signs) */
    for (int i = 0; i < slip; i++) s[k++] = (rnd() & 1) ? 40 : -40;
    for (size_t i = 0; i < n; i++) s[k++] = b[i] ? -40 : 40;
    k &= ~(size_t)1;
    for (size_t i = 0; i < k; i += 2) {
        int x, y;
        turn_pair(t, s[i], s[i + 1], &x, &y);
        float fx = x + noise * gauss(), fy = y + noise * gauss();
        s[i]     = (int8_t)(fx > 127 ? 127 : fx < -127 ? -127 : fx);
        s[i + 1] = (int8_t)(fy > 127 ? 127 : fy < -127 ? -127 : fy);
    }
    if (kill) {                     /* one frame's bits (the 6th of the pictures') turned to noise */
        size_t at = (size_t)slip + (size_t)(6 + 4) * 16384;
        for (size_t i = 0; i < 16384 && at + i < k; i++) s[at + i] = (int8_t)(((rnd() & 0xff) - 128) | 1);
    }
    lrpt_t     *l = lrpt_new(0, mode);
    lrpt_stat_t st;
    lrpt_feed_soft(l, s, k);
    lrpt_stat(l, &st);
    long bad = compare(l, kill ? 1 : 0);
    snprintf(what, sizeof(what), "%s: %u of %d frames, %u corrected, %u RS bytes fixed, %u packets; picture rows wrong %ld",
             name, st.frames, frames_made, st.frames_ok, st.rs_fixed, st.packets, bad);
    if (kill) check(bad >= 0 && bad <= 6 * 8 && st.frames_ok >= (unsigned)frames_made - 8, what);
    else check(bad == 0 && st.frames_ok >= (unsigned)frames_made - 6, what);
    lrpt_free(l);
    free(s);
    free(b);
}

/* ---- The signal: QPSK or offset QPSK through a root raised cosine ---- */
static float rrc(float t) {
    const float a = 0.6f, pi = (float)M_PI;
    if (fabsf(t) < 1e-6f) return 1 - a + 4 * a / pi;
    if (fabsf(fabsf(t) - 1 / (4 * a)) < 1e-5f)
        return a / sqrtf(2) * ((1 + 2 / pi) * sinf(pi / (4 * a)) + (1 - 2 / pi) * cosf(pi / (4 * a)));
    return (sinf(pi * t * (1 - a)) + 4 * a * t * cosf(pi * t * (1 + a))) / (pi * t * (1 - 16 * a * a * t * t));
}

static void signal_run(const char *name, int mode, float rs, bool oq, bool diff, bool il, float fs, float df,
                       float ppm, float snr_db, float min_ok) {
    if (getenv("LRPT_ONLY") && !strstr(name, getenv("LRPT_ONLY"))) return;     /* one case alone, to look at it */
    seed(name);
    size_t   n;
    uint8_t *b = on_air(diff, il, &n);
    size_t   nsym = n / 2;
    float    T = fs / rs * (1 + ppm * 1e-6f);      /* samples a symbol, the satellite's clock off */
    size_t   ns = (size_t)((nsym + 20) * T);
    float   *iq = calloc(2 * ns, sizeof(float));
    float    sig = 0;
    char     what[240];
    int      span = 5;

    /* each symbol's pulse added in: I at kT, Q at kT (+T/2 offset) */
    for (size_t k = 0; k < nsym; k++) {
        float a = b[2 * k] ? -1.0f : 1.0f, c = b[2 * k + 1] ? -1.0f : 1.0f;
        for (int part = 0; part < 2; part++) {
            float t0 = (k + 10) * T + (part && oq ? T / 2 : 0);
            long  lo = (long)(t0 - span * T), hi = (long)(t0 + span * T);
            for (long i = lo < 0 ? 0 : lo; i <= hi && i < (long)ns; i++) {
                float v = rrc((i - t0) / T);
                if (!part) iq[2 * i] += a * v;
                else iq[2 * i + 1] += c * v;
            }
        }
    }
    for (size_t i = 0; i < ns; i++) sig += iq[2 * i] * iq[2 * i] + iq[2 * i + 1] * iq[2 * i + 1];
    sig /= ns;
    /* the carrier off by df (and a phase), noise for the SNR in the symbols' bandwidth (rs) */
    float  nsd = sqrtf(sig / powf(10, snr_db / 10) * (fs / rs) / 2);
    double ph  = 1.1;
    for (size_t i = 0; i < ns; i++) {
        float c = (float)cos(ph), s = (float)sin(ph), r = iq[2 * i], q = iq[2 * i + 1];
        iq[2 * i]     = r * c - q * s + nsd * gauss();
        iq[2 * i + 1] = r * s + q * c + nsd * gauss();
        ph = fmod(ph + 2 * M_PI * df / fs, 2 * M_PI);
    }
    lrpt_t     *l = lrpt_new(fs, mode);
    lrpt_stat_t st;
    clock_t     c0 = clock();
    for (size_t i = 0; i < ns; i += 4096) lrpt_feed(l, iq + 2 * i, ns - i < 4096 ? ns - i : 4096);
    double cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    lrpt_stat(l, &st);
    long bad = compare(l, STRIPS);
    snprintf(what, sizeof(what),
             "%s: %u of %d frames, %u corrected (%u RS bytes), rows wrong %ld, carrier %+.0f Hz (sent %+.0f), quality "
             "%.2f, %.2f s of processor for %.1f s",
             name, st.frames, frames_made, st.frames_ok, st.rs_fixed, bad, st.freq, df, st.quality, cpu, ns / fs);
    check(bad >= 0 && bad <= (long)((1 - min_ok) * 3 * STRIPS * 8) &&
              fabsf(st.freq - df) < 50,
          what);
    lrpt_free(l);
    free(iq);
    free(b);
}

int main(void) {
    const uint8_t *dcb, *dcv, *acb, *acv;
    char           what[160];

    lrpt_jpeg_tables(&dcb, &dcv, &acb, &acv);
    henc_build(&hdc, dcb, dcv);
    henc_build(&hac, acb, acv);

    printf("The layers\n");
    {   /* the AC table: every run/size once, EOB and ZRL */
        int seen[256] = {0}, ok = 1, total = 0;
        for (int i = 0; i < 16; i++) total += acb[i];
        for (int i = 0; i < 162; i++) seen[acv[i]]++;
        for (int r = 0; r < 16; r++)
            for (int s = 1; s <= 10; s++) ok &= seen[r << 4 | s] == 1;
        check(ok && seen[0] == 1 && seen[0xf0] == 1 && total == 162, "the AC Huffman table: 160 run/sizes, EOB, ZRL");
    }
    {
        uint8_t z[8] = {0};
        lrpt_pn(z, 8);
        check(z[0] == 0xff && z[1] == 0x48 && z[2] == 0x0e && z[3] == 0xc0 && z[4] == 0x9a && z[5] == 0x0d &&
                  z[6] == 0x70 && z[7] == 0xbc,
              "the scrambler: ff 48 0e c0 9a 0d 70 bc");
    }
    for (int dual = 0; dual < 2; dual++) {   /* RS: up to 16 bytes wrong in any places corrected; 17 refused */
        int ok = 1, refused = 0;
        for (int trial = 0; trial < 300; trial++) {
            uint8_t d[223], cw[255], sent[255];
            for (int i = 0; i < 223; i++) d[i] = (uint8_t)rnd();
            memcpy(cw, d, 223);
            lrpt_rs_encode(d, cw + 223, dual);
            memcpy(sent, cw, 255);
            ok &= lrpt_rs_decode(cw, dual) == 0;
            int ne = trial % 18;
            for (int e = 0; e < ne; e++) {      /* ne different bytes */
                int at;
                do at = (int)(rnd() % 255);
                while (cw[at] != sent[at]);
                cw[at] ^= (uint8_t)(rnd() | 1);
            }
            int r = lrpt_rs_decode(cw, dual);
            if (ne <= 16) ok &= r == ne && memcmp(cw, sent, 255) == 0;
            else refused += r < 0;
        }
        snprintf(what, sizeof(what), "RS(255,223) %s basis: 0-16 bytes wrong corrected; 17 refused (%d of 16)",
                 dual ? "dual" : "conventional", refused);
        check(ok && refused == 16, what);
    }
    {   /* the two bases told apart: a codeword of one is not one of the other */
        uint8_t d[223], cw[255];
        for (int i = 0; i < 223; i++) d[i] = (uint8_t)rnd();
        memcpy(cw, d, 223);
        lrpt_rs_encode(d, cw + 223, false);
        check(lrpt_rs_decode(cw, false) == 0 && lrpt_rs_decode(cw, true) != 0, "RS: the bases told apart");
    }
    {   /* a strip's MCUs: the picture back within the quantization's error */
        uint8_t px[8][112], got[8][112];
        buf_t   j = {0};
        double  err = 0;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 112; x++) px[y][x] = (uint8_t)(128 + 90 * sin(x / 9.0) * cos(y / 3.0));
        jpeg_mcus(px, 90, &j);
        int n = lrpt_mcus(j.b, j.n, 90, got);
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 112; x++) err += fabs((double)got[y][x] - px[y][x]);
        snprintf(what, sizeof(what), "MCUs: 14 of 14 decoded (%d), mean error %.2f at quality 90 (%zu bytes)", n,
                 err / 896, j.n);
        check(n == 14 && err / 896 < 2.5, what);
        free(j.b);
    }

    make_pictures();
    make_packets();
    make_cadus(6);
    encode_all();
    printf("The bits (%d frames, %zu packet bytes)\n", frames_made, zone.n);
    for (int t = 0; t < 8; t++) {
        char name[64];
        snprintf(name, sizeof(name), "72k QPSK, turned %d, %d bits off", t, t * 2);
        soft_run(name, LRPT_72K_QPSK, false, false, t, t * 2, 0, 0);
    }
    for (int t = 0; t < 8; t += 2) {
        char name[80];
        snprintf(name, sizeof(name), "72k OQPSK differential, signs %d, %d bits off, noise", t, t + 1);
        soft_run(name, LRPT_72K_OQPSK, true, false, t, t + 1, 20, 0);
    }
    soft_run("72k QPSK, a frame lost", LRPT_72K_QPSK, false, false, 2, 0, 20, 1);
    soft_run("80k QPSK interleaved, turned 3, 8 bits off", LRPT_80K_QPSK, false, true, 3, 8, 20, 0);
    soft_run("80k OQPSK interleaved differential, signs 6, 7 bits off", LRPT_80K_OQPSK, true, true, 6, 7, 20, 0);

    printf("The signal\n");
    signal_run("72k QPSK at 264.6 kHz, +2.5 kHz, 30 ppm, 9 dB", LRPT_72K_QPSK, 72000, false, false, false, 264600, 2500,
               30, 9, 0.97f);
    signal_run("72k QPSK at 144 kHz (2 a symbol), -4 kHz, -20 ppm, 9 dB", LRPT_72K_QPSK, 72000, false, false, false,
               144000, -4000, -20, 9, 0.97f);
    signal_run("72k OQPSK differential at 264.6 kHz, -1.5 kHz, 9 dB", LRPT_72K_OQPSK, 72000, true, true, false, 264600,
               -1500, 10, 9, 0.97f);
    signal_run("72k QPSK at 264.6 kHz, +1 kHz, 5 dB", LRPT_72K_QPSK, 72000, false, false, false, 264600, 1000, 0, 5,
               0.9f);
    signal_run("AUTO on 72k OQPSK at 264.6 kHz, +3 kHz, 10 dB", LRPT_AUTO, 72000, true, true, false, 264600, 3000, 0,
               10, 0.8f);
    signal_run("72k OQPSK differential at 264.6 kHz, +12 kHz (past the fourth power's reach), 10 dB", LRPT_72K_OQPSK,
               72000, true, true, false, 264600, 12000, 0, 10, 0.97f);
    signal_run("72k QPSK at 264.6 kHz, -14 kHz (past the fourth power's reach), 8 dB", LRPT_72K_QPSK, 72000, false,
               false, false, 264600, -14000, -10, 8, 0.97f);
    signal_run("80k OQPSK interleaved differential at 264.6 kHz, -2 kHz, 10 dB", LRPT_80K_OQPSK, 80000, true, true,
               true, 264600, -2000, 15, 10, 0.95f);      /* the first frames have the most erasures */

    printf("%d checks, %d wrong\n", checks, fails);
    return fails ? 1 : 0;
}
