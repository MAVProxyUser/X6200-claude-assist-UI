/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - x6x00-rtlspec: an RTL-SDR stick's spectrum for the GUI (src/rtl_spectrum.c), which starts it and reads
 *  its standard output; a program of its own, as dump1090 is for AIRSPACE, so a stick that hangs or goes away never
 *  takes the GUI with it.
 *
 *  It opens the stick (librtlsdr, the RTL-SDR Blog one), reads its I/Q at the sample rate and puts out ROWS rows a
 *  second: each the average of up to AVG windowed FFTs of BINS points from that row's samples, in dB, from the lowest
 *  frequency to the highest. The samples between are read and not used: the processor is not spent on all of them.
 *
 *  Standard output: one text line first, "RTLSPEC 1 <tuner> <rate> <bins>\n", then frames of
 *      "RSPC", u32 seq, u64 centre Hz, u32 rate, u16 bins, u16 flags (1: the first since a retune), f32 dB[bins]
 *  little endian. Or one line and the exit status: "RTLSPEC NOSTICK" (2), "RTLSPEC BUSY" (3, another program has it).
 *  Standard input, a line each: "f <Hz>" retunes, "g <tenths of dB>" a gain ("g -1": the tuner's AGC), "q" ends.
 *
 *      x6x00-rtlspec [-d index] [-f Hz] [-s rate] [-n bins] [-r rows/s] [-g tenths]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rtl-sdr.h>

#define AVG       8         /* FFTs averaged into a row, at most */
#define MAX_BINS  8192
#define READ_LEN  (16384 * 2)

static volatile sig_atomic_t stop;

static void on_term(int s) {
    (void)s;
    stop = 1;
}

/* In place, radix 2, n a power of two */
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        float  wr = (float)cos(a), wi = (float)sin(a);

        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;

            for (int k = 0; k < len / 2; k++) {
                int   p = i + k, q = p + len / 2;
                float xr = re[q] * cr - im[q] * ci, xi = re[q] * ci + im[q] * cr;

                re[q] = re[p] - xr;
                im[q] = im[p] - xi;
                re[p] += xr;
                im[p] += xi;
                float t = cr * wr - ci * wi;
                ci      = cr * wi + ci * wr;
                cr      = t;
            }
        }
    }
}

static const char *tuner_name(enum rtlsdr_tuner t) {
    switch (t) {
        case RTLSDR_TUNER_E4000: return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T: return "R820T";
        case RTLSDR_TUNER_R828D: return "R828D";
        default: return "UNKNOWN";
    }
}

static void set_gain(rtlsdr_dev_t *dev, int tenths) {
    if (tenths < 0) {
        rtlsdr_set_tuner_gain_mode(dev, 0);
        return;
    }
    /* The nearest the tuner has */
    int gains[64], n = rtlsdr_get_tuner_gains(dev, NULL), best = 0;

    if (n <= 0 || n > 64) return;
    rtlsdr_get_tuner_gains(dev, gains);
    for (int i = 1; i < n; i++)
        if (abs(gains[i] - tenths) < abs(gains[best] - tenths)) best = i;
    rtlsdr_set_tuner_gain_mode(dev, 1);
    rtlsdr_set_tuner_gain(dev, gains[best]);
}

/* The commands waiting on standard input */
static void commands(rtlsdr_dev_t *dev, uint64_t *centre, bool *fresh) {
    static char line[128];
    static int  len;
    struct pollfd p = {.fd = 0, .events = POLLIN};

    while (poll(&p, 1, 0) > 0) {
        ssize_t n = read(0, line + len, sizeof(line) - 1 - len);

        if (n <= 0) {
            stop = 1;                   /* the GUI went */
            return;
        }
        len += (int)n;
        line[len] = 0;
        char *nl;
        while ((nl = strchr(line, '\n'))) {
            *nl = 0;
            if (line[0] == 'f') {
                uint64_t hz = strtoull(line + 1, NULL, 10);
                if (hz >= 24000000 && hz <= 1766000000 && rtlsdr_set_center_freq(dev, (uint32_t)hz) == 0) {
                    *centre = hz;
                    *fresh  = true;
                }
            } else if (line[0] == 'g') {
                set_gain(dev, atoi(line + 1));
            } else if (line[0] == 'q') {
                stop = 1;
            }
            len -= (int)(nl + 1 - line);
            memmove(line, nl + 1, len + 1);
        }
        if (len >= (int)sizeof(line) - 1) len = 0;
    }
}

int main(int argc, char **argv) {
    int      index = 0, bins = 2048, rows = 20, gain = 280, opt;
    uint32_t rate   = 2400000;
    uint64_t centre = 162550000;

    while ((opt = getopt(argc, argv, "d:f:s:n:r:g:")) != -1) {
        switch (opt) {
            case 'd': index = atoi(optarg); break;
            case 'f': centre = strtoull(optarg, NULL, 10); break;
            case 's': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'n': bins = atoi(optarg); break;
            case 'r': rows = atoi(optarg); break;
            case 'g': gain = atoi(optarg); break;
            default: return 1;
        }
    }
    if (bins < 64 || bins > MAX_BINS || (bins & (bins - 1)) || rows < 1 || rows > 50) return 1;
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, on_term);

    if ((int)rtlsdr_get_device_count() <= index) {
        printf("RTLSPEC NOSTICK\n");
        return 2;
    }
    rtlsdr_dev_t *dev;
    if (rtlsdr_open(&dev, index) != 0) {
        printf("RTLSPEC BUSY\n");
        return 3;
    }
    rtlsdr_set_sample_rate(dev, rate);
    rtlsdr_set_center_freq(dev, (uint32_t)centre);
    rtlsdr_set_bias_tee(dev, 0);
    set_gain(dev, gain);
    rtlsdr_reset_buffer(dev);
    rate = rtlsdr_get_sample_rate(dev);
    printf("RTLSPEC 1 %s %u %d\n", tuner_name(rtlsdr_get_tuner_type(dev)), rate, bins);
    fflush(stdout);

    static uint8_t  buf[READ_LEN];
    static float    re[MAX_BINS], im[MAX_BINS], win[MAX_BINS], acc[MAX_BINS];
    static uint8_t  frame[24 + MAX_BINS * 4];
    static uint8_t  blk[MAX_BINS * 2];
    uint32_t        seq = 0;
    bool            fresh = true;
    long            per_row = (long)rate / rows, row_left = per_row;     /* samples */
    int             nfft = 0, fill = 0;
    float           wsum = 0;

    for (int i = 0; i < bins; i++) {
        win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (bins - 1));
        wsum += win[i];
    }
    /* dB of the window's full-scale sine: a level read as dBFS */
    float norm = 20 * log10f(wsum);

    while (!stop) {
        int n = 0;

        commands(dev, &centre, &fresh);
        if (rtlsdr_read_sync(dev, buf, READ_LEN, &n) < 0 || n <= 0) {
            break;                      /* unplugged */
        }
        for (int off = 0; off + 1 < n; off += 2) {
            /* The first BINS * AVG samples of each row's share go into its FFTs, the rest are passed over */
            if (nfft < AVG) {
                blk[fill * 2]     = buf[off];
                blk[fill * 2 + 1] = buf[off + 1];
                if (++fill == bins) {
                    for (int i = 0; i < bins; i++) {
                        re[i] = (blk[i * 2] - 127.4f) / 128.0f * win[i];
                        im[i] = (blk[i * 2 + 1] - 127.4f) / 128.0f * win[i];
                    }
                    fft(re, im, bins);
                    for (int i = 0; i < bins; i++) acc[i] += re[i] * re[i] + im[i] * im[i];
                    nfft++;
                    fill = 0;
                }
            }
            if (--row_left > 0) continue;
            row_left = per_row;
            if (nfft == 0) continue;

            /* A row: lowest frequency first (the FFT's second half, then its first), the DC bin from its neighbours */
            uint8_t *p = frame;
            uint16_t nb = (uint16_t)bins, flags = fresh ? 1 : 0;

            memcpy(p, "RSPC", 4); p += 4;
            seq++;
            memcpy(p, &seq, 4); p += 4;
            memcpy(p, &centre, 8); p += 8;
            memcpy(p, &rate, 4); p += 4;
            memcpy(p, &nb, 2); p += 2;
            memcpy(p, &flags, 2); p += 2;
            acc[0] = (acc[1] + acc[bins - 1]) / 2;
            for (int i = 0; i < bins; i++) {
                float v  = acc[(i + bins / 2) % bins] / nfft;
                float db = 10 * log10f(v + 1e-20f) - norm;

                memcpy(p, &db, 4);
                p += 4;
                acc[(i + bins / 2) % bins] = 0;
            }
            for (int i = 0; i < bins; i++) acc[i] = 0;
            nfft  = 0;
            fill  = 0;
            fresh = false;
            size_t   want = (size_t)(p - frame);
            uint8_t *w    = frame;
            while (want > 0) {
                ssize_t k = write(1, w, want);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) {
                    stop = 1;
                    break;
                }
                w += k;
                want -= (size_t)k;
            }
        }
    }
    rtlsdr_close(dev);
    return 0;
}
