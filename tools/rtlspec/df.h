/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - direction finding with a KrakenSDR (docs/plans/kraken_df_plan.md): the parts of x6x00-df that need
 *  no device, so they run and are tested on the Mac as on the radio (df_test.c, df_bench.c).
 *
 *  Written from the published maths (MUSIC, Capon, Bartlett, Burg's MEM) and the measurements the plan describes; no
 *  code from Heimdall or krakensdr_doa (GPL-3).
 *
 *  Angles: degrees clockwise from the array's forward direction (ANT 0 of a circle), as KrakenRF's Antenna Array
 *  Size Calculator draws it (top-down, ANT k of a circle at 360k/N clockwise). Positions in metres, x forward, y left.
 *  A line (ULA) runs from ANT 0 on the left to ANT N-1 on the right, forward being broadside.
 */

#pragma once

#include <complex.h>
#include <stdbool.h>
#include <stdint.h>

#define DF_MAXCH    8
#define DF_GRID     360         /* the DoA spectrum: one point a degree */
#define DF_C        299792458.0

/* ------------------------------------------------------------------------------------------------- FFT (radix 2) */

typedef struct {
    int            n;
    float complex *tw;          /* n/2 twiddles */
    int           *rev;         /* bit reversal */
} df_fft_t;

int  df_fft_init(df_fft_t *f, int n);                           /* n a power of two; 0 done */
void df_fft_free(df_fft_t *f);
void df_fft(const df_fft_t *f, float complex *x, bool inverse);  /* in place, unscaled */

/* u8 I/Q (an RTL's bytes) to complex, its mean taken off */
void df_u8_to_cf(const uint8_t *iq, int n, float complex *out);

/* -------------------------------------------------------------------------------------------- calibration */

/* From one noise-source block: what makes channel m match channel 0. Channel m is read k[m] samples later than
 * channel 0 (so its offset in a common read is k[m] - min k), then multiplied by df_cal_corr(). */
typedef struct {
    int   nch;
    int   k[DF_MAXCH];          /* whole-sample delay */
    float tau[DF_MAXCH];        /* what is left of it, in samples (under one) */
    float phi[DF_MAXCH];        /* phase at 0 Hz, radians */
    float gain[DF_MAXCH];       /* amplitude correction */
    float margin_db[DF_MAXCH];  /* cross-correlation peak over the mean of the rest */
    float resid_deg[DF_MAXCH];  /* rms of what is left of the phase once the line and the ripple are taken off */
    float ripple_deg[DF_MAXCH]; /* rms of the ripple: the tuners' filters' own phase, off the line */
    float ripple[DF_MAXCH][1024];   /* the ripple per cross-spectrum bin (DF_CAL_SEG, 0 Hz in the middle), radians:
                                     * smoothed over +-4 bins; added to the line at the target (df_cal_corr) */
    float level_db[DF_MAXCH];   /* the noise's power, dB full scale */
    bool  ok;
    char  why[80];
} df_cal_t;

#define DF_CAL_N        65536   /* samples per channel for a calibration */
#define DF_CAL_SEG      1024    /* the cross-spectrum's FFT */
#define DF_CAL_MARGIN   20.0f   /* dB the correlation peak must stand over the rest (as Heimdall) */
#define DF_CAL_RESID    5.0f    /* degrees rms a bin's phase may miss the line + ripple by: a slip inside the block
                                 * gives tens of degrees. On the radio's Kraken the noise alone left 2.3-2.7 per bin
                                 * at 915 MHz (2026-11-17), so the correction (+-4 bins smoothed) is good to about 1 */
#define DF_CAL_RIPPLE   20.0f   /* degrees rms of ripple beyond which the block is not trusted */

/* x: nch channels of n samples (a power of two, DF_CAL_N), all read at the same index; their means already off.
 * maxlag: the largest whole-sample delay looked for (searched in the first DF_CAL_XN samples, all n if that fails).
 * known_k: the whole-sample delays of the last good calibration, to skip that search (NULL: search). Returns 0 and
 * out->ok, or -1 with out->why (with known_k: try again without) */
#define DF_CAL_XN       16384
#define DF_CAL_NSEG     32      /* cross-spectrum pieces averaged */
int df_cal_run(float complex *const x[], int nch, int n, int maxlag, const int *known_k, df_cal_t *out);

/* channel m's correction for a signal f Hz from the centre at sample rate fs */
float complex df_cal_corr(const df_cal_t *c, int m, double f, double fs);

/* ------------------------------------------------------------------------------------------ the channel filter */

/* The target mixed to 0 Hz (one oscillator for every channel, so no phase between them) and decimated in two FIR
 * stages to about 32 kS/s or more. The same filters on every channel */
typedef struct {
    int     nch, d1, d2, t1, t2;
    double  fs, fs_out, f, bw;
    float  *h1, *h2;                    /* taps, reversed for a forward dot product */
    double  ph;                         /* the oscillator's phase, cycles */
    int     cap;                        /* the largest block */
    float  *nco;                        /* cos, sin per sample of a chunk */
    double  dc[DF_MAXCH][2];            /* each channel's mean bytes, from the chunk before */
    float  *x1[DF_MAXCH][2];            /* stage 1 input: history + block, I and Q */
    float  *x2[DF_MAXCH][2];            /* stage 2 input */
    int     n1, n2;                     /* samples held in x1 / x2 (history + new) */
    int     next1, next2;               /* where each stage's next output's newest sample is */
} df_chan_t;

/* fs input rate, f the target's offset from the centre, bw its bandwidth (Hz), cap the largest block; 0 done */
int  df_chan_init(df_chan_t *c, int nch, double fs, double f, double bw, int cap);
void df_chan_free(df_chan_t *c);
void df_chan_retune(df_chan_t *c, double f);        /* a new offset, same filters */
/* iq: per channel n u8 I/Q samples, already aligned; out: per channel room for n / (d1 d2) + 1. Returns the count */
int  df_chan_run(df_chan_t *c, const uint8_t *const iq[], int n, float complex *const out[]);

/* ------------------------------------------------------------------------------------------------- arrays */

typedef enum { DF_UCA, DF_ULA, DF_CUSTOM } df_kind_t;

typedef struct {
    df_kind_t kind;
    int       n;
    double    size;                     /* UCA radius, ULA spacing, metres (0 for custom) */
    double    x[DF_MAXCH], y[DF_MAXCH]; /* metres: x forward, y left */
} df_array_t;

void df_array_uca(df_array_t *a, int n, double radius);
void df_array_ula(df_array_t *a, int n, double spacing);

/* The calculator's numbers at a frequency: the spacing multiplier s (element spacing over lambda) and its
 * "est. resolution" (degrees(1.22 / aperture in lambda) / 10, the aperture 2r or (N-1)d) */
void df_array_calc(const df_array_t *a, double f_hz, double *s, double *res_deg);

/* ------------------------------------------------------------------------------------------------- DoA */

typedef enum { DF_MUSIC, DF_BARTLETT, DF_CAPON, DF_MEM } df_method_t;

typedef struct {
    int            n;
    double         lambda;
    float complex  a[DF_GRID][DF_MAXCH];    /* steering vectors, one a degree */
} df_steer_t;

void df_steer_make(df_steer_t *s, const df_array_t *a, double lambda);
/* the steering vector at any angle */
void df_steer_at(const df_array_t *a, double lambda, double deg, double complex *v);

typedef struct {
    double bearing;             /* degrees 0-360 */
    double mirror;              /* a line's other side (-1: none) */
    double fit;                 /* 0-1: the share of the received power one plane wave from the bearing explains */
    double eig_db;              /* strongest eigenvalue over the next */
    double snr_db;              /* per element, from the eigenvalues */
    double papr_db;             /* the spectrum's peak over its mean (KrakenRF's "confidence") */
    double width_deg;           /* Bartlett's half-power width at the bearing */
    int    npeaks;
    double peaks[3];            /* other peaks within 6 dB */
    float  p_db[DF_GRID];       /* the method's spectrum, 0 dB at its peak */
} df_doa_t;

/* R: n x n Hermitian, row major. A line (ULA) is searched in front only, its mirror given; front_only puts its back
 * half at the floor in p_db */
int df_doa(const double complex *R, const df_array_t *a, const df_steer_t *s, df_method_t m, int nsrc,
           bool front_only, df_doa_t *out);

/* Each channel left out in turn: the best Bartlett fit of the rest, at any bearing. One channel whose absence makes
 * the rest fit (a slip, or a bad cable) stands out */
void df_fit_without(const double complex *R, const df_array_t *a, const df_steer_t *s, double *fit);

/* Hermitian eigen-decomposition (Jacobi), n <= DF_MAXCH: values largest first, vectors as columns of v */
void df_eigh(const double complex *A, int n, double *val, double complex *v);

/* ------------------------------------------------------------------------------------------------- engine */

typedef struct {
    /* set before df_engine_init */
    int         nch;
    double      fs, fc;                 /* sample rate, the tuners' centre (Hz) */
    double      f, bw;                  /* the target: offset from the centre, bandwidth */
    df_array_t  arr;
    df_method_t method;
    int         nsrc;
    bool        front_only;
    double      sql_db;                 /* the squelch: target over the floor (default 8) */
    double      eig_min_db;             /* one source across the array (default 6) */
    double      fit_min;                /* a plane wave explains it (default 0.5) */
    double      cal_every_s;            /* the timer (default 120; negative: never) */

    /* inside */
    df_chan_t      chan;
    df_steer_t     steer;
    df_fft_t       fft;
    df_cal_t       cal;
    bool           have_cal;
    float complex  corr[DF_MAXCH];
    float complex *dec[DF_MAXCH];
    float complex *fbuf;
    int            cap, open_run;
    double         since_cal, low_fit, last_slip_cal, now, sql_closed;
    double         hist[10];
    int            nhist;

    /* which channels go into the bearing: all but those the user left out and those the target is 12 dB or more
     * under the rest on for a second (a dead antenna or cable: back in once within 6 dB) */
    unsigned       user_off;            /* set by the caller: bit m leaves channel m out */
    bool           low[DF_MAXCH];
    double         low_t[DF_MAXCH];
    unsigned       sub_mask;            /* the elements sub and sub_steer were made for */
    df_array_t     sub;
    df_steer_t     sub_steer;
    float          spec_db[1024];       /* CH0's last spectrum, dBFS, lowest frequency first */
    float          win[1024];           /* Hann */
    double         floor_db;            /* its median bin */
} df_engine_t;

/* A dead antenna: the target this far under the median of the others for a second; back in once within the second.
 * Not 12: indoors one element of the radio's Kraken heard the HackRF 12 dB under the rest with nothing wrong with it
 * (2026-11-17); a dead or unplugged one is 20-30 dB down */
#define DF_LOW_DB       20.0
#define DF_LOW_BACK_DB  12.0

typedef struct {
    bool     gate;                      /* a bearing worth showing */
    double   level_db;                  /* the target over the floor: the median of the channels in use */
    double   uncert_deg;
    int      cal_wanted;                /* 0 no, 1 a slip, 2 the timer */
    int      bad_ch;                    /* the channel that looks slipped, -1 none */
    int      k;                         /* snapshots behind it */
    bool     have_doa;                  /* doa is there (enough elements, enough samples) */
    unsigned used;                      /* the channels in the bearing (bit m) */
    int      nused;
    double   ch_db[DF_MAXCH];           /* the target's strongest bin over each channel's own floor */
    double   clip[DF_MAXCH];            /* share of each channel's bytes at 0 or 255 */
    df_doa_t doa;                       /* bad_ch and the bearing are in whole-array channel numbers */
} df_est_t;

int  df_engine_init(df_engine_t *e, int cap);
void df_engine_free(df_engine_t *e);
void df_engine_set_cal(df_engine_t *e, const df_cal_t *c);
void df_engine_retarget(df_engine_t *e, double f);
/* one block of aligned u8 I/Q per channel (n samples, n <= cap): an estimate always (gate says if it counts) */
int  df_engine_block(df_engine_t *e, const uint8_t *const iq[], int n, df_est_t *out);
