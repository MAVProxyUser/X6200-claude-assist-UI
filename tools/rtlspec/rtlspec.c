/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - x6x00-rtlspec: the RTL-SDR stick's daemon. It opens the stick once and keeps it streaming for as long
 *  as the radio runs; the GUI (src/rtl_spectrum.c) connects to it and asks for what it wants. So the stick is never
 *  closed and opened again for a GUI started again (a hot install, a crash), a screen changed or AIRSPACE opened:
 *  the radio's USB host (MUSB) has been left sending nothing for the stick ("THE RTL-SDR SENDS NOTHING") by its
 *  stream stopped and started. A program of its own, as dump1090 is, so a stick that hangs or goes away never takes
 *  the GUI with it.
 *
 *  The stick: opened when it is there (looked for each second), set as the GUI last asked, its I/Q read at the sample
 *  rate all the time. Closed only when it is pulled out (another one is opened when it is plugged in) or the daemon
 *  is told to end. One that opens and sends nothing, or whose stream ends with it still plugged in, is left open and
 *  said to be STALLED: only pulling it out mends that, and nothing here touches it in the meantime.
 *
 *  Out of the samples, only what the client asks for, and nothing with no client: ROWS rows a second while it wants
 *  rows ("r 1"), each the average of up to AVG windowed FFTs of BINS points from that row's samples, in dB, lowest
 *  frequency first (the samples between are read and not used: the processor is not spent on all of them); a
 *  channel's audio while it listens; SAME alerts while it watches; the raw samples to AIRSPACE's dump1090 while that
 *  has them.
 *
 *  The client: a UNIX socket in the abstract namespace, "@x6x00-rtlspec", SOCK_SEQPACKET, one client at a time (a
 *  new one replaces the old: the GUI started again). Each message is one thing. From the daemon, little endian:
 *      "RTLSPEC 2 <state> <tuner> <rate> <bins>"   on connecting and at each change of state: NOSTICK, BUSY (another
 *                          program has it), OPENING, ON (samples coming), STALLED (open, nothing coming), ADSB
 *                          (AIRSPACE's dump1090 has the samples)
 *      "RSPC", u32 seq, u64 centre Hz, u32 rate, u16 bins, u16 flags (1: the first since a retune), f32 dB[bins]
 *      "RAUD", u32 count, s16 samples[count]       the channel listened to, mono, 44.1 kHz, 50 ms a message
 *      "RSAM", u32 length, the header's text and "|" and the event's name when known
 *                          ("ZCZC-WXR-TOR-039049+0030-3170815-KILN/NWS-|TORNADO WARNING"; "NNNN": a message's end)
 *  A client that does not read misses messages: the stream never waits for it. From the client, a line each:
 *      "r 1", "r 0"        rows wanted, or not
 *      "f <Hz>"            the centre; "s <rate>" the sample rate; "g <tenths of dB>" the gain ("g -1": the AGC)
 *      "l <Hz> <mode>"     listen (mode 1 NFM, 2 AM, 3 WFM; "l 0 0" stops): mixed to 0 Hz, decimated to 264.6 kHz
 *                          (CIC, order 2) and by 6 (a 63-tap FIR) to 44.1 kHz and demodulated. Only at a sample rate
 *                          that is a multiple of 264.6 kHz: 1058400 or 2116800
 *      "w <Hz>"            watch that channel (narrow FM, NOAA Weather Radio) for SAME alerts (same.c); "w 0" stops
 *      "a" and a file descriptor (SCM_RIGHTS)  the raw samples (unsigned 8-bit I/Q) written to it at 1090 MHz,
 *                          2.4 MS/s and the tuner's top gain, for AIRSPACE's dump1090 ("--ifile -"); no rows or audio
 *                          meanwhile. "a 0", or dump1090 gone: the stick back to what the client asked
 *      "Q"                 the daemon ends and closes the stick (a GUI that wants another version of it)
 *  A client gone: its rows, listening and watch end; the stick goes on, tuned as it was.
 *
 *      x6x00-rtlspec [-F] [-d index] [-f Hz] [-s rate] [-n bins] [-r rows/s] [-g tenths] [-i FILE]
 *
 *  It goes into the background (-F: it stays in front); with one already running it says "RTLSPEC RUNNING" and ends.
 *  -i replays FILE (unsigned 8-bit I/Q at the rate, as rtl_sdr writes it) in place of the stick, at its own pace and
 *  over again: the same rows, audio and alerts, to try the GUI without a signal on the air (a SAME alert, say); a
 *  retune only moves what the rows say.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <rtl-sdr.h>

#include "same.h"

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

/* ---- Channels: one demodulated to the speaker, one watched for SAME alerts ---- */

static uint32_t rate_now;       /* the stick's sample rate */
static int      want_mode;      /* the mode asked for: kept across a retune that leaves the channel outside */

#define AUD_RATE  44100
#define MID_RATE  264600        /* the CIC's output: the rate / MID_RATE, 4 or 8 */
#define R2        6             /* 264600 -> 44100 (FIR) */
#define TAPS      63
#define AUD_CHUNK 2205          /* 50 ms a frame */

static int16_t sin_lut[1024];   /* the mixers in integers: 2.1 million samples a second leave no room for floats */

typedef struct chan chan_t;
struct chan {
    int      mode;              /* 0 off, 1 NFM, 2 AM, 3 WFM */
    uint64_t hz;
    uint32_t ph, ph_inc;        /* the mixer's phase, 2^32 a turn */
    uint32_t i1r, i1i, i2r, i2i, c1r, c1i, c2r, c2i;     /* CIC: modular arithmetic */
    int      cic_n, r1;
    float    hist_r[TAPS], hist_i[TAPS], taps[TAPS];
    int      hist_at, fir_n;
    float    prev_r, prev_i, deemph, dc, avg_mag;
    bool     raw;               /* the demodulated FM as it is, no audio shaping (SAME's tones) */
    void   (*out)(float v);     /* each 44.1 kHz sample */
};

static void listen_out(float v);
static void watch_out(float v);
static chan_t lis    = {.r1 = 8, .prev_r = 1, .avg_mag = 0.01f, .out = listen_out};
static chan_t watch  = {.r1 = 8, .prev_r = 1, .avg_mag = 0.01f, .raw = true, .out = watch_out};
#define l_mode lis.mode
#define l_hz   lis.hz

/* The client, and what it wants */
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static int             client = -1;     /* its socket, -1 none */
static atomic_bool     rows_on;         /* "r 1" */

/* One message to the client, whole or not at all: one not reading (its socket full) misses it, as the stream (the
 * stick's own thread) must never wait */
static void put(const void *b, size_t n) {
    pthread_mutex_lock(&out_lock);
    if (client >= 0) (void)send(client, b, n, MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&out_lock);
}

/* A low pass of cutoff Hz at 264.6 kHz, Blackman windowed sinc, unity at DC */
static void design(float *taps, float cutoff) {
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

/* What a channel takes, against the centre the stick is tuned to: the mixer's step, the filter; off outside */
static void chan_set(chan_t *c, int mode, uint64_t hz, uint64_t centre, uint32_t rate) {
    double off = (double)hz - (double)centre;

    c->mode = (mode >= 1 && mode <= 3 && rate % MID_RATE == 0 && rate / MID_RATE <= 16 && fabs(off) < rate * 0.45)
              ? mode : 0;
    c->hz = hz;
    if (!c->mode) return;
    c->r1     = (int)(rate / MID_RATE);
    c->cic_n  = 0;
    c->ph_inc = (uint32_t)(int64_t)llround(-off / rate * 4294967296.0);
    design(c->taps, c->mode == 2 ? 5000 : c->mode == 1 ? 9000 : 15000);   /* AM, NFM; WFM's: the audio's */
    memset(c->hist_r, 0, sizeof(c->hist_r));
    memset(c->hist_i, 0, sizeof(c->hist_i));
    c->deemph = c->dc = 0;
}

static void listen_set(int mode, uint64_t hz, uint64_t centre, uint32_t rate) {
    chan_set(&lis, mode, hz, centre, rate);
}

static void listen_out(float v) {
    static uint8_t msg[8 + AUD_CHUNK * 2];
    static int     aud_n;
    int16_t        s;

    v *= 20000;
    s = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    memcpy(msg + 8 + aud_n * 2, &s, 2);
    if (++aud_n == AUD_CHUNK) {
        uint32_t cnt = AUD_CHUNK;

        memcpy(msg, "RAUD", 4);
        memcpy(msg + 4, &cnt, 4);
        put(msg, sizeof(msg));
        aud_n = 0;
    }
}

/* The watched channel's audio into the SAME decoder; a header decided goes out as "RSAM", u32 length, its text */
static same_t *same;

static void watch_out(float v) {
    static float buf[441];
    static int   n;
    char         text[SAME_MAX + 48];
    uint8_t      msg[8 + sizeof(text)];

    buf[n++] = v;
    if (n < (int)(sizeof(buf) / sizeof(buf[0]))) return;
    n = 0;
    if (!same) same = same_new(AUD_RATE);
    if (same && same_feed(same, buf, sizeof(buf) / sizeof(buf[0]), text, sizeof(text) - 40)) {
        /* the event's name after a "|", for the GUI: "ZCZC-WXR-TOR-...-|TORNADO WARNING" */
        const char *name = strlen(text) > 12 ? same_event_name(text + 9) : NULL;

        if (name) snprintf(text + strlen(text), sizeof(text) - strlen(text), "|%s", name);
        uint32_t len = (uint32_t)strlen(text);

        memcpy(msg, "RSAM", 4);
        memcpy(msg + 4, &len, 4);
        memcpy(msg + 8, text, len);
        put(msg, 8 + len);
    }
}

/* One sample at 264.6 kHz: into the FIR, every sixth a 44.1 kHz one demodulated (WFM: demodulated here, then the
 * FIR on the audio) */
static void at_264k(chan_t *c, float r, float i) {
    if (c->mode == 3) {
        float zr = r * c->prev_r + i * c->prev_i, zi = i * c->prev_r - r * c->prev_i;

        c->prev_r = r;
        c->prev_i = i;
        r = atan2f(zi, zr) * (264600.0f / (2 * (float)M_PI * 75000));    /* +-75 kHz: +-1 */
        i = 0;
    }
    c->hist_r[c->hist_at] = r;
    c->hist_i[c->hist_at] = i;
    c->hist_at = (c->hist_at + 1) % TAPS;
    if (++c->fir_n < R2) return;
    c->fir_n = 0;

    float yr = 0, yi = 0;
    for (int k = 0; k < TAPS; k++) {
        int h = (c->hist_at + k) % TAPS;
        yr += c->hist_r[h] * c->taps[k];
        yi += c->hist_i[h] * c->taps[k];
    }
    if (c->mode == 1) {                 /* NFM: +-5 kHz full scale, a little low pass and no DC */
        float zr = yr * c->prev_r + yi * c->prev_i, zi = yi * c->prev_r - yr * c->prev_i;
        float d  = atan2f(zi, zr) * (AUD_RATE / (2 * (float)M_PI * 5000));

        c->prev_r = yr;
        c->prev_i = yi;
        if (c->raw) {
            c->dc += 0.002f * (d - c->dc);
            c->out(d - c->dc);
            return;
        }
        c->deemph += 0.45f * (d - c->deemph);
        c->dc += 0.002f * (c->deemph - c->dc);
        c->out((c->deemph - c->dc) * 0.7f);
    } else if (c->mode == 2) {          /* AM: the envelope over its average, its DC out */
        float mag = sqrtf(yr * yr + yi * yi);

        c->avg_mag += 0.0005f * (mag - c->avg_mag);
        c->dc += 0.002f * (mag - c->dc);
        c->out((mag - c->dc) / (c->avg_mag + 1e-6f) * 0.6f);
    } else {                            /* WFM: 75 us de-emphasis */
        c->deemph += 0.26f * (yr - c->deemph);
        c->out(c->deemph * 0.8f);
    }
}

/* One stick sample into a channel: mixed down (integers, a 1024-step sine), into its CIC */
static inline void chan_sample(chan_t *c, uint8_t ib, uint8_t qb) {
    int      x = ib - 127, y = qb - 127;
    uint32_t k = c->ph >> 22;
    int      s = sin_lut[k], co = sin_lut[(k + 256) & 1023];
    int      r = (x * co - y * s) >> 8, i = (x * s + y * co) >> 8;     /* +-32k */

    c->ph += c->ph_inc;
    c->i1r += (uint32_t)r;
    c->i1i += (uint32_t)i;
    c->i2r += c->i1r;
    c->i2i += c->i1i;
    if (++c->cic_n < c->r1) return;
    c->cic_n = 0;

    uint32_t a = c->i2r - c->c1r, b = c->i2i - c->c1i;      /* the combs */
    c->c1r = c->i2r;
    c->c1i = c->i2i;
    uint32_t cr = a - c->c2r, ci = b - c->c2i;
    c->c2r = a;
    c->c2i = b;
    at_264k(c, (int32_t)cr / (32768.0f * c->r1 * c->r1), (int32_t)ci / (32768.0f * c->r1 * c->r1));
}

static long per_row_now;        /* samples a row at the rate now */
static int  rows_now = 20;

/* The commands, on a thread of their own: the tuner's calls are USB control transfers, which libusb does not take
 * from inside its own event callback (the stream's, on_samples). What they change for the stream is handed to it
 * through `pend` and taken at the start of its next transfer */
static pthread_mutex_t pend_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool     centre, rate, listen, watch;
    uint64_t centre_hz, listen_hz, watch_hz;
    uint32_t rate_sps;
    int      mode;
} pend;

static uint32_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint64_t centre_now;
static int      n_bins;
static long     row_left;       /* samples till the next row */
static float    re[MAX_BINS], im[MAX_BINS], win[MAX_BINS], acc[MAX_BINS], wsum, norm;
static uint8_t  frame[24 + MAX_BINS * 4];
static uint8_t  blk[MAX_BINS * 2];
static uint32_t seq;
static bool     fresh = true;
static int      nfft, fill;

/* ---- The stick's state, for the client ---- */

enum { S_NOSTICK, S_BUSY, S_OPENING, S_ON, S_STALLED };
static const char *const s_name[] = {"NOSTICK", "BUSY", "OPENING", "ON", "STALLED"};

#define PROTO      2
#define SOCK_NAME  "x6x00-rtlspec"      /* abstract: gone with the daemon, a second one cannot take it */
#define STALL_MS   3000                 /* open and nothing for so long: STALLED */
#define ADSB_HZ    1090000000u
#define ADSB_RATE  2400000u             /* dump1090's own */

static atomic_int  s_now = S_NOSTICK;
static atomic_uint last_ms;             /* the stick's last transfer */
static atomic_bool feeding;             /* AIRSPACE's dump1090 has the samples */
static int         dev_index;
static atomic_bool stick_done;          /* the stick's thread ended: the stick closed */
static const char *replay;              /* -i */

/* ---- The tuner: as the client asked, or for dump1090 ---- */

static pthread_mutex_t tune_lock = PTHREAD_MUTEX_INITIALIZER;   /* all below; taken before pend_lock */
static rtlsdr_dev_t   *dev_now;         /* the stick open */
static char            tuner_s[16] = "-";
static uint64_t        want_centre;
static uint32_t        want_rate;
static int             want_gain;
static uint64_t        cur_centre;      /* what the stick is set to: 0 (INT_MIN) not set since it opened */
static uint32_t        cur_rate;
static int             cur_gain = INT_MIN;

/* The stick set as it should be. On the client's thread, the watchdog's, the stick's before it streams: never on the
 * stream's, as libusb does not take control transfers from inside its own event callback. What the stream has to
 * know is handed to it through pend */
static void apply_tuning(void) {
    pthread_mutex_lock(&tune_lock);
    bool     ads = atomic_load(&feeding);
    uint64_t c   = ads ? ADSB_HZ : want_centre;
    uint32_t r   = ads ? ADSB_RATE : want_rate;
    int      g   = ads ? 999 : want_gain;           /* the tuner's top, as dump1090 itself sets */

    if (dev_now) {
        if (r != cur_rate && rtlsdr_set_sample_rate(dev_now, r) == 0) {
            cur_rate = rtlsdr_get_sample_rate(dev_now);
            pthread_mutex_lock(&pend_lock);
            pend.rate     = true;
            pend.rate_sps = cur_rate;
            pthread_mutex_unlock(&pend_lock);
        }
        if (g != cur_gain) {
            set_gain(dev_now, g);
            cur_gain = g;
        }
    }
    if (c != cur_centre && (replay || (dev_now && rtlsdr_set_center_freq(dev_now, (uint32_t)c) == 0))) {
        cur_centre = c;
        pthread_mutex_lock(&pend_lock);
        pend.centre    = true;
        pend.centre_hz = c;
        pthread_mutex_unlock(&pend_lock);
    }
    pthread_mutex_unlock(&tune_lock);
}

static void hello(char *s, size_t n) {
    pthread_mutex_lock(&tune_lock);
    snprintf(s, n, "RTLSPEC %d %s %s %u %d", PROTO, atomic_load(&feeding) ? "ADSB" : s_name[atomic_load(&s_now)],
             tuner_s, cur_rate ? cur_rate : want_rate, n_bins);
    pthread_mutex_unlock(&tune_lock);
}

/* ---- AIRSPACE's dump1090: the raw samples into its standard input ---- */

static pthread_mutex_t feed_lock = PTHREAD_MUTEX_INITIALIZER;
static int             feed_fd = -1;
static bool            feed_odd;        /* an I/Q pair half written: its other half owed first */
static atomic_bool     feed_gone;       /* dump1090 went (the stream saw it): the tuner back, by the watchdog */

static void feed_set(int fd) {
    pthread_mutex_lock(&feed_lock);
    if (feed_fd >= 0) close(feed_fd);   /* dump1090 at the end of its input: it ends */
    feed_fd  = fd;
    feed_odd = false;
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    atomic_store(&feeding, fd >= 0);
    pthread_mutex_unlock(&feed_lock);
    apply_tuning();
}

/* On the stream's thread: never waiting; what does not fit in the pipe is dropped, the pairs kept whole */
static void feed_out(const uint8_t *b, uint32_t n) {
    pthread_mutex_lock(&feed_lock);
    if (feed_fd >= 0) {
        ssize_t k = feed_odd ? write(feed_fd, "\x7f", 1) : 0;

        if (k == 1) feed_odd = false;
        if (!feed_odd) {
            k = write(feed_fd, b, n);
            if (k > 0 && (k & 1)) feed_odd = true;
        }
        if (k < 0 && errno == EPIPE) {
            close(feed_fd);
            feed_fd = -1;
            atomic_store(&feeding, false);
            atomic_store(&feed_gone, true);
        }
    }
    pthread_mutex_unlock(&feed_lock);
}

/* ---- The client's commands, on its own thread ---- */

static void command(char *line, int fd) {
    if (line[0] == 'r') {
        atomic_store(&rows_on, atoi(line + 1) != 0);
    } else if (line[0] == 'f') {
        uint64_t hz = strtoull(line + 1, NULL, 10);

        if (hz >= 24000000 && hz <= 1766000000) {
            pthread_mutex_lock(&tune_lock);
            want_centre = hz;
            pthread_mutex_unlock(&tune_lock);
            apply_tuning();
        }
    } else if (line[0] == 's') {
        uint32_t r = (uint32_t)strtoul(line + 1, NULL, 10);

        if (r >= 900001 && r <= 3200000 && !replay) {
            pthread_mutex_lock(&tune_lock);
            want_rate = r;
            pthread_mutex_unlock(&tune_lock);
            apply_tuning();
        }
    } else if (line[0] == 'g') {
        pthread_mutex_lock(&tune_lock);
        want_gain = atoi(line + 1);
        pthread_mutex_unlock(&tune_lock);
        apply_tuning();
    } else if (line[0] == 'l') {
        char    *e;
        uint64_t hz = strtoull(line + 1, &e, 10);

        pthread_mutex_lock(&pend_lock);
        pend.listen    = true;
        pend.listen_hz = hz;
        pend.mode      = hz ? (int)strtol(e, NULL, 10) : 0;
        pthread_mutex_unlock(&pend_lock);
    } else if (line[0] == 'w') {          /* "w <Hz>": the channel watched for SAME alerts, 0 none */
        pthread_mutex_lock(&pend_lock);
        pend.watch    = true;
        pend.watch_hz = strtoull(line + 1, NULL, 10);
        pthread_mutex_unlock(&pend_lock);
    } else if (line[0] == 'a') {
        feed_set(fd);                   /* -1: "a 0", the samples dump1090's no more */
        fd = -1;
    } else if (line[0] == 'Q') {
        stop = 1;
        pthread_mutex_lock(&tune_lock);
        if (dev_now) rtlsdr_cancel_async(dev_now);
        pthread_mutex_unlock(&tune_lock);
    }
    if (fd >= 0) close(fd);
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    for (;;) {
        char line[128];
        union {
            struct cmsghdr h;
            char           b[CMSG_SPACE(sizeof(int) * 4)];
        } cm;
        struct iovec  io = {.iov_base = line, .iov_len = sizeof(line) - 1};
        struct msghdr m  = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof(cm.b)};
        ssize_t       n  = recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
        int           got = -1;

        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        line[n] = 0;
        for (struct cmsghdr *h = CMSG_FIRSTHDR(&m); h; h = CMSG_NXTHDR(&m, h)) {
            if (h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) continue;
            int k = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));

            for (int i = 0; i < k; i++) {
                int x;

                memcpy(&x, CMSG_DATA(h) + i * sizeof(int), sizeof(int));
                if (got < 0) got = x;
                else close(x);
            }
        }
        command(line, got);
    }
    /* Gone (or replaced): what it asked for ends; the stick goes on, tuned as it was */
    pthread_mutex_lock(&out_lock);
    if (client == fd) client = -1;
    pthread_mutex_unlock(&out_lock);
    close(fd);
    atomic_store(&rows_on, false);
    pthread_mutex_lock(&pend_lock);
    pend.listen    = true;
    pend.listen_hz = 0;
    pend.mode      = 0;
    pend.watch     = true;
    pend.watch_hz  = 0;
    pthread_mutex_unlock(&pend_lock);
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
    if (pend.watch || ((pend.centre || pend.rate) && watch.hz))
        chan_set(&watch, (pend.watch ? pend.watch_hz : watch.hz) ? 1 : 0, pend.watch ? pend.watch_hz : watch.hz,
                 *centre, rate_now);
    pend.centre = pend.rate = pend.listen = pend.watch = false;
    pthread_mutex_unlock(&pend_lock);
}

/* ---- The stream: rows, audio and alerts as asked, or dump1090's samples ---- */

/* Each transfer the stick sends, on librtlsdr's thread: the commands waiting first */
static void on_samples(unsigned char *buf, uint32_t n, void *ctx) {
    int  bins = n_bins;
    bool rows;
    (void)ctx;

    atomic_store(&last_ms, now_ms());
    if (atomic_load(&s_now) != S_ON) atomic_store(&s_now, S_ON);
    if (stop) {
        if (term_dev) rtlsdr_cancel_async(term_dev);
        return;
    }
    commands(&centre_now, &fresh);
    if (atomic_load(&feeding)) {
        feed_out(buf, n);
        return;
    }
    rows = atomic_load(&rows_on);
    if (!rows) {                        /* rows again later: from a clean start */
        nfft = fill = 0;
        row_left = per_row_now;
        fresh    = true;
        if (!lis.mode && !watch.mode) return;
    }
    for (uint32_t off = 0; off + 1 < n; off += 2) {
        if (lis.mode) chan_sample(&lis, buf[off], buf[off + 1]);
        if (watch.mode) chan_sample(&watch, buf[off], buf[off + 1]);
        if (!rows) continue;
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
    }
}

/* -i: a recording in place of the stick, at its own pace and over again */
static void replay_run(void) {
    static uint8_t chunk[READ_LEN];
    FILE          *f = fopen(replay, "rb");

    if (!f) {
        atomic_store(&s_now, S_NOSTICK);
        sleep(2);
        return;
    }
    pthread_mutex_lock(&tune_lock);
    snprintf(tuner_s, sizeof(tuner_s), "REPLAY");
    cur_rate   = want_rate;
    cur_centre = 0;
    pthread_mutex_unlock(&tune_lock);
    apply_tuning();
    while (!stop) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);

        if (n < 2) {
            if (ftell(f) < 2) break;
            rewind(f);
            continue;
        }
        on_samples(chunk, (uint32_t)n, NULL);
        usleep((useconds_t)(n / 2 * 1000000.0 / rate_now));
    }
    fclose(f);
    sleep(1);
}

/* The stick: opened when it is there, streaming till it is pulled out */
static void *stick_thread(void *arg) {
    (void)arg;
    while (!stop) {
        rtlsdr_dev_t *dev;

        if (replay) {
            replay_run();
            continue;
        }
        if ((int)rtlsdr_get_device_count() <= dev_index) {
            atomic_store(&s_now, S_NOSTICK);
            sleep(1);
            continue;
        }
        if (rtlsdr_open(&dev, (uint32_t)dev_index) != 0) {     /* another program has it: tried again, not forced */
            atomic_store(&s_now, S_BUSY);
            sleep(2);
            continue;
        }
        rtlsdr_set_bias_tee(dev, 0);
        pthread_mutex_lock(&tune_lock);
        dev_now    = dev;
        cur_centre = 0;
        cur_rate   = 0;
        cur_gain   = INT_MIN;
        snprintf(tuner_s, sizeof(tuner_s), "%s", tuner_name(rtlsdr_get_tuner_type(dev)));
        pthread_mutex_unlock(&tune_lock);
        apply_tuning();
        rtlsdr_reset_buffer(dev);
        atomic_store(&last_ms, now_ms());
        atomic_store(&s_now, S_OPENING);
        fprintf(stderr, "rtlspec: the stick open, %s\n", tuner_s);

        /* Several transfers queued all the time (as rtl_sdr and dump1090 read): one at a time, what the stick sent
         * between two of them was lost, a quarter of it whatever the rate */
        term_dev = dev;
        rtlsdr_read_async(dev, on_samples, NULL, 12, READ_LEN);

        /* The stream ended with the stick still there (not pulled out, not told to end): left open and alone till it
         * is; closed and opened again, it would not be mended, and that is what wears the USB host */
        if (!stop && (int)rtlsdr_get_device_count() > dev_index) {
            fprintf(stderr, "rtlspec: the stream ended, the stick still there: kept as it is till it is pulled out\n");
            atomic_store(&s_now, S_STALLED);
            while (!stop && (int)rtlsdr_get_device_count() > dev_index) sleep(1);
        }
        term_dev = NULL;
        pthread_mutex_lock(&tune_lock);
        dev_now = NULL;
        pthread_mutex_unlock(&tune_lock);
        rtlsdr_close(dev);
        fprintf(stderr, "rtlspec: the stick closed (%s)\n", stop ? "the daemon ends" : "pulled out");
        atomic_store(&s_now, S_NOSTICK);
    }
    atomic_store(&stick_done, true);
    return NULL;
}

/* Each 200 ms: the state told when it changes, a stall seen, dump1090's samples taken back when it went */
static void *watch_thread(void *arg) {
    char     said[96] = "", now[96];
    uint32_t looked = 0;
    (void)arg;

    while (!stop) {
        int s = atomic_load(&s_now);

        if (atomic_exchange(&feed_gone, false)) apply_tuning();
        if ((s == S_ON || s == S_OPENING) && now_ms() - atomic_load(&last_ms) > STALL_MS) {
            fprintf(stderr, "rtlspec: nothing from the stick for %d s\n", STALL_MS / 1000);
            atomic_store(&s_now, S_STALLED);
        }
        /* stalled and pulled out, its stream not ended by it: ended here, so it is closed and the next one opened */
        if (atomic_load(&s_now) == S_STALLED && term_dev && now_ms() - looked > 1000) {
            looked = now_ms();
            if ((int)rtlsdr_get_device_count() <= dev_index) rtlsdr_cancel_async(term_dev);
        }
        hello(now, sizeof(now));
        if (strcmp(now, said)) {
            put(now, strlen(now));
            snprintf(said, sizeof(said), "%s", now);
        }
        usleep(200000);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int      bins = 2048, rows = 20, gain = 280, opt, ls;
    bool     front = false;
    uint32_t rate   = 2400000;
    uint64_t centre = 162550000;

    while ((opt = getopt(argc, argv, "Fd:f:s:n:r:g:i:")) != -1) {
        switch (opt) {
            case 'F': front = true; break;
            case 'd': dev_index = atoi(optarg); break;
            case 'f': centre = strtoull(optarg, NULL, 10); break;
            case 's': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'n': bins = atoi(optarg); break;
            case 'r': rows = atoi(optarg); break;
            case 'g': gain = atoi(optarg); break;
            case 'i': replay = optarg; break;
            default: return 1;
        }
    }
    if (bins < 64 || bins > MAX_BINS || (bins & (bins - 1)) || rows < 1 || rows > 50) return 1;

    /* The name taken first: a second daemon stops here. Listened on once in the background, so a client is told this
     * process (SO_PEERCRED), and a GUI that started it connects a moment later */
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    socklen_t          alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));

    memcpy(a.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));
    ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ls < 0) return 1;
    if (bind(ls, (struct sockaddr *)&a, alen) != 0) {
        printf("RTLSPEC RUNNING\n");
        return 0;
    }
    if (!front) {                       /* in the background, nobody's child: no GUI's end is its end */
        pid_t p = fork();

        if (p < 0) return 1;
        if (p > 0) _exit(0);
        setsid();
        if ((p = fork()) < 0) return 1;
        if (p > 0) _exit(0);
        if (chdir("/") != 0) return 1;
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
    }
    listen(ls, 4);

    struct sigaction sa = {.sa_handler = on_term};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);           /* a client or dump1090 gone is seen as an error, not the daemon's end */

    want_centre = centre;
    want_rate   = rate;
    want_gain   = gain;
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

    pthread_t stick, wd, ct;
    bool      ct_on = false;

    pthread_create(&stick, NULL, stick_thread, NULL);
    pthread_create(&wd, NULL, watch_thread, NULL);
    fprintf(stderr, "rtlspec: daemon %d running\n", (int)getpid());

    while (!stop) {
        struct pollfd p = {.fd = ls, .events = POLLIN};
        char          h[96];
        int           c;

        if (poll(&p, 1, 500) <= 0) continue;
        if ((c = accept4(ls, NULL, NULL, SOCK_CLOEXEC)) < 0) continue;
        int big = 1 << 20;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
        pthread_mutex_lock(&out_lock);
        if (client >= 0) shutdown(client, SHUT_RDWR);   /* the one before: its thread ends */
        pthread_mutex_unlock(&out_lock);
        if (ct_on) pthread_join(ct, NULL);
        pthread_mutex_lock(&out_lock);
        client = c;
        pthread_mutex_unlock(&out_lock);
        hello(h, sizeof(h));
        put(h, strlen(h));
        ct_on = pthread_create(&ct, NULL, client_thread, (void *)(intptr_t)c) == 0;
    }

    /* Told to end: the stream cancelled, the stick closed (6 s for it, then gone anyway) */
    if (term_dev) rtlsdr_cancel_async(term_dev);
    for (int i = 0; i < 60 && !atomic_load(&stick_done); i++) usleep(100000);
    fprintf(stderr, "rtlspec: daemon ends\n");
    _exit(0);
}
