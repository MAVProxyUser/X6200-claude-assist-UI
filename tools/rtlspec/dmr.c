/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - DMR from a narrow FM channel's discriminator (dmr.h). The discriminator through a root raised cosine
 *  (roll-off 0.2: the transmitter's, so the two make a raised cosine with no intersymbol interference) into a ring;
 *  the sync searched for at every sample (24 symbols of +-3 against each of the eight patterns, its DC off, every
 *  sign right while not locked); a sync found, its burst read at its symbols (the levels from the sync's own) and the
 *  bursts after it every 30 ms (144 symbols: a burst and a CACH) on the same clock, the slot from the CACH (a
 *  repeater's) or the sync (direct mode), else the other one. Locked till no sync for 400 ms.
 *  A voice burst A (its sync) and B-F after it in the same slot: three AMBE frames each, out as sent; B-E's embedded
 *  signalling put together into the link control (its 5-bit checksum). A data burst: the slot type's colour code and
 *  data type; a voice header or terminator's link control (BPTC(196,96), its Hamming codes put right, RS(12,9) with
 *  the header's or the terminator's mask). Bit orders and tables: ETSI TS 102 361-1 (B.1, B.2, B.3, 9.1).
 */

#include "dmr.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define SYM_RATE  4800.0
#define RRC_SPAN  4             /* symbols each side */
#define RRC_MAX   96
#define RING      8192          /* filtered samples kept: 185 ms at 44.1 kHz */
#define BURST     132           /* symbols */
#define SLOT_SYM  144           /* a burst and the CACH after it: 30 ms */
#define SYNC_AT   54            /* the sync's first symbol in its burst */
#define QUEUE     8
#define MIN_LEVEL 0.08f         /* the sync's symbols at least this far apart on average (+-400 Hz): not noise */
#define LOCK_S    0.4

enum { P_BS_VOICE, P_BS_DATA, P_MS_VOICE, P_MS_DATA, P_TS1_VOICE, P_TS1_DATA, P_TS2_VOICE, P_TS2_DATA, P_N };
#define P_NONE (-1)
static const uint64_t sync_pat[P_N] = {0x755FD7DF75F7ULL, 0xDFF57D75DF5DULL, 0x7F7D5DD57DFDULL, 0xD5D7F77FD757ULL,
                                       0x5D577F7757FFULL, 0xF7FDD5DDFD55ULL, 0x7DFFD5F55D5FULL, 0xD7557F5FF7F5ULL};
static bool pat_voice(int p) { return p == P_BS_VOICE || p == P_MS_VOICE || p == P_TS1_VOICE || p == P_TS2_VOICE; }

/* A dibit's symbol: 01 +3, 00 +1, 10 -1, 11 -3 */
static const int sym_of[4] = {1, 3, -1, -3};

typedef struct {
    double start;               /* its symbol 0, in samples */
    int    pat;                 /* its sync, P_NONE one expected there (30 ms after the one before) */
} pend_t;

typedef struct {
    bool       voice;
    int        vf;              /* the voice burst it is at: 0 A, 1-5 B-F */
    double     last_voice;      /* the sample of its last voice burst */
    uint8_t    elc[128];        /* the embedded LC's bits as they come (B-E) */
    int        elc_n;           /* bits so far, -1 waiting for a first piece */
    dmr_call_t call;
} slot_t;

struct dmr_s {
    float       rate;           /* the ring's: the input's over dec_n */
    double      sps;            /* the ring's samples a symbol */
    int         dec_n, dec;     /* the filter's output taken every dec_n-th input sample (2 at 44.1 kHz) */
    float       taps[RRC_MAX], hist[2 * RRC_MAX];
    int         ntaps, hat;
    float       ring[RING];
    uint64_t    n;              /* samples in: the newest at n - 1 */
    signed char sgn[P_N][24];
    uint32_t    pmask[P_N];     /* each sync's signs, bit k for symbol k positive */
    int         s_off[24];      /* a sync's symbol k ending at a whole sample t: between t - s_off[k] and the next */
    float       s_frac[24];
    /* the best sync while it may still grow */
    float       cand_c;         /* its raw match (sync_match) */
    double      cand_at;
    int         cand_p, cand_left, hold;     /* hold: samples after a sync found before another is looked for */
    /* the clock */
    bool        locked, bs;
    double      next_bs, last_sync;
    float       centre, unit;   /* the levels: +-1 symbols at centre +- unit */
    pend_t      q[QUEUE];
    int         qn, prev_slot;
    slot_t      sl[3];          /* by slot: 1, 2 (0 unused) */
    uint32_t    syncs, bursts, lcs, elcs;
    dmr_ambe_cb ambe;
    dmr_call_cb callf;
    void       *ctx;
};

/* ---- The codes ---- */

static uint8_t gexp[512], glog[256];

static void gf_init(void) {
    static bool done;
    int         x = 1;

    if (done) return;
    for (int i = 0; i < 255; i++) {
        gexp[i] = gexp[i + 255] = (uint8_t)x;
        glog[x]                 = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    done = true;
}

static uint8_t gmul(uint8_t a, uint8_t b) { return a && b ? gexp[glog[a] + glog[b]] : 0; }

/* RS(12,9) over GF(256) (x^8+x^4+x^3+x^2+1), generator (x+a)(x+a^2)(x+a^3): its parity, as sent */
void dmr_rs129(const uint8_t lc[9], uint8_t parity[3]) {
    static const uint8_t g[3] = {64, 56, 14};
    uint8_t              p[3] = {0, 0, 0};

    gf_init();
    for (int i = 0; i < 9; i++) {
        uint8_t f = lc[i] ^ p[2];

        p[2] = p[1] ^ gmul(g[2], f);
        p[1] = p[0] ^ gmul(g[1], f);
        p[0] = gmul(g[0], f);
    }
    parity[0] = p[2];
    parity[1] = p[1];
    parity[2] = p[0];
}

/* Hamming (15,11,3) of a BPTC row, (13,9,3) of a column, (16,11,4) of the embedded LC's rows: each parity bit the sum
 * of the data bits in its mask */
static const uint16_t h15[4] = {0x1AF, 0x35E, 0x6BC, 0x4D7};       /* bit i: d[i] */
static const uint16_t h13[4] = {0x06B, 0x0D7, 0x1AF, 0x135};
static const uint16_t h16[5] = {0x1AF, 0x35E, 0x6BC, 0x4D7, 0x765};

static int parity_of(const uint8_t *d, int k, uint16_t mask) {
    int p = 0;

    for (int i = 0; i < k; i++)
        if (mask >> i & 1) p ^= d[i];
    return p;
}

/* A codeword of n bits (k data, then n - k parity) checked; one bit wrong put right. False: more wrong */
static bool ham_fix(uint8_t *d, int n, int k, const uint16_t *mask) {
    int syn = 0;

    for (int j = 0; j < n - k; j++) syn |= (parity_of(d, k, mask[j]) ^ d[k + j]) << j;
    if (!syn) return true;
    for (int i = 0; i < n; i++) {       /* the bit whose own syndrome it is */
        int s = 0;

        for (int j = 0; j < n - k; j++) s |= (i < k ? (mask[j] >> i & 1) : (i - k == j)) << j;
        if (s == syn) {
            d[i] ^= 1;
            return true;
        }
    }
    return false;
}

/* BPTC(196,96): 13 rows of 15 after a reserved bit, rows 0-8 data and Hamming (15,11), rows 9-12 the columns' (13,9);
 * sent interleaved, bit a of the matrix the (a * 181 mod 196)th */
static void bptc_rows_cols(uint8_t m[196], bool fix) {
    for (int r = 0; r < 9; r++) {
        uint8_t *row = m + 1 + 15 * r;

        if (fix) ham_fix(row, 15, 11, h15);
        else
            for (int j = 0; j < 4; j++) row[11 + j] = (uint8_t)parity_of(row, 11, h15[j]);
    }
    for (int c = 0; c < 15; c++) {
        uint8_t col[13];

        for (int r = 0; r < 13; r++) col[r] = m[1 + c + 15 * r];
        if (fix) ham_fix(col, 13, 9, h13);
        else
            for (int j = 0; j < 4; j++) col[9 + j] = (uint8_t)parity_of(col, 9, h13[j]);
        for (int r = 0; r < 13; r++) m[1 + c + 15 * r] = col[r];
    }
}

/* the 96 data bits' places in the matrix: row 0 from its 4th bit (three reserved), rows 1-8 their first 11 */
static int bptc_place(int i) { return i < 8 ? 4 + i : 1 + 15 * ((i - 8) / 11 + 1) + (i - 8) % 11; }

void dmr_bptc_encode(const uint8_t in[96], uint8_t out[196]) {
    uint8_t m[196] = {0};

    for (int i = 0; i < 96; i++) m[bptc_place(i)] = in[i] & 1;
    bptc_rows_cols(m, false);
    for (int a = 0; a < 196; a++) out[a * 181 % 196] = m[a];
}

static void bptc_decode(const uint8_t in[196], uint8_t out[96], bool fix) {
    uint8_t m[196];

    for (int a = 0; a < 196; a++) m[a] = in[a * 181 % 196];
    if (fix) {
        bptc_rows_cols(m, true);
        bptc_rows_cols(m, true);        /* a column put right can leave a row right again */
    }
    for (int i = 0; i < 96; i++) out[i] = m[bptc_place(i)];
}

/* The embedded LC: 72 bits and a 5-bit checksum (the bytes' sum mod 31) in 7 rows of 16 (11 data, Hamming (16,11,4)),
 * an 8th of the columns' parity; sent down the columns, bit a at row a mod 8, column a / 8. The checksum's bits are
 * column 10 of rows 2-6; the LC rows 0-1's 11 bits and rows 2-6's first 10 */
static int elc_place(int i) { return i < 22 ? (i / 11) * 16 + i % 11 : (2 + (i - 22) / 10) * 16 + (i - 22) % 10; }

static uint8_t lc_sum(const uint8_t lc[9]) {
    unsigned s = 0;

    for (int i = 0; i < 9; i++) s += lc[i];
    return (uint8_t)(s % 31);
}

void dmr_elc_encode(const uint8_t lc[9], uint8_t out[128]) {
    uint8_t raw[128] = {0}, cs = lc_sum(lc);

    for (int i = 0; i < 72; i++) raw[elc_place(i)] = lc[i / 8] >> (7 - i % 8) & 1;
    for (int j = 0; j < 5; j++) raw[(2 + j) * 16 + 10] = cs >> (4 - j) & 1;
    for (int r = 0; r < 7; r++)
        for (int j = 0; j < 5; j++) raw[r * 16 + 11 + j] = (uint8_t)parity_of(raw + r * 16, 11, h16[j]);
    for (int c = 0; c < 16; c++) {
        int p = 0;

        for (int r = 0; r < 7; r++) p ^= raw[r * 16 + c];
        raw[7 * 16 + c] = (uint8_t)p;
    }
    for (int a = 0; a < 128; a++) out[a] = raw[(a % 8) * 16 + a / 8];
}

static bool elc_decode(const uint8_t in[128], uint8_t lc[9]) {
    uint8_t raw[128], cs = 0;

    for (int a = 0; a < 128; a++) raw[(a % 8) * 16 + a / 8] = in[a];
    for (int r = 0; r < 7; r++) ham_fix(raw + r * 16, 16, 11, h16);
    memset(lc, 0, 9);
    for (int i = 0; i < 72; i++) lc[i / 8] |= (uint8_t)(raw[elc_place(i)] << (7 - i % 8));
    for (int j = 0; j < 5; j++) cs = (uint8_t)(cs << 1 | raw[(2 + j) * 16 + 10]);
    return cs == lc_sum(lc);
}

/* A full LC from a data burst's 196 bits: the header's (mask 0x96) or the terminator's (0x99), 0 neither */
static int full_lc(const uint8_t info[196], uint8_t lc[9]) {
    for (int fix = 0; fix < 2; fix++) {
        uint8_t bits[96], b[12] = {0}, par[3];

        bptc_decode(info, bits, fix);
        for (int i = 0; i < 96; i++) b[i / 8] |= (uint8_t)(bits[i] << (7 - i % 8));
        dmr_rs129(b, par);
        for (int k = 0; k < 2; k++) {
            uint8_t mask = k ? 0x99 : 0x96;

            if ((b[9] ^ mask) == par[0] && (b[10] ^ mask) == par[1] && (b[11] ^ mask) == par[2]) {
                memcpy(lc, b, 9);
                return k ? 2 : 1;
            }
        }
    }
    return 0;
}

/* ---- The modem ---- */

static float rrc(double t) {           /* t in symbols; roll-off 0.2 */
    const double a = 0.2;

    if (fabs(t) < 1e-9) return (float)(1 - a + 4 * a / M_PI);
    if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9)
        return (float)(a / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * cos(M_PI / (4 * a))));
    return (float)((sin(M_PI * t * (1 - a)) + 4 * a * t * cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - pow(4 * a * t, 2))));
}

dmr_t *dmr_new(float rate, dmr_ambe_cb ambe, dmr_call_cb call, void *ctx) {
    dmr_t *d = calloc(1, sizeof(*d));
    float  sum = 0;

    if (!d) return NULL;
    gf_init();
    /* the filter at the input's rate, its output every other sample above 30 kHz: 4.6 samples a symbol at 44.1 kHz
     * are plenty, and the search costs half (DMR was 10% of a core of the radio more than NFM alone with every one) */
    double in_sps = rate / SYM_RATE;

    d->dec_n = rate > 30000 ? 2 : 1;
    d->rate  = rate / d->dec_n;
    d->sps   = d->rate / SYM_RATE;
    d->ntaps = 2 * (int)ceil(RRC_SPAN * in_sps) + 1;
    if (d->ntaps > RRC_MAX) d->ntaps = RRC_MAX | 1;
    for (int i = 0; i < d->ntaps; i++) sum += d->taps[i] = rrc((i - d->ntaps / 2) / in_sps);
    for (int i = 0; i < d->ntaps; i++) d->taps[i] /= sum;
    for (int k = 0; k < 24; k++) {     /* t - (23 - k) * sps, for a whole t: its sample before and how far past it */
        double a = (23 - k) * d->sps;

        d->s_off[k]  = (int)ceil(a);
        d->s_frac[k] = (float)(d->s_off[k] - a);
    }
    for (int p = 0; p < P_N; p++)
        for (int k = 0; k < 24; k++) {
            d->sgn[p][k] = (signed char)(sym_of[sync_pat[p] >> (46 - 2 * k) & 3] / 3);
            if (d->sgn[p][k] > 0) d->pmask[p] |= 1u << k;
        }
    d->ambe  = ambe;
    d->callf = call;
    d->ctx   = ctx;
    for (int s = 0; s < 3; s++) {
        d->sl[s].call.slot = s;
        d->sl[s].call.cc   = -1;
        d->sl[s].elc_n     = -1;
    }
    return d;
}

void dmr_free(dmr_t *d) { free(d); }

/* The filtered signal at a sample position, between samples linearly */
static float at(const dmr_t *d, double pos) {
    int64_t i = (int64_t)floor(pos);
    float   f = (float)(pos - (double)i);

    if (i < 0 || (uint64_t)i + 1 >= d->n || d->n - (uint64_t)i >= RING) return 0;
    return d->ring[i & (RING - 1)] * (1 - f) + d->ring[(i + 1) & (RING - 1)] * f;
}

/* The 24 symbols of a sync ending at pos, their DC off; their mean magnitude; their signs, bit k for x[k] > 0 */
static float sync_syms(const dmr_t *d, double pos, float x[24], uint32_t *signs) {
    float    mean = 0, sa = 0;
    uint32_t m    = 0;

    if (pos == floor(pos) && pos + 1 < (double)d->n && pos > d->s_off[0]) {    /* a whole sample: the offsets made */
        uint64_t t = (uint64_t)pos;

        for (int k = 0; k < 24; k++) {
            uint64_t i = t - (uint64_t)d->s_off[k];

            mean += x[k] = d->ring[i & (RING - 1)] * (1 - d->s_frac[k]) + d->ring[(i + 1) & (RING - 1)] * d->s_frac[k];
        }
    } else {
        for (int k = 0; k < 24; k++) mean += x[k] = at(d, pos - (23 - k) * d->sps);
    }
    mean /= 24;
    for (int k = 0; k < 24; k++) {
        sa += fabsf(x[k] -= mean);
        if (x[k] > 0) m |= 1u << k;
    }
    if (signs) *signs = m;
    return sa / 24;
}

/* Their match with sync p over their mean magnitude (1 a perfect one: every sign right, wherever between the symbols
 * it is taken); raw: the match itself, largest at the symbols' centres (where the peak is looked for); bad: the
 * symbols of the wrong sign */
static float sync_match(const dmr_t *d, const float x[24], float level, int p, int *bad, float *raw) {
    float dot = 0;

    *bad = 0;
    for (int k = 0; k < 24; k++) {
        float v = x[k] * d->sgn[p][k];

        dot += v;
        if (v < 0) (*bad)++;
    }
    *raw = dot / 24;
    return level > 0 ? dot / (24 * level) : 0;
}

static float sync_raw(const dmr_t *d, double pos, int p) {
    float x[24], level = sync_syms(d, pos, x, NULL), raw;
    int   bad;

    sync_match(d, x, level, p, &bad, &raw);
    return raw;
}

static void emit(dmr_t *d, slot_t *s) {
    if (d->callf) d->callf(d->ctx, &s->call);
}

static void voice_end(dmr_t *d, slot_t *s) {
    if (!s->voice) return;
    s->voice      = false;
    s->call.voice = false;
    emit(d, s);
    s->call.lc = false;
}

/* A voice call's link control into the slot's call: false another kind (talker alias, GPS...) */
static bool set_lc(slot_t *s, const uint8_t lc[9]) {
    int flco = lc[0] & 0x3F;

    if (flco != 0 && flco != 3) return false;
    s->call.lc   = true;
    s->call.flco = flco;
    s->call.dst  = (uint32_t)lc[3] << 16 | (uint32_t)lc[4] << 8 | lc[5];
    s->call.src  = (uint32_t)lc[6] << 16 | (uint32_t)lc[7] << 8 | lc[8];
    return true;
}

static void put_lc(dmr_t *d, slot_t *s, const uint8_t lc[9], bool voice) {
    dmr_call_t was = s->call;

    if (!set_lc(s, lc)) return;
    s->call.voice = voice;
    if (!was.lc || was.flco != s->call.flco || was.dst != s->call.dst || was.src != s->call.src || was.voice != voice)
        emit(d, s);
}

/* The 72 bits of a voice burst's frame: dibits [a, a + 36), the second split round the sync */
static void frame_of(const uint8_t *db, int which, uint8_t out[9]) {
    static const int first[3] = {0, 36, 96};

    memset(out, 0, 9);
    for (int k = 0; k < 36; k++) {
        int i = first[which] + k;

        if (which == 1 && k >= 18) i = 78 + (k - 18);
        out[k / 4] |= (uint8_t)(db[i] << (6 - 2 * (k % 4)));
    }
}

static void burst(dmr_t *d, double start, int pat) {
    uint8_t db[BURST], cach[12];
    int     slot;

    d->bursts++;
    if (pat != P_NONE) {                /* the levels from its own sync */
        float pos = 0, neg = 0;
        int   np = 0, nn = 0;

        for (int k = 0; k < 24; k++) {
            float v = at(d, start + (SYNC_AT + k) * d->sps);

            if (d->sgn[pat][k] > 0) pos += v, np++;
            else neg += v, nn++;
        }
        if (np && nn) {
            d->centre = (pos / np + neg / nn) / 2;
            d->unit   = (pos / np - neg / nn) / 6;
        }
        d->bs = pat == P_BS_VOICE || pat == P_BS_DATA;
    }
    if (d->unit <= 0) return;
    for (int k = -12; k < BURST; k++) {
        float   v = (at(d, start + k * d->sps) - d->centre) / d->unit;
        uint8_t b = v > 2 ? 1 : v > 0 ? 0 : v > -2 ? 2 : 3;

        if (k < 0) cach[k + 12] = b;
        else db[k] = b;
    }
    if (pat == P_TS1_VOICE || pat == P_TS1_DATA) slot = 1;
    else if (pat == P_TS2_VOICE || pat == P_TS2_DATA) slot = 2;
    else if (d->bs) slot = (cach[2] >> 1 & 1) + 1;     /* the CACH's TC, its 5th bit */
    else slot = d->prev_slot ? 3 - d->prev_slot : 1;
    d->prev_slot = slot;

    slot_t *s = &d->sl[slot];

    s->call.bs = d->bs;
    if (pat != P_NONE && !pat_voice(pat)) {             /* data: its slot type, then a header or a terminator */
        uint8_t info[196], lc[9];
        int     k;

        s->call.cc = db[49] << 2 | db[50];
        for (k = 0; k < 98; k++) info[k] = db[k / 2] >> (1 - k % 2) & 1;
        for (k = 0; k < 98; k++) info[98 + k] = db[83 + k / 2] >> (1 - k % 2) & 1;
        int what = full_lc(info, lc);

        if (what) d->lcs++;
        if (what == 1) {                /* a header (two or three of them): the call starts */
            if (!s->voice) s->vf = -1;
            s->voice      = true;
            s->last_voice = start;
            put_lc(d, s, lc, true);
            return;
        }
        if (what == 2 && s->voice) set_lc(s, lc);    /* a terminator (two or three): who it was, then the end */
        voice_end(d, s);                /* it, an idle or a control burst: no voice */
        return;
    }
    if (pat != P_NONE) {                /* voice A */
        if (!s->voice || !s->call.voice) {
            s->voice      = true;
            s->call.voice = true;
            emit(d, s);
        }
        s->vf    = 0;
        s->elc_n = -1;
    } else {                            /* B-F, expected */
        if (!s->voice || start - s->last_voice > 1.5 * 2 * SLOT_SYM * d->sps) {
            if (s->voice && start - s->last_voice > 6 * 2 * SLOT_SYM * d->sps) voice_end(d, s);
            return;
        }
        if (++s->vf > 6) {              /* A twice missed: not voice any more */
            voice_end(d, s);
            return;
        }
        int emb  = db[54] << 6 | db[55] << 4 | db[56] << 2 | db[57];      /* CC, PI, LCSS: its first 7 bits */
        int lcss = emb >> 1 & 3;

        s->call.cc = emb >> 4;
        if (lcss == 1) s->elc_n = 0;    /* the first piece */
        if ((lcss == 1 || lcss == 3 || lcss == 2) && s->elc_n >= 0 && s->elc_n <= 96) {
            for (int k = 0; k < 32; k++) s->elc[s->elc_n + k] = db[58 + k / 2] >> (1 - k % 2) & 1;
            s->elc_n += 32;
            if (lcss == 2) {
                uint8_t lc[9];

                if (s->elc_n == 128 && elc_decode(s->elc, lc)) {
                    d->elcs++;
                    put_lc(d, s, lc, true);
                }
                s->elc_n = -1;
            }
        } else if (lcss != 0) {
            s->elc_n = -1;
        }
    }
    s->last_voice = start;
    for (int f = 0; f < 3; f++) {
        uint8_t fr[9];

        frame_of(db, f, fr);
        if (d->ambe) d->ambe(d->ctx, slot, fr);
    }
}

static void queue(dmr_t *d, double start, int pat) {
    if (d->qn < QUEUE) d->q[d->qn++] = (pend_t){start, pat};
}

static void sync_commit(dmr_t *d, double end, int pat) {
    double start = end - (SYNC_AT + 23) * d->sps;

    d->syncs++;
    d->last_sync = end;
    for (int i = 0; i < d->qn; i++)     /* one expected there: this is it */
        if (fabs(d->q[i].start - start) < d->sps) {
            d->q[i].start = start;
            d->q[i].pat   = pat;
            d->next_bs    = start + SLOT_SYM * d->sps;
            return;
        }
    queue(d, start, pat);
    d->next_bs = start + SLOT_SYM * d->sps;
    d->locked  = true;
}

static void step(dmr_t *d) {
    double t = (double)d->n - 2, expect = d->next_bs + (SYNC_AT + 23) * d->sps;   /* t: a sample behind the newest,
                                                                                         * which at() reads up to */
    bool   near = d->locked && fabs(t - expect) < 4;
    float    x[24], level;
    uint32_t signs;
    int      ok_bad = near ? 2 : 0;

    /* every other sample while nothing is near with more than 8 samples a symbol: a sync's signs hold for more than a
     * sample either side of its peak, which is then looked for at every one */
    if (d->hold > 0) d->hold--;
    else if (t > 26 * d->sps + 2 && (d->cand_c > 0 || near || d->sps < 8 || (d->n & 1)) &&
             (level = sync_syms(d, t, x, &signs)) >= MIN_LEVEL) {
        for (int p = 0; p < P_N; p++) {
            int   bad;
            float raw, c;

            if (__builtin_popcount((signs ^ d->pmask[p]) & 0xFFFFFF) > ok_bad) continue;
            c = sync_match(d, x, level, p, &bad, &raw);
            if (bad <= ok_bad && c >= (near ? 0.6f : 0.75f) && raw > d->cand_c) {
                d->cand_c    = raw;
                d->cand_at   = t;
                d->cand_p    = p;
                d->cand_left = (int)ceil(d->sps / 2);
            }
        }
    }
    if (d->cand_c > 0 && --d->cand_left <= 0) {         /* the peak: refined between samples */
        float  l = sync_raw(d, d->cand_at - 0.5, d->cand_p), r = sync_raw(d, d->cand_at + 0.5, d->cand_p),
               den = l - 2 * d->cand_c + r;
        double off = den < 0 ? 0.25 * (l - r) / den : 0;

        sync_commit(d, d->cand_at + (off > 0.5 ? 0.5 : off < -0.5 ? -0.5 : off), d->cand_p);
        d->cand_c = 0;
        d->hold   = (int)(12 * d->sps);    /* the same sync not found again a few samples on */
    }
    if (d->locked && t > d->next_bs + (BURST + 2) * d->sps) {     /* the burst after: no sync there */
        queue(d, d->next_bs, P_NONE);
        d->next_bs += SLOT_SYM * d->sps;
    }
    if (d->locked && t - d->last_sync > LOCK_S * d->rate) {
        d->locked = false;
        for (int s = 1; s <= 2; s++) voice_end(d, &d->sl[s]);
    }
    for (int i = 0; i < d->qn;) {       /* those whole: read */
        if (t > d->q[i].start + (BURST + 1) * d->sps) {
            pend_t p = d->q[i];

            memmove(&d->q[i], &d->q[i + 1], sizeof(d->q[0]) * (size_t)(d->qn - i - 1));
            d->qn--;
            burst(d, p.start, p.pat);
        } else {
            i++;
        }
    }
}

void dmr_feed(dmr_t *d, const float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        float y = 0;

        d->hist[d->hat] = d->hist[d->hat + d->ntaps] = x[i];
        d->hat = d->hat + 1 == d->ntaps ? 0 : d->hat + 1;
        if (++d->dec < d->dec_n) continue;
        d->dec = 0;
        const float *h = d->hist + d->hat;
        int          k = 0;
#if defined(__ARM_NEON)
        float32x4_t acc = vdupq_n_f32(0);

        for (; k + 4 <= d->ntaps; k += 4) acc = vmlaq_f32(acc, vld1q_f32(h + k), vld1q_f32(d->taps + k));
        y = vgetq_lane_f32(acc, 0) + vgetq_lane_f32(acc, 1) + vgetq_lane_f32(acc, 2) + vgetq_lane_f32(acc, 3);
#else
        float y0 = 0, y1 = 0, y2 = 0, y3 = 0;

        for (; k + 4 <= d->ntaps; k += 4) {
            y0 += h[k] * d->taps[k];
            y1 += h[k + 1] * d->taps[k + 1];
            y2 += h[k + 2] * d->taps[k + 2];
            y3 += h[k + 3] * d->taps[k + 3];
        }
        y = y0 + y1 + y2 + y3;
#endif
        for (; k < d->ntaps; k++) y += h[k] * d->taps[k];
        d->ring[d->n & (RING - 1)] = y;
        d->n++;
        step(d);
    }
}

bool dmr_active(const dmr_t *d) { return d->syncs && (double)d->n - d->last_sync < 0.5 * d->rate; }

void dmr_counts(const dmr_t *d, uint32_t *syncs, uint32_t *bursts, uint32_t *lcs, uint32_t *elcs) {
    if (syncs) *syncs = d->syncs;
    if (bursts) *bursts = d->bursts;
    if (lcs) *lcs = d->lcs;
    if (elcs) *elcs = d->elcs;
}
