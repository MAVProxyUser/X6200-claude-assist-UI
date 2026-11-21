/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the simulated SSB link (channel.h).
 */
#include "channel.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double gauss(uint64_t *s) {
    double u, v, r;
    do {
        *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
        u  = ((*s >> 11) / 9007199254740992.0) * 2 - 1;
        *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
        v  = ((*s >> 11) / 9007199254740992.0) * 2 - 1;
        r  = u * u + v * v;
    } while (r >= 1 || r == 0);
    return u * sqrt(-2 * log(r) / r);
}

void channel_init(channel_t *c, double fs, bool ssb, double offset_hz, double ppm, double snr_db) {
    memset(c, 0, sizeof(*c));
    c->fs        = fs;
    c->ssb       = ssb;
    c->offset_hz = offset_hz;
    c->ppm       = ppm;
    c->snr_db    = snr_db;
    c->rng       = 12345;
    c->sig_power = 0.25;
    if (ssb) {
        bandpass(&c->txf, 4, 300.0, 2700.0, fs);
        bandpass(&c->rxf, 4, 250.0, 2800.0, fs);
    }
    c->nh  = 255;
    c->hil = calloc(c->nh, sizeof(double));
    c->dl  = calloc(2 * c->nh, sizeof(double));
    for (int i = 0; i < c->nh; i++) {
        int    k = i - c->nh / 2;
        double w = 0.42 - 0.5 * cos(TAU * i / (c->nh - 1)) + 0.08 * cos(2 * TAU * i / (c->nh - 1));
        c->hil[i] = (k % 2) ? -2.0 / (M_PI * k) * w : 0.0;    /* reversed: the line is read oldest first */
    }
}

void channel_free(channel_t *c) {
    free(c->hil);
    free(c->dl);
}

double channel_noise(channel_t *c) {
    if (c->snr_db >= 200)
        return 0;
    return gauss(&c->rng) * sqrt(c->sig_power / pow(10, c->snr_db / 10));
}

int channel_run(channel_t *c, double x, double *out) {
    if (c->ssb)
        x = cascade_run(&c->txf, x);
    /* shift every frequency by offset_hz: the analytic signal turned, its real part */
    c->dl[c->at] = c->dl[c->at + c->nh] = x;
    c->at                                = (c->at + 1) % c->nh;
    const double *s = c->dl + c->at;
    double        h = 0;
    for (int i = 0; i < c->nh; i++)
        h += c->hil[i] * s[i];
    double re = s[c->nh / 2];
    c->phase += c->offset_hz / c->fs;
    c->phase -= floor(c->phase);
    double y = re * cos(TAU * c->phase) - h * sin(TAU * c->phase);
    if (c->ssb)
        y = cascade_run(&c->rxf, y);
    y += channel_noise(c);
    /* the far clock: read the line at 1 + ppm of its rate */
    memmove(c->buf, c->buf + 1, sizeof(double) * 3);
    c->buf[3] = y;
    int n     = 0;
    double step = 1.0 + c->ppm * 1e-6;
    while (c->t < 1.0) {
        double f = c->t, a = c->buf[0], b = c->buf[1], cc = c->buf[2], d = c->buf[3];
        out[n++] = a * (-f * (f - 1) * (f - 2) / 6) + b * ((f + 1) * (f - 1) * (f - 2) / 2) +
                   cc * (-(f + 1) * f * (f - 2) / 2) + d * ((f + 1) * f * (f - 1) / 6);
        c->t += step;
    }
    c->t -= 1.0;
    return n;
}
