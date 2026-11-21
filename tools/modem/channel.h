/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: a simulated SSB link for the tests on the Mac: the transmitter's audio filter, a carrier
 *  offset (the two radios' oscillators apart: a single-sideband shift by an analytic signal), the
 *  receiver's filter, the two sample clocks apart, and noise.
 */
#ifndef X6X00_MODEM_CHANNEL_H
#define X6X00_MODEM_CHANNEL_H

#include "dsp.h"

typedef struct {
    double    fs, offset_hz, ppm, snr_db, gain;
    bool      ssb;                      /* the radios' 300-2700 Hz audio filters */
    cascade_t txf, rxf;
    double   *hil;                      /* the Hilbert transformer, odd length */
    double   *dl;
    int       nh, at;
    double    phase;
    double    t, buf[8];                /* the far clock: resampling */
    int       nbuf;
    uint64_t  rng;
    double    sig_power;                /* of the signal, for the noise's level */
} channel_t;

void   channel_init(channel_t *c, double fs, bool ssb, double offset_hz, double ppm, double snr_db);
/* one sample in; the samples out (0, 1 or 2 with the clocks apart) into out, their count returned */
int    channel_run(channel_t *c, double x, double *out);
void   channel_free(channel_t *c);
double channel_noise(channel_t *c);     /* a sample of the noise alone, for quiet stretches */

#endif
