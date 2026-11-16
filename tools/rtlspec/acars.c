/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - ACARS from one channel's AM audio (acars.h). Coherent MSK, as acarsdec reads it: an oscillator on the
 *  1800 Hz between the two tones mixes the audio down, a half-sine filter two bits long (MSK's own pulse) gives each
 *  bit's I or Q in turn, and the sign of the one due is the bit. What the other says of the oscillator's phase steers
 *  it (a phase-locked loop). ACARS keys its tones on their zero crossings, so its bits keep step with the 1800 Hz: the
 *  oscillator's own turning is the bit clock, three quarters of a turn a bit.
 *
 *  How the bits stand for the data is not taken for granted: as they come (acarsdec's reading: MSK's own I and Q, the
 *  tones their changes), their changes (the tones as the bits) or the changes of those, each as it is or inverted,
 *  are all read for the sync, '*', SYN, SYN, SOH, and the one that shows it reads the message. The check (BCS) says
 *  whether it was right.
 */

#include "acars.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BAUD    2400.0f
#define FC      1800.0f
#define FMAX    128                 /* the half-sine filter's length at most: rate / 1200 + 1 (37 at 44.1 kHz) */
#define PLL_BIT 0.02f               /* the loop's phase step a bit for a whole radian of error */
#define MAXLEN  240                 /* mode .. ETX: 12 + STX + 220 + ETX, and room */
#define SYNC    0x0116162Au         /* '*' SYN SYN SOH as they come, least significant bit first: their bytes */

#define STX 0x02
#define ETX 0x03
#define ETB 0x17
#define NAK 0x15

enum { HUNT, BODY, BCS1, BCS2 };

struct acars_s {
    float    rate;
    float    clk, df;               /* the bit clock's phase (radians), the oscillator's correction a sample */
    float    pc, ps, stc, sts;      /* the oscillator: cos, sin of its phase, and of its step (set once a bit) */
    float    pllc;                  /* the loop's gain */
    float    mh[FMAX];              /* the half-sine filter */
    float    ib_r[FMAX], ib_i[FMAX];
    int      flen, ib_at;
    unsigned ms;                    /* bits so far: I or Q, and the sign's turn */
    int      last_f, last_ch;       /* the last bit, and whether it was kept */
    uint32_t sr[3];                 /* the bits as they are [0], kept or changed [1], that kept or changed [2] */
    int      way;                   /* the reading that showed the sync */
    bool     inv;                   /* inverted */
    int      state, nbit, len;
    uint8_t  byte;
    uint8_t  buf[MAXLEN + 2];
    float    pow_sum;
    int      pow_n;
    uint32_t synced, bad;
};

uint16_t acars_crc(const uint8_t *b, size_t n) {
    uint16_t crc = 0;

    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) crc = crc & 1 ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}

acars_t *acars_new(float rate) {
    acars_t *a = calloc(1, sizeof(*a));

    if (!a) return NULL;
    a->rate = rate;
    a->flen = (int)(rate / 1200) + 1;
    if (a->flen > FMAX) a->flen = FMAX;
    for (int j = 0; j < a->flen; j++)       /* half a turn of 600 Hz over two bits */
        a->mh[j] = cosf(2 * (float)M_PI * 600.0f * (j - (a->flen - 1) / 2.0f) / rate);
    a->pllc = PLL_BIT * BAUD / rate;
    a->pc   = 1;
    a->stc  = cosf(2 * (float)M_PI * FC / rate);
    a->sts  = sinf(2 * (float)M_PI * FC / rate);
    return a;
}

void acars_free(acars_t *a) { free(a); }

void acars_counts(const acars_t *a, uint32_t *synced, uint32_t *bad) {
    if (synced) *synced = a->synced;
    if (bad) *bad = a->bad;
}

static int odd(uint8_t c) { return __builtin_popcount(c) & 1; }

static void hunt(acars_t *a) {
    a->state = HUNT;
    a->sr[0] = a->sr[1] = a->sr[2] = 0;
}

/* The message in buf (mode .. ETX/ETB, then the BCS) as out: true when its check passed */
static bool message(acars_t *a, acars_msg_t *m) {
    int n = a->len;                         /* mode .. ETX/ETB */

    if (n < 13 || acars_crc(a->buf, (size_t)n + 2) != 0) return false;
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < n; i++) m->parity_errs += !odd(a->buf[i]);
    uint8_t c[MAXLEN];
    for (int i = 0; i < n; i++) c[i] = a->buf[i] & 0x7f;
    m->mode = (char)c[0];
    int r = 1;
    while (r < 8 && c[r] == '.') r++;
    int k = 0;
    for (; r < 8; r++) m->reg[k++] = c[r] >= ' ' && c[r] < 0x7f ? (char)c[r] : ' ';
    m->ack      = c[8] == NAK ? '!' : (char)c[8];
    m->label[0] = (char)c[9];
    m->label[1] = c[10] == 0x7f ? 'd' : (char)c[10];
    m->block    = (char)c[11];
    m->more     = c[n - 1] == ETB;
    int t = 12, end = n - 1;                /* the text: after STX, to ETX/ETB */
    if (c[12] == STX) t = 13;
    else end = 12;                          /* none */
    if (m->block >= '0' && m->block <= '9' && end - t >= 10) {     /* from the aircraft: its number and flight */
        memcpy(m->msn, c + t, 4);
        memcpy(m->flight, c + t + 4, 6);
        for (int i = 0; i < 6; i++)
            if (m->flight[i] < ' ' || m->flight[i] >= 0x7f) m->flight[i] = ' ';
        t += 10;
    }
    k = 0;
    for (int i = t; i < end && k < (int)sizeof(m->text) - 1; i++) m->text[k++] = c[i] >= ' ' && c[i] < 0x7f ? (char)c[i] : ' ';
    while (k && m->text[k - 1] == ' ') k--;
    m->text[k] = 0;
    m->level = a->pow_n ? 10 * log10f(a->pow_sum / a->pow_n + 1e-12f) : -99;
    return true;
}

/* A data bit, read as the sync showed */
static int bit_in(acars_t *a, int b, acars_msg_t *out, int max, int *got) {
    a->byte |= (uint8_t)(b << a->nbit);
    if (++a->nbit < 8) return 0;
    uint8_t c = a->byte;
    a->nbit = 0;
    a->byte = 0;
    switch (a->state) {
    case BODY:
        a->buf[a->len++] = c;
        if ((c & 0x7f) == ETX || (c & 0x7f) == ETB) a->state = BCS1;
        else if (a->len >= MAXLEN) hunt(a);
        break;
    case BCS1:
        a->buf[a->len] = c;
        a->state = BCS2;
        break;
    case BCS2:
        a->buf[a->len + 1] = c;
        if (*got < max && message(a, &out[*got])) (*got)++;
        else a->bad++;
        hunt(a);
        break;
    }
    return 0;
}

/* A bit as demodulated, before the sync says how it reads */
static void tone_in(acars_t *a, int f, acars_msg_t *out, int max, int *got) {
    int ch = f == a->last_f, ch2 = ch == a->last_ch;   /* the bit kept; that kept as the one before */

    a->last_f  = f;
    a->last_ch = ch;
    if (a->state == HUNT) {
        a->sr[0] = a->sr[0] >> 1 | (uint32_t)f << 31;
        a->sr[1] = a->sr[1] >> 1 | (uint32_t)ch << 31;
        a->sr[2] = a->sr[2] >> 1 | (uint32_t)ch2 << 31;
        for (int w = 0; w < 3; w++)
            if (a->sr[w] == SYNC || a->sr[w] == ~SYNC) {
                a->way   = w;
                a->inv   = a->sr[w] != SYNC;
                a->state = BODY;
                a->len = a->nbit = 0;
                a->byte    = 0;
                a->pow_sum = 0;
                a->pow_n   = 0;
                a->synced++;
                return;
            }
        return;
    }
    int b = (a->way == 0 ? f : a->way == 1 ? ch : ch2) ^ (a->inv ? 1 : 0);
    bit_in(a, b, out, max, got);
}

int acars_feed(acars_t *a, const float *x, size_t n, acars_msg_t *out, int max) {
    const float w0 = 2 * (float)M_PI * FC / a->rate, turn = 3 * (float)M_PI / 2;
    int         got = 0;

    for (size_t s = 0; s < n; s++) {
        float st = w0 + a->df;
        float c  = a->pc * a->stc - a->ps * a->sts, sn = a->pc * a->sts + a->ps * a->stc;   /* a step on, turned */

        a->pc = c;
        a->ps = sn;
        a->ib_r[a->ib_at] = x[s] * c;
        a->ib_i[a->ib_at] = -x[s] * sn;
        a->ib_at = (a->ib_at + 1) % a->flen;
        if (a->state != HUNT) {
            a->pow_sum += x[s] * x[s];
            a->pow_n++;
        }
        a->clk += st;
        if (a->clk < turn - st / 2) continue;
        a->clk -= turn;

        float vr = 0, vi = 0;                   /* the last two bits through the half-sine, oldest first */
        for (int j = 0; j < a->flen; j++) {
            int k = (a->ib_at + j) % a->flen;
            vr += a->mh[j] * a->ib_r[k];
            vi += a->mh[j] * a->ib_i[k];
        }
        float l = sqrtf(vr * vr + vi * vi) + 1e-9f, vo, dphi;
        vr /= l;
        vi /= l;
        if (a->ms & 1) {                        /* Q's turn: I says how far the phase is off */
            vo   = vi;
            dphi = vo >= 0 ? -vr : vr;
        } else {
            vo   = vr;
            dphi = vo >= 0 ? vi : -vi;
        }
        int b = ((a->ms & 2) ? -vo : vo) > 0;
        a->ms++;
        a->df  = a->pllc * dphi;
        a->stc = cosf(w0 + a->df);              /* the step for the bit to come; the length kept at 1 */
        a->sts = sinf(w0 + a->df);
        float g = 1.5f - 0.5f * (a->pc * a->pc + a->ps * a->ps);
        a->pc *= g;
        a->ps *= g;
        tone_in(a, b, out, max, &got);
    }
    return got;
}
