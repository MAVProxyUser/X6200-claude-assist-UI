/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the receiver every phase and amplitude modem here shares (V.27 ter, V.29).
 *
 *  BinModem's method (dsp/src/qam): the line mixed down at the nominal carrier and low-passed, the
 *  burst's turn-on sequence found against the symbols the transmitter is known to send, a half-symbol
 *  equaliser of 31 taps solved to it by least squares, then decisions with the taps, the carrier and
 *  the clock followed. Rewritten for a radio link rather than ported: BinModem's receivers chase a
 *  VoIP jitter buffer's slips and real fax machines' quirks, which a cable between two radios has
 *  none of, while an SSB link has what a telephone line has not, the two radios' carriers some hertz
 *  apart (tens, off the air), and both radios' sample clocks free-running. So:
 *    found:    a non-coherent match of the known turn-on, in 5 ms pieces, over +-30 ms of where the
 *              level rose: the burst's timing to a sample, whatever the carriers' offset up to +-80 Hz;
 *    measured: the offset from how the pieces' phases turn, then refined over the whole of them;
 *    trained:  the taps by least squares over a few hundred known symbols, at the best of five
 *              half-symbol alignments, refused under accept_db;
 *    followed: decision-directed LMS on the taps, a second-order loop on the carrier, and a second-order
 *              loop on the clock from the error against the equaliser output's slope, as BinModem's
 *              core turns an error into a timing error (Gardner's detector would need both band
 *              edges, and an SSB filter cuts V.29's upper one); a half symbol moved in the taps when
 *              their centre of energy drifts most of one.
 *  The far end's offset and clock come out as offset_hz (its carrier above ours) and rate_err (its
 *  sample clock against ours, -rate_err x 1e6 ppm).
 */
#ifndef X6X00_MODEM_QAMRX_H
#define X6X00_MODEM_QAMRX_H

#include "dsp.h"

#define QAM_REACH 15                    /* taps each side of the centre, in half symbols */
#define QAM_TAPS  (2 * QAM_REACH + 1)
#define QAM_RING  65536                 /* baseband samples kept: 2.7 s at 24 kHz */
#define QAM_HALVES 256

typedef struct {
    double      fs, baud, carrier, rolloff;
    int         npts;
    cplx        pts[16];                /* the slicer's table, unit power */
    const cplx *known;                  /* the turn-on from its first symbol with energy, unit power */
    int         nknown;
    int         find_syms;              /* of known, matched to find the burst */
    int         solve_from, solve_to;   /* the least-squares window, in known symbols */
    double      accept_db;
} qamrx_cfg_t;

typedef enum { QAM_IDLE, QAM_COLLECT, QAM_TRACK, QAM_ENDED } qam_state_t;

typedef struct qamrx qamrx_t;
/* a symbol decided: its index from the turn-on's first, the table's label, the point as equalised */
typedef void (*qam_symbol_fn)(void *ctx, int k, int label, cplx y);

struct qamrx {
    qamrx_cfg_t   cfg;
    qam_symbol_fn on_symbol;
    void         *ctx;
    double        sps, hsp;

    /* front end */
    double  mix_phase, mix_step;
    int     nfir;
    double *fir;
    cplx   *dl;                         /* the mixed line, a ring twice the filter long */
    int     dl_at;
    cplx   *ring;
    uint64_t n;                         /* baseband samples made */

    /* level */
    onepole_t lev;
    double    floor_, loudest;
    int       quiet_for;

    qam_state_t state;
    uint64_t    trig;
    int         fails;

    /* followed */
    double   pos;                       /* the line sample (fractional) of the next half */
    int64_t  half;                      /* its number: half 0 is symbol 0's centre */
    cplx     xh[QAM_HALVES];
    int      hoff;                      /* symbol k is centred on half 2k + hoff */
    int      k;                         /* the next symbol */
    cplx     taps[QAM_TAPS];
    double   dfo, ppre, theta;          /* the carrier: hertz off, its phase before and after the taps */
    double   pin;                       /* the equaliser's input power */
    double   mse;
    double   rate_err, ted, tkp, tki;  /* the far clock against ours; the timing error; its loop */
    double   where0;                    /* the symbols' place on the half grid as trained */
    double   slope;                     /* the equaliser output's mean squared slope, a half symbol */
    int      lost;                      /* symbols in a row far off their points */

    /* what the view shows */
    double   offset_hz, trained_snr_db, snr_db, level;
    double   timing_halves;             /* where the taps' centre of energy is, relative to theirs */
    int      shifts;                    /* half-symbol moves of the clock */
    cplx     last;
};

int  qamrx_init(qamrx_t *q, const qamrx_cfg_t *cfg, qam_symbol_fn fn, void *ctx);
void qamrx_free(qamrx_t *q);
void qamrx_reset(qamrx_t *q);           /* wait for the next burst */
void qamrx_feed(qamrx_t *q, double x);

#endif
