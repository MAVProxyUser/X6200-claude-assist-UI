/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - acars.c on made ACARS: ARINC 618 messages (odd parity, the BCS, "+*" SYN SYN SOH after a pre-key) as
 *  2400 bit/s MSK audio, 1200 and 2400 Hz phase continuous, with the tones standing for the bits either way (as they
 *  are, inverted, or each bit the tone kept or changed), noise, the sender's clock off, and back to back. Off the radio:
 *      cc -O2 -o /tmp/acars_test tools/rtlspec/acars_test.c tools/rtlspec/acars.c -lm && /tmp/acars_test
 */
#include "acars.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100.0f

static int fails, cases;

static void check(int ok, const char *what) {
    cases++;
    if (!ok) fails++;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint8_t par(uint8_t c) {
    c &= 0x7f;
    return __builtin_popcount(c) & 1 ? c : (uint8_t)(c | 0x80);
}

/* A message's bytes from the pre-key on */
static int frame(uint8_t *f, char mode, const char *reg7, uint8_t ack, const char *label, char blk, const char *text,
                 int etb) {
    int n = 0;
    for (int i = 0; i < 16; i++) f[n++] = 0xff;     /* the pre-key */
    f[n++] = par('+'), f[n++] = par('*'), f[n++] = par(0x16), f[n++] = par(0x16), f[n++] = par(0x01);
    int body = n;
    f[n++] = par((uint8_t)mode);
    for (int i = 0; i < 7; i++) f[n++] = par((uint8_t)reg7[i]);
    f[n++] = par(ack);
    f[n++] = par((uint8_t)label[0]), f[n++] = par((uint8_t)label[1]);
    f[n++] = par((uint8_t)blk);
    if (text) {
        f[n++] = par(0x02);
        for (const char *t = text; *t; t++) f[n++] = par((uint8_t)*t);
    }
    f[n++] = par(etb ? 0x17 : 0x03);
    uint16_t crc = acars_crc(f + body, (size_t)(n - body));
    f[n++] = (uint8_t)(crc & 0xff), f[n++] = (uint8_t)(crc >> 8);
    f[n++] = par(0x7f);
    f[n++] = 0xff, f[n++] = 0xff;
    return n;
}

static float gauss(void) {
    float u = (float)((rand() + 1.0) / (RAND_MAX + 2.0)), v = (float)((rand() + 1.0) / (RAND_MAX + 2.0));
    return sqrtf(-2 * logf(u)) * cosf(2 * (float)M_PI * v);
}

/* The bytes as MSK audio after lead seconds of noise: way 0 the tone is the bit (1 the 2400 Hz), inverted with inv;
 * way 1 a 1 keeps the tone, a 0 changes it. ppm: the sender's clock off. Into x, the count */
static size_t synth(float *x, size_t cap, const uint8_t *b, int n, int way, int inv, float noise, float ppm, float lead) {
    size_t s = 0;
    double T = 1.0 / 2400 / (1 + ppm * 1e-6), t0 = lead, ph0 = 0;   /* the sender's bit, its start; its phase then */
    int    f = 1, k = 0;

    for (; s < (size_t)(lead * RATE) && s < cap; s++) x[s] = noise * gauss();
    /* exact MSK: within bit k the phase is ph0 + 2 pi f (t - t_k), whatever sample falls there; each bit's tone moves
     * it half a turn (1200 Hz) or a whole one (2400): every bit starts on a zero crossing, as a real sender's */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 8; j++, k++) {
            int d = (b[i] >> j) & 1;
            f = way ? (d ? f : !f) : d ^ inv;
            double fr = f ? 2400.0 : 1200.0, tk = t0 + k * T;
            for (; s < cap && (double)s / RATE < tk + T; s++)
                x[s] = 0.5f * (float)sin(ph0 + 2 * M_PI * fr * ((double)s / RATE - tk)) + noise * gauss();
            ph0 += 2 * M_PI * fr * T;
        }
    for (int i = 0; i < (int)(0.2f * RATE) && s < cap; i++, s++) x[s] = noise * gauss();
    return s;
}

static float buf[44100 * 6];

static int run(acars_msg_t *m, int max, int way, int inv, float noise, float ppm, float lead, int two) {
    uint8_t  b[1024];
    int      n = frame(b, '2', ".N12345", 0x15, "H1", '3', "M01AUA1234HELLO FROM THE BENCH 0123456789", 0);
    if (two) n += frame(b + n, '2', ".N54321", 'A', "_\x7f", 'B', NULL, 1);
    size_t   len = synth(buf, sizeof(buf) / sizeof(buf[0]), b, n, way, inv, noise, ppm, lead);
    acars_t *a   = acars_new(RATE);
    int      got = 0;
    for (size_t at = 0; at < len; at += 2205) {     /* 50 ms at a time, as the daemon's audio comes */
        size_t k = len - at < 2205 ? len - at : 2205;
        got += acars_feed(a, buf + at, k, m + got, max - got);
    }
    acars_free(a);
    return got;
}

static int first_ok(const acars_msg_t *m) {
    return m->mode == '2' && !strcmp(m->reg, "N12345") && m->ack == '!' && !strcmp(m->label, "H1") && m->block == '3' &&
           !strcmp(m->msn, "M01A") && !strcmp(m->flight, "UA1234") && !strcmp(m->text, "HELLO FROM THE BENCH 0123456789") &&
           !m->more && m->parity_errs == 0;
}

int main(void) {
    acars_msg_t m[4];
    char        what[128];

    check(acars_crc((const uint8_t *)"123456789", 9) == 0x2189, "the BCS's CRC: CRC-16/KERMIT's check value 0x2189");

    for (int way = 0; way < 2; way++)
        for (int inv = 0; inv < 2; inv++) {
            int got = run(m, 4, way, inv, 0, 0, 0.137f, 0);
            snprintf(what, sizeof(what), "clean, tones %s%s: one message, every field", way ? "as changes" : "as bits",
                     inv ? ", inverted" : "");
            check(got == 1 && first_ok(&m[0]), what);
        }
    {
        int got = run(m, 4, 0, 0, 0, 0, 0.31f, 1);
        check(got == 2 && first_ok(&m[0]) && !strcmp(m[1].reg, "N54321") && m[1].ack == 'A' && !strcmp(m[1].label, "_d") &&
                  m[1].block == 'B' && !m[1].msn[0] && !m[1].text[0] && m[1].more,
              "back to back: the second an uplink with no text, ETB, label _d");
    }
    for (int p = -1; p <= 1; p += 2) {
        int got = run(m, 4, 1, 0, 0.05f, p * 300.0f, 0.2f, 0);
        snprintf(what, sizeof(what), "the sender's clock %+d ppm, a little noise", p * 300);
        check(got == 1 && first_ok(&m[0]), what);
    }
    /* noise: where it stops reading (10 tries a level) */
    float worst = 0;
    for (int i = 1; i <= 12; i++) {
        float noise = 0.05f * i;
        int   ok    = 0;
        srand(1000 + i);
        for (int t = 0; t < 10; t++) ok += run(m, 4, 0, 0, noise, 0, 0.1f + t * 0.013f, 0) == 1 && first_ok(&m[0]);
        printf("       noise %.2f (SNR %4.1f dB in 4.8 kHz): %d of 10\n", noise,
               10 * log10f(0.125f / (noise * noise * 4800 / 22050)), ok);
        if (ok >= 9) worst = noise;
    }
    snprintf(what, sizeof(what), "9 of 10 or more read up to noise %.2f (at least 0.30, 8 dB, wanted)", worst);
    check(worst >= 0.30f, what);
    {
        int      got = 0;
        acars_t *a   = acars_new(RATE);
        uint32_t sy, bad;
        srand(7);
        for (int k = 0; k < 20; k++) {
            for (int i = 0; i < 44100; i++) buf[i] = 0.5f * gauss();
            got += acars_feed(a, buf, 44100, m, 4);
        }
        acars_counts(a, &sy, &bad);
        acars_free(a);
        snprintf(what, sizeof(what), "20 s of noise alone: nothing (syncs %u, failed %u)", sy, bad);
        check(got == 0, what);
    }
    printf("%d of %d ACARS cases as they should be\n", cases - fails, cases);
    return fails != 0;
}
