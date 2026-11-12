/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - NAVTEX (SITOR-B, CCIR 476 FEC) receiver, see sitorb.h
 *
 *  Chain, per audio sample (11025 Hz):
 *    - two quadrature mixers bring mark (1085 Hz) and space (915 Hz) to 0 Hz;
 *    - a raised-cosine low pass, done as a 512-point block transform on 256-sample
 *      blocks (overlap-add, only the 11 bins around 0 Hz are non-zero, so they are
 *      computed directly instead of with a full FFT);
 *    - envelope and noise floor tracked on each tone, then the mark/space difference
 *      with automatic threshold: its sign is the bit (the DSC receiver's);
 *    - bit timing: the weight is averaged at three points of the bit (early, prompt
 *      and late, 1/5 of a bit apart), every 8 bits the sampling instant moves towards
 *      the side with more energy;
 *    - soft bit for the framing: mark power minus space power, each tone held between
 *      its noise floor and its envelope (a burst of static cannot outweigh a character);
 *    - framing: each character is sent twice, DX then RX 5 characters (35 bits) later.
 *      Every bit ends a character in one of 14 phases (7 bit offsets, DX or RX slot);
 *      each phase is scored by how well its characters read as an RX copy matching
 *      its DX copy, and the best phase is followed. A character is read from both
 *      copies at once: the code (of the 35 with 4 ones in 7) that agrees most with
 *      the sum of their soft bits, so a copy hit by a fade or a burst is outvoted.
 */

#include "sitorb.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define RATE            11025
#define BAUD            100.0
#define TONE_MARK       1085.0          /* 1000 Hz centre, +-85 Hz */
#define TONE_SPACE      915.0

#define BLOCK           256             /* new samples per block transform */
#define NFFT            (2 * BLOCK)     /* zero padded transform length */
#define NBINS           5               /* bins -5..+5 pass the low pass, all others are 0 */

#define NBUF            64              /* soft bits kept, a power of 2 (a character and its DX copy fit) */
#define FEC_GAP         35              /* the DX copy comes 5 characters (35 bits) before the RX one */
#define PHASES          14              /* 7 bit offsets, each a DX or an RX slot */

/* Phase scores: the average agreement of a phase's characters (read_pair()) */
#define SCORE_AVG       8               /* characters averaged */
#define SCORE_NOISE     0.65            /* what noise scores, the start value */
#define LOCK_SCORE      0.8             /* to lock, a phase needs this ... */
#define LOCK_MARGIN     0.15            /* ... and this much over the mean of all 14 */
#define SWITCH_MARGIN   0.1             /* locked: another phase this much better takes over */
#define PRINT_SCORE     0.75            /* locked phase under this: nothing printed ... */
#define LOST_CHARS      10              /* ... and after more such characters, hunt again */
#define PRINT_AGREE     0.6             /* a character agreeing less is not printed */
#define END_ALPHAS      3               /* ALPHA in both copies this many times: end of emission */
#define ODD_FIGURES     2               /* rare figures after FIGS: this many, back to letters */

/* CCIR 476 control codes */
#define C_LTRS          0x5a
#define C_FIGS          0x36
#define C_ALPHA         0x0f
#define C_BETA          0x33
#define C_CHAR32        0x6a
#define C_REP           0x66
#define PHASING         0x80            /* not a code: read_pair() saw the phasing pair */

typedef struct {
    double re, im;
} cpx_t;

/* Low pass of one tone: collects BLOCK samples, outputs BLOCK filtered samples */
typedef struct {
    cpx_t   in[BLOCK];
    cpx_t   tail[BLOCK];                /* second half of the previous block's output */
    cpx_t   out[BLOCK];
    int     fill;
} tone_lpf_t;

struct navtex_decoder {
    bool        reverse;
    double      tone_mark, tone_space;  /* Hz: TONE_MARK and TONE_SPACE, moved by the options' offset */
    double      print_score, print_agree;   /* PRINT_SCORE and PRINT_AGREE, lower when loose */

    double      mark_phase, space_phase;
    tone_lpf_t  mark_lpf, space_lpf;

    double      mark_env, space_env;
    double      mark_noise, space_noise;

    double      n;                      /* filtered samples so far (a double counts exactly for 25000 years) */
    int         tick;                   /* n modulo 8 bits, for adjust_timing() */
    double      spb;                    /* samples per bit: 110.25 */
    double      t_early, t_prompt, t_late;
    double      e_early, e_prompt, e_late;

    double      bits[NBUF];             /* soft bits, a ring: positive = mark, the size is the weight */
    unsigned long long nbits;           /* bits taken so far */
    double      score[PHASES];          /* per phase (nbits % PHASES at the end of a character) */
    int         phase;                  /* the phase read, -1 while hunting */
    int         lost;                   /* characters in a row with the phase under PRINT_SCORE */
    int         alphas;                 /* ALPHA pairs in a row */
    bool        figs;
    int         odd_figs;               /* rare figures since FIGS */

    double      bit_on, bit_off;        /* the last bit's powers: the tone on (the stronger), the one off (demod) */
    double      on_sum, off_sum;        /* this transmission's, for its signal to noise */
    navtex_stats_t st, last;            /* navtex_decoder_get_stats(): the one read now, the last one */

    navtex_char_cb_t char_cb;
    void            *char_ud;
    void           (*bit_cb)(int mark, void *userdata);
    void            *bit_ud;
};

/* ---- low pass ------------------------------------------------------------ */

static cpx_t   cos_sin[NFFT];           /* cos and sin of 2 pi m / NFFT */
static cpx_t   lpf_gain[2 * NBINS + 1]; /* bins -NBINS..+NBINS */
static bool    tables_ready;

/*
 * Raised cosine (cos^2) shaped on the bit rate with a 1.2 widening, divided by
 * sinc to flatten the Nyquist channel, and a delay of NFFT / 4 samples. Bins past
 * +-5 get 0. The 1.2 (it was 1.4) lets less noise in for a little more overlap
 * between bits: both SITOR-B and DSC decode weaker signals with it.
 */
static void make_tables(void) {
    double width = BAUD / RATE * 1.2;

    for (int m = 0; m < NFFT; m++) {
        cos_sin[m].re = cos(2.0 * M_PI * m / NFFT);
        cos_sin[m].im = sin(2.0 * M_PI * m / NFFT);
    }
    for (int k = 0; k <= NBINS; k++) {
        double x = (double)k / BLOCK;
        double g = x <= 0 ? 1.0 : x > 2.0 * width ? 0.0 : cos(M_PI * x / (width * 4.0));

        g *= g;
        if (k) {
            double x2 = 2.0 * k * width;
            g /= sin(M_PI * x2) / (M_PI * x2);
        }
        /* delay: -k pi/2 on bin k, +k pi/2 on bin -k */
        double a = (double)k * -0.5 * M_PI, b = (double)k * 0.5 * M_PI;

        /* 1 / NFFT of the inverse transform folded in (a power of 2: exact) */
        lpf_gain[NBINS + k] = (cpx_t){g * cos(a) / NFFT, g * sin(a) / NFFT};
        lpf_gain[NBINS - k] = (cpx_t){g * cos(b) / NFFT, g * sin(b) / NFFT};
    }
    tables_ready = true;
}

static inline cpx_t cmul(cpx_t a, cpx_t b) {
    return (cpx_t){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

/* Adds one sample; returns true when out[] holds BLOCK new filtered samples */
static bool lpf_push(tone_lpf_t *f, cpx_t v) {
    f->in[f->fill++] = v;
    if (f->fill < BLOCK)
        return false;
    f->fill = 0;

    /*
     * Spectrum of the zero padded block on the pass bins, times the filter. Bins k and -k
     * share the cosine and sine sums: X(+-k) = C -+ jS.
     */
    cpx_t spec[2 * NBINS + 1];
    cpx_t c0 = {0, 0};

    for (int i = 0; i < BLOCK; i++) {
        c0.re += f->in[i].re;
        c0.im += f->in[i].im;
    }
    spec[NBINS] = cmul(c0, lpf_gain[NBINS]);

    for (int k = 1; k <= NBINS; k++) {
        cpx_t c = {0, 0}, s = {0, 0};

        for (int i = 0; i < BLOCK; i++) {
            cpx_t w = cos_sin[(k * i) & (NFFT - 1)];

            c.re += f->in[i].re * w.re;
            c.im += f->in[i].im * w.re;
            s.re += f->in[i].re * w.im;
            s.im += f->in[i].im * w.im;
        }
        spec[NBINS + k] = cmul((cpx_t){c.re + s.im, c.im - s.re}, lpf_gain[NBINS + k]);
        spec[NBINS - k] = cmul((cpx_t){c.re - s.im, c.im + s.re}, lpf_gain[NBINS - k]);
    }

    /* back to time, NFFT points: the first half plus the previous tail is the output */
    for (int i = 0; i < NFFT; i++) {
        double re = spec[NBINS].re, im = spec[NBINS].im;

        for (int k = 1; k <= NBINS; k++) {
            cpx_t w = cos_sin[(k * i) & (NFFT - 1)];
            cpx_t p = spec[NBINS + k], m = spec[NBINS - k];

            /* Z(k) e^(j th) + Z(-k) e^(-j th) */
            re += (p.re + m.re) * w.re - (p.im - m.im) * w.im;
            im += (p.im + m.im) * w.re + (p.re - m.re) * w.im;
        }
        if (i < BLOCK) {
            f->out[i].re = f->tail[i].re + re;
            f->out[i].im = f->tail[i].im + im;
        } else {
            f->tail[i - BLOCK] = (cpx_t){re, im};
        }
    }
    return true;
}

/* ---- CCIR 476 ------------------------------------------------------------ */

/* The 29 printing codes: letters and figures case */
static const struct {
    unsigned char code;
    char          ltr, fig;
} ccir476[] = {
    {0x17, 'J', '\''}, {0x1b, 'F', '!'}, {0x1d, 'C', ':'}, {0x1e, 'K', '('},
    {0x27, 'W', '2'},  {0x2b, 'Y', '6'}, {0x2d, 'P', '0'}, {0x2e, 'Q', '1'},
    {0x35, 'G', '&'},  {0x39, 'M', '.'}, {0x3a, 'X', '/'}, {0x3c, 'V', ';'},
    {0x47, 'A', '-'},  {0x4b, 'S', 7},   {0x4d, 'I', '8'}, {0x4e, 'U', '7'},
    {0x53, 'D', '$'},  {0x55, 'R', '4'}, {0x56, 'E', '3'}, {0x59, 'N', ','},
    {0x5c, ' ', ' '},  {0x63, 'Z', '"'}, {0x65, 'L', ')'}, {0x69, 'H', '#'},
    {0x6c, '\n', '\n'}, {0x71, 'O', '9'}, {0x72, 'B', '?'}, {0x74, 'T', '5'},
    {0x78, '\r', '\r'},
};

static int ccir_char(int code, bool figs) {
    for (size_t i = 0; i < sizeof(ccir476) / sizeof(ccir476[0]); i++)
        if (ccir476[i].code == code)
            return figs ? ccir476[i].fig : ccir476[i].ltr;
    return -1;
}

/* The 35 valid codes: exactly 4 of 7 bits set */
static const unsigned char valid_codes[] = {
    0x0f, 0x17, 0x1b, 0x1d, 0x1e, 0x27, 0x2b, 0x2d, 0x2e, 0x33, 0x35, 0x36,
    0x39, 0x3a, 0x3c, 0x47, 0x4b, 0x4d, 0x4e, 0x53, 0x55, 0x56, 0x59, 0x5a,
    0x5c, 0x63, 0x65, 0x66, 0x69, 0x6a, 0x6c, 0x71, 0x72, 0x74, 0x78,
};

/* ---- framing ------------------------------------------------------------- */

static void emit(navtex_decoder_t *d, int c) {
    if (c == 7)
        c = '\'';           /* BELL: French stations use it as an apostrophe */
    else if (c == '\r')
        return;
    if (d->char_cb)
        d->char_cb(c, d->char_ud);
}

/* Figures a warning hardly ever has: a misread FIGS turns letters into these */
static bool odd_figure(int c) {
    return c == '!' || c == '&' || c == '#' || c == '$' || c == ';' || c == '"' || c == 7;
}

static void take_code(navtex_decoder_t *d, int code) {
    switch (code) {
        case PHASING:
        case C_REP:
        case C_ALPHA:
        case C_BETA:
        case C_CHAR32:
            break;
        case C_LTRS:
            d->figs = false;
            break;
        case C_FIGS:
            d->figs = true;
            d->odd_figs = 0;
            break;
        default: {
            int c = ccir_char(code, d->figs);

            /* a missed LTRS would print figures until the next one: give up on them early */
            if (d->figs && odd_figure(c) && ++d->odd_figs >= ODD_FIGURES) {
                d->figs = false;
                c = ccir_char(code, false);
            }
            if (c >= 0)
                emit(d, c);
            break;
        }
    }
}

static inline double bit_at(const navtex_decoder_t *d, unsigned long long i) {
    return d->bits[i % NBUF];
}

/*
 * Reads the character whose RX copy ends at bit `end` (bits end-7 .. end-1, the first
 * received is bit 0 of the code) with its DX copy 35 bits earlier: of the 35 codes, the
 * one whose ones and zeros agree most with the soft bits of both copies added up. A
 * strong copy outvotes a faded one, a bit lost in one copy is set by the other. The
 * phasing pair (ALPHA as RX, REP as DX, BETA from some encoders) is one more choice.
 * *agree: the agreement over the total weight, 1 when every bit of both copies fits,
 * about 0.65 on noise.
 */
/* One copy as received, bit by bit on its own: 7 bits ending at `end`, bit 0 first */
static int hard_code(const navtex_decoder_t *d, unsigned long long end) {
    int code = 0;

    for (unsigned i = 0; i < 7; i++)
        if (bit_at(d, end - 7 + i) > 0)
            code |= 1 << i;
    return code;
}

/* A character of the transmission locked on, for navtex_decoder_get_stats() */
static void count_char(navtex_decoder_t *d, int code, bool printed) {
    if (code == PHASING || code == C_ALPHA || code == C_REP || code == C_BETA)
        return;
    d->st.chars++;
    if (!printed)
        d->st.lost++;
    else if (hard_code(d, d->nbits) == code && hard_code(d, d->nbits - FEC_GAP) == code)
        d->st.clean++;
    else
        d->st.repaired++;
}

/* Signal to noise in a tone's filter: the tone on over the one off, less the off one's noise (on noise alone the
 * stronger of two is three times the weaker on average: about 3 dB comes out there, not 0) */
static float snr_of(double on, double off) {
    return off > 0 ? (float)(10 * log10(fmax(on - off, off * 1e-3) / off)) : 0;
}

/* The transmission is over (or was noise): kept as the last one when it had characters */
static void end_of(navtex_decoder_t *d) {
    d->phase        = -1;
    d->st.receiving = 0;
    if (d->st.chars)
        d->last = d->st;
}

static int read_pair(const navtex_decoder_t *d, unsigned long long end, double *agree) {
    double sum[7], all = 0, total = 0, alpha = 0, rep = 0, beta = 0;

    for (unsigned i = 0; i < 7; i++) {
        double rx = bit_at(d, end - 7 + i);
        double dx = bit_at(d, end - 7 - FEC_GAP + i);

        sum[i] = rx + dx;
        all += sum[i];
        total += fabs(rx) + fabs(dx);
        alpha += C_ALPHA >> i & 1 ? rx : -rx;
        rep += C_REP >> i & 1 ? dx : -dx;
        beta += C_BETA >> i & 1 ? dx : -dx;
    }

    int    best = PHASING;
    double best_fit = alpha + fmax(rep, beta);

    for (size_t k = 0; k < sizeof(valid_codes); k++) {
        double fit = -all;              /* + the sum on the code's ones, - on its zeros */

        for (int i = 0; i < 7; i++)
            if (valid_codes[k] >> i & 1)
                fit += 2 * sum[i];
        if (fit > best_fit) {
            best_fit = fit;
            best = valid_codes[k];
        }
    }
    *agree = total > 0 ? best_fit / total : 0;
    return best;
}

/*
 * Every bit ends a character in one of the 14 phases: read as an RX copy with its DX
 * copy, its agreement goes into that phase's score. Hunting, the best phase is taken
 * once it stands out; locked, a clearly better phase takes over (a bit slip moves the
 * phase by one), and ALPHA in both copies (end of emission) or a phase that keeps
 * scoring like noise (the signal is gone) sends the receiver back to hunting.
 */
static void take_bit(navtex_decoder_t *d, double weight) {
    d->bits[d->nbits++ % NBUF] = weight;
    if (d->phase >= 0) {
        d->on_sum += d->bit_on;
        d->off_sum += d->bit_off;
        d->st.snr_db = snr_of(d->on_sum, d->off_sum);
    }
    if (d->nbits < FEC_GAP + 7)
        return;

    int    now = d->nbits % PHASES;
    double agree;
    int    code = read_pair(d, d->nbits, &agree);

    d->score[now] += (agree - d->score[now]) / SCORE_AVG;

    int    best = 0;
    double mean = 0;

    for (int i = 0; i < PHASES; i++) {
        mean += d->score[i] / PHASES;
        if (d->score[i] > d->score[best])
            best = i;
    }

    if (d->phase < 0) {
        if (d->score[best] < LOCK_SCORE || d->score[best] - mean < LOCK_MARGIN)
            return;
        d->phase = best;
        d->lost = 0;
        d->alphas = 0;
        d->figs = false;
        memset(&d->st, 0, sizeof(d->st));   /* a new transmission */
        d->on_sum = d->off_sum = 0;
        d->st.receiving = 1;
        return;
    }
    if (best != d->phase && d->score[best] > d->score[d->phase] + SWITCH_MARGIN)
        d->phase = best;
    if (now != d->phase)
        return;

    if (d->score[now] < d->print_score) {
        if (++d->lost > LOST_CHARS) {
            end_of(d);
            return;
        }
    } else {
        d->lost = 0;
    }
    if (code == C_ALPHA) {
        if (++d->alphas >= END_ALPHAS) {
            end_of(d);
            return;
        }
    } else {
        d->alphas = 0;
    }
    bool printed = d->score[now] >= d->print_score && agree > d->print_agree;

    count_char(d, code, printed);
    if (printed)
        take_code(d, code);
}

/* ---- demodulator --------------------------------------------------------- */

static inline double smooth(double avg, double v, int weight) {
    return weight <= 1 ? v : (v - avg) / weight + avg;
}

/* envelope: up fast, down slow; noise floor: down fast, up slow */
static inline double track_env(const navtex_decoder_t *d, double avg, double v) {
    return smooth(avg, v, v > avg ? (int)(d->spb / 4) : (int)(d->spb * 16));
}

static inline double track_noise(const navtex_decoder_t *d, double avg, double v) {
    return smooth(avg, v, v < avg ? (int)(d->spb / 4) : (int)(d->spb * 48));
}

/* Every 8 bits: move the sampling instants towards the stronger side */
static void adjust_timing(navtex_decoder_t *d) {
    bool now = d->tick == 0;

    if (++d->tick >= (int)(d->spb * 8))
        d->tick = 0;
    if (!now)
        return;

    double step = (d->e_late - d->e_early) / (d->e_prompt + 0.1) * 22;

    if (d->e_prompt > d->e_early && d->e_prompt > d->e_late)
        step /= 2;
    d->t_early += step;
    d->t_prompt += step;
    d->t_late += step;
}

static void demod(navtex_decoder_t *d, const cpx_t *mark, const cpx_t *space) {
    for (int i = 0; i < BLOCK; i++) {
        double m = hypot(mark[i].re, mark[i].im);
        double s = hypot(space[i].re, space[i].im);

        adjust_timing(d);

        d->mark_env = track_env(d, d->mark_env, m);
        d->mark_noise = track_noise(d, d->mark_noise, m);
        d->space_env = track_env(d, d->space_env, s);
        d->space_noise = track_noise(d, d->space_noise, s);

        double floor = (d->space_noise + d->mark_noise) / 2;

        m = fmax(fmin(m, d->mark_env), floor);
        s = fmax(fmin(s, d->space_env), floor);

        /* mark minus space, each weighted by its own envelope (automatic threshold) */
        double me = d->mark_env - floor, se = d->space_env - floor;
        double level = (m - floor) * me - (s - floor) * se - 0.5 * (me * me - se * se);
        double mag = fabs(level);

        if (d->n >= d->t_early) {
            d->e_early = smooth(d->e_early, mag, 64);
            d->t_early += d->spb;
        }
        if (d->n >= d->t_late) {
            d->e_late = smooth(d->e_late, mag, 64);
            d->t_late += d->spb;
        }
        bool bit_now = d->n >= d->t_prompt;

        if (bit_now) {
            d->e_prompt = smooth(d->e_prompt, mag, 64);
            d->t_prompt += d->spb;
        }

        if (bit_now) {
            /* the tones' powers as received (not clipped), for the signal to noise */
            double mp = mark[i].re * mark[i].re + mark[i].im * mark[i].im;
            double sp = space[i].re * space[i].re + space[i].im * space[i].im;

            d->bit_on  = fmax(mp, sp);
            d->bit_off = fmin(mp, sp);
            if (d->bit_cb)
                d->bit_cb(level > 0, d->bit_ud);
            take_bit(d, m * m - s * s);     /* the clipped tones' powers */
        }
        d->n++;
    }
}

/* Mixes a sample down by the tone; the real sample goes on both I and Q */
static inline cpx_t mix(double *phase, double tone, double v) {
    double c = cos(*phase), s = sin(*phase);

    *phase -= 2.0 * M_PI * tone / RATE;
    if (*phase < -2.0 * M_PI)
        *phase += 2.0 * M_PI;
    return (cpx_t){c * v - s * v, c * v + s * v};
}

/* ---- API ----------------------------------------------------------------- */

navtex_decoder_t *navtex_decoder_create(int reverse) {
    navtex_options_t o = {.reverse = reverse};

    return navtex_decoder_create_ex(&o);
}

navtex_decoder_t *navtex_decoder_create_ex(const navtex_options_t *o) {
    if (!tables_ready)
        make_tables();

    navtex_decoder_t *d = calloc(1, sizeof(*d));

    if (!d)
        return NULL;
    d->reverse = o->reverse != 0;
    d->tone_mark = TONE_MARK + o->offset_hz;
    d->tone_space = TONE_SPACE + o->offset_hz;
    d->print_score = o->loose ? PRINT_SCORE - 0.07 : PRINT_SCORE;
    d->print_agree = o->loose ? PRINT_AGREE - 0.12 : PRINT_AGREE;
    d->spb = RATE / BAUD;
    d->t_early = 0;
    d->t_prompt = d->spb / 5;
    d->t_late = d->spb * 2 / 5;
    d->phase = -1;
    for (int i = 0; i < PHASES; i++)
        d->score[i] = SCORE_NOISE;
    return d;
}

void navtex_decoder_destroy(navtex_decoder_t *d) {
    free(d);
}

void navtex_decoder_set_char_callback(navtex_decoder_t *d, navtex_char_cb_t cb, void *userdata) {
    if (d) {
        d->char_cb = cb;
        d->char_ud = userdata;
    }
}

void navtex_decoder_set_bit_callback(navtex_decoder_t *d, void (*cb)(int mark, void *userdata), void *userdata) {
    if (d) {
        d->bit_cb = cb;
        d->bit_ud = userdata;
    }
}

void navtex_decoder_process(navtex_decoder_t *d, const float *samples, unsigned int n) {
    if (!d || !samples)
        return;

    for (unsigned int i = 0; i < n; i++) {
        double v = 32767.0f * samples[i];   /* the int16 scale the levels below are tuned on */
        cpx_t  mk = mix(&d->mark_phase, d->tone_mark, v);
        cpx_t  sp = mix(&d->space_phase, d->tone_space, v);

        bool done = lpf_push(&d->mark_lpf, mk);

        lpf_push(&d->space_lpf, sp);
        if (!done)
            continue;
        if (d->reverse)
            demod(d, d->space_lpf.out, d->mark_lpf.out);
        else
            demod(d, d->mark_lpf.out, d->space_lpf.out);
    }
}

void navtex_decoder_get_stats(const navtex_decoder_t *d, navtex_stats_t *stats) {
    *stats = d->st.receiving && d->st.chars ? d->st : d->last;
}
