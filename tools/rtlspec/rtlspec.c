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
 *  Standard input, a line each: "f <Hz>" retunes, "g <tenths of dB>" a gain ("g -1": the tuner's AGC), "q" ends,
 *  "l <Hz> <mode>" listens to that frequency (mode 1 NFM, 2 AM, 3 WFM; 0 or "l 0 0" stops): it is mixed to 0 Hz,
 *  decimated to 264.6 kHz (CIC, order 2) and by 6 (a 63-tap FIR) to 44.1 kHz and demodulated, and comes out as
 *  frames of "RAUD", u32 count, s16 samples[count] (mono, 44.1 kHz) between the rows. Only at a sample rate that is
 *  a multiple of 264.6 kHz: 1058400 or 2116800 (the radio's USB carries the first without loss, not the second).
 *  "s <rate>" sets another sample rate.
 *
 *      x6x00-rtlspec [-d index] [-f Hz] [-s rate] [-n bins] [-r rows/s] [-g tenths]
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
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

static rtlsdr_dev_t *volatile term_dev;      /* the stick streaming: a signal cancels its transfers */

/* As rtl_sdr's handler: the transfers cancelled here too, so a stream that has stalled (no callback to see stop)
 * still ends cleanly; the radio's USB host (MUSB) is left broken by a stick killed with transfers in flight */
static void on_term(int s) {
    (void)s;
    stop = 1;
    if (term_dev) rtlsdr_cancel_async(term_dev);
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

/* ---- Listening: one channel demodulated ---- */

static uint32_t rate_now;       /* the stick's sample rate */
static int      want_mode;      /* the mode asked for: kept across a retune that leaves the channel outside */

#define AUD_RATE  44100
#define MID_RATE  264600        /* the CIC's output: the rate / MID_RATE, 4 or 8 */
#define R2        6             /* 264600 -> 44100 (FIR) */
#define TAPS      63
#define AUD_CHUNK 2205          /* 50 ms a frame */

static int      l_mode;         /* 0 off, 1 NFM, 2 AM, 3 WFM */
static uint64_t l_hz;
static int16_t  sin_lut[1024];  /* the mixer in integers: 2.1 million samples a second leave no room for floats */
static uint32_t ph, ph_inc;     /* its phase, 2^32 a turn */
static uint32_t i1r, i1i, i2r, i2i, c1r, c1i, c2r, c2i;     /* CIC: modular arithmetic */
static int      cic_n, r1 = 8;
static float    hist_r[TAPS], hist_i[TAPS], taps[TAPS];
static int      hist_at, fir_n;
static float    prev_r = 1, prev_i, deemph, dc, avg_mag = 0.01f;
static int16_t  aud[AUD_CHUNK];
static int      aud_n;

static void put(const void *b, size_t want) {
    const uint8_t *w = b;

    while (want > 0) {
        ssize_t k = write(1, w, want);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) {
            stop = 1;
            return;
        }
        w += k;
        want -= (size_t)k;
    }
}

/* A low pass of cutoff Hz at 264.6 kHz, Blackman windowed sinc, unity at DC */
static void design(float cutoff) {
    float fc = cutoff / (AUD_RATE * R2), sum = 0;

    for (int i = 0; i < TAPS; i++) {
        int   m = i - TAPS / 2;
        float s = m ? sinf(2 * (float)M_PI * fc * m) / ((float)M_PI * m) : 2 * fc;
        float w = 0.42f - 0.5f * cosf(2 * (float)M_PI * i / (TAPS - 1)) + 0.08f * cosf(4 * (float)M_PI * i / (TAPS - 1));
        taps[i] = s * w;
        sum += taps[i];
    }
    for (int i = 0; i < TAPS; i++) taps[i] /= sum;
}

/* What to listen to, against the centre the stick is tuned to: the mixer's step, the filter; off when outside */
static void listen_set(int mode, uint64_t hz, uint64_t centre, uint32_t rate) {
    double off = (double)hz - (double)centre;

    l_mode = (mode >= 1 && mode <= 3 && rate % MID_RATE == 0 && rate / MID_RATE <= 16 && fabs(off) < rate * 0.45)
             ? mode : 0;
    l_hz   = hz;
    if (!l_mode) return;
    r1    = (int)(rate / MID_RATE);
    cic_n = 0;
    ph_inc = (uint32_t)(int64_t)llround(-off / rate * 4294967296.0);
    design(l_mode == 2 ? 5000 : l_mode == 1 ? 9000 : 15000);     /* AM, NFM; WFM's is the audio's after the demod */
    memset(hist_r, 0, sizeof(hist_r));
    memset(hist_i, 0, sizeof(hist_i));
    deemph = dc = 0;
}

static void aud_out(float v) {
    v *= 20000;
    aud[aud_n++] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    if (aud_n == AUD_CHUNK) {
        uint32_t cnt = AUD_CHUNK;

        put("RAUD", 4);
        put(&cnt, 4);
        put(aud, sizeof(aud));
        aud_n = 0;
    }
}

/* One sample at 264.6 kHz: into the FIR, every sixth a 44.1 kHz one demodulated (WFM: demodulated here, then the
 * FIR on the audio) */
static void at_264k(float r, float i) {
    if (l_mode == 3) {
        float zr = r * prev_r + i * prev_i, zi = i * prev_r - r * prev_i;

        prev_r = r;
        prev_i = i;
        r = atan2f(zi, zr) * (264600.0f / (2 * (float)M_PI * 75000));    /* +-75 kHz: +-1 */
        i = 0;
    }
    hist_r[hist_at] = r;
    hist_i[hist_at] = i;
    hist_at = (hist_at + 1) % TAPS;
    if (++fir_n < R2) return;
    fir_n = 0;

    float yr = 0, yi = 0;
    for (int k = 0; k < TAPS; k++) {
        int h = (hist_at + k) % TAPS;
        yr += hist_r[h] * taps[k];
        yi += hist_i[h] * taps[k];
    }
    if (l_mode == 1) {                  /* NFM: +-5 kHz full scale, a little low pass and no DC */
        float zr = yr * prev_r + yi * prev_i, zi = yi * prev_r - yr * prev_i;
        float d  = atan2f(zi, zr) * (AUD_RATE / (2 * (float)M_PI * 5000));

        prev_r = yr;
        prev_i = yi;
        deemph += 0.45f * (d - deemph);
        dc += 0.002f * (deemph - dc);
        aud_out((deemph - dc) * 0.7f);
    } else if (l_mode == 2) {           /* AM: the envelope over its average, its DC out */
        float mag = sqrtf(yr * yr + yi * yi);

        avg_mag += 0.0005f * (mag - avg_mag);
        dc += 0.002f * (mag - dc);
        aud_out((mag - dc) / (avg_mag + 1e-6f) * 0.6f);
    } else {                            /* WFM: 75 us de-emphasis */
        deemph += 0.26f * (yr - deemph);
        aud_out(deemph * 0.8f);
    }
}

/* One stick sample, while listening: mixed down (integers, a 1024-step sine), into the CIC */
static inline void listen_sample(uint8_t ib, uint8_t qb) {
    int      x = ib - 127, y = qb - 127;
    uint32_t k = ph >> 22;
    int      s = sin_lut[k], c = sin_lut[(k + 256) & 1023];
    int      r = (x * c - y * s) >> 8, i = (x * s + y * c) >> 8;     /* +-32k */

    ph += ph_inc;
    i1r += (uint32_t)r;
    i1i += (uint32_t)i;
    i2r += i1r;
    i2i += i1i;
    if (++cic_n < r1) return;
    cic_n = 0;

    uint32_t a = i2r - c1r, b = i2i - c1i;      /* the combs */
    c1r = i2r;
    c1i = i2i;
    uint32_t cr = a - c2r, ci = b - c2i;
    c2r = a;
    c2i = b;
    at_264k((int32_t)cr / (32768.0f * r1 * r1), (int32_t)ci / (32768.0f * r1 * r1));
}

static long per_row_now;        /* samples a row at the rate now */
static int  rows_now = 20;

/* The commands, on a thread of their own: the tuner's calls are USB control transfers, which libusb does not take
 * from inside its own event callback (the stream's, on_samples). What they change for the stream is handed to it
 * through `pend` and taken at the start of its next transfer */
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool     centre, rate, listen;
    uint64_t centre_hz, listen_hz;
    uint32_t rate_sps;
    int      mode;
} pend;

static void *commands_thread(void *arg) {
    rtlsdr_dev_t *dev = arg;
    char          line[128];
    FILE         *in = fdopen(0, "r");

    while (in && !stop && fgets(line, sizeof(line), in)) {
        if (line[0] == 'f') {
            uint64_t hz = strtoull(line + 1, NULL, 10);

            if (hz >= 24000000 && hz <= 1766000000 && rtlsdr_set_center_freq(dev, (uint32_t)hz) == 0) {
                pthread_mutex_lock(&pend_lock);
                pend.centre    = true;
                pend.centre_hz = hz;
                pthread_mutex_unlock(&pend_lock);
            }
        } else if (line[0] == 'l') {
            char    *e;
            uint64_t hz = strtoull(line + 1, &e, 10);

            pthread_mutex_lock(&pend_lock);
            pend.listen    = true;
            pend.listen_hz = hz;
            pend.mode      = hz ? (int)strtol(e, NULL, 10) : 0;
            pthread_mutex_unlock(&pend_lock);
        } else if (line[0] == 's') {
            uint32_t r = (uint32_t)strtoul(line + 1, NULL, 10);

            if (r >= 900001 && r <= 3200000 && rtlsdr_set_sample_rate(dev, r) == 0) {
                pthread_mutex_lock(&pend_lock);
                pend.rate     = true;
                pend.rate_sps = rtlsdr_get_sample_rate(dev);
                pthread_mutex_unlock(&pend_lock);
            }
        } else if (line[0] == 'g') {
            set_gain(dev, atoi(line + 1));
        } else if (line[0] == 'q') {
            break;
        }
    }
    stop = 1;                           /* "q", or the GUI went */
    rtlsdr_cancel_async(dev);
    return NULL;
}

/* What the commands changed, taken by the stream */
static void commands(uint64_t *centre, bool *fresh) {
    pthread_mutex_lock(&pend_lock);
    if (pend.rate) {
        rate_now    = pend.rate_sps;
        per_row_now = (long)rate_now / rows_now;
        *fresh      = true;
    }
    if (pend.centre) {
        *centre = pend.centre_hz;
        *fresh  = true;
    }
    if (pend.listen) {
        want_mode = pend.mode;
        listen_set(pend.mode, pend.listen_hz, *centre, rate_now);
        if (!pend.listen_hz) l_hz = 0;
    } else if ((pend.centre || pend.rate) && l_hz) {
        listen_set(l_mode ? l_mode : want_mode, l_hz, *centre, rate_now);
    }
    pend.centre = pend.rate = pend.listen = false;
    pthread_mutex_unlock(&pend_lock);
}

/* ---- The stream: rows and, while listening, audio ---- */

static rtlsdr_dev_t *the_dev;
static uint64_t      centre_now;
static int           n_bins;
static long          row_left;      /* samples till the next row */
static float         re[MAX_BINS], im[MAX_BINS], win[MAX_BINS], acc[MAX_BINS], wsum, norm;
static uint8_t       frame[24 + MAX_BINS * 4];
static uint8_t       blk[MAX_BINS * 2];
static uint32_t      seq;
static bool          fresh = true;
static int           nfft, fill;

/* Each transfer the stick sends, on librtlsdr's thread: the commands waiting first */
static void on_samples(unsigned char *buf, uint32_t n, void *ctx) {
    int bins = n_bins;
    (void)ctx;

    commands(&centre_now, &fresh);
    if (stop) {
        rtlsdr_cancel_async(the_dev);
        return;
    }
    for (uint32_t off = 0; off + 1 < n; off += 2) {
        if (l_mode) listen_sample(buf[off], buf[off + 1]);
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
        row_left = per_row_now;
        if (nfft == 0) continue;

        /* A row: lowest frequency first (the FFT's second half, then its first), the DC bin from its neighbours */
        uint8_t *p = frame;
        uint16_t nb = (uint16_t)bins, flags = fresh ? 1 : 0;

        memcpy(p, "RSPC", 4); p += 4;
        seq++;
        memcpy(p, &seq, 4); p += 4;
        memcpy(p, &centre_now, 8); p += 8;
        memcpy(p, &rate_now, 4); p += 4;
        memcpy(p, &nb, 2); p += 2;
        memcpy(p, &flags, 2); p += 2;
        acc[0] = (acc[1] + acc[bins - 1]) / 2;
        for (int i = 0; i < bins; i++) {
            float v  = acc[(i + bins / 2) % bins] / nfft;
            float db = 10 * log10f(v + 1e-20f) - norm;

            memcpy(p, &db, 4);
            p += 4;
        }
        for (int i = 0; i < bins; i++) acc[i] = 0;
        nfft  = 0;
        fill  = 0;
        fresh = false;
        put(frame, (size_t)(p - frame));
        if (stop) {
            rtlsdr_cancel_async(the_dev);
            return;
        }
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
    rate_now    = rate;
    rows_now    = rows;
    per_row_now = (long)rate / rows;
    row_left    = per_row_now;
    n_bins      = bins;
    centre_now  = centre;
    for (int i = 0; i < 1024; i++) sin_lut[i] = (int16_t)lrint(32767 * sin(2 * M_PI * i / 1024));
    for (int i = 0; i < bins; i++) {
        win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (bins - 1));
        wsum += win[i];
    }
    norm = 20 * log10f(wsum);           /* dB of the window's full-scale sine: a level read as dBFS */
    printf("RTLSPEC 1 %s %u %d\n", tuner_name(rtlsdr_get_tuner_type(dev)), rate, bins);
    fflush(stdout);

    /* Several transfers queued all the time (as rtl_sdr and dump1090 read): one at a time, what the stick sent
     * between two of them was lost, a quarter of it whatever the rate */
    the_dev  = dev;
    term_dev = dev;
    pthread_t cmd;
    pthread_create(&cmd, NULL, commands_thread, dev);
    rtlsdr_read_async(dev, on_samples, NULL, 12, READ_LEN);
    stop = 1;
    rtlsdr_close(dev);
    return 0;
}
