/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - x6x00-df: direction finding with a KrakenSDR (docs/plans/kraken_df_plan.md, Phase 1). The bare
 *  minimum the plan's section 1.4 lists, and nothing else: no Heimdall, no ZeroMQ, no web page.
 *
 *    - the Kraken found by its serials (1000-1004, CH0-CH4), never by RTL number; opened one at a time, equal manual
 *      gain, every AGC off, bias-T and noise source off;
 *    - one reader per stick (librtlsdr's async read) into a ring of its own, with the time each transfer came in;
 *    - calibration (df_cal.c) on one block of the Kraken's own noise source (GPIO 0 of CH0): the streams aligned in
 *      software (whole samples, first from the transfers' times, then from the noise), the rest of the delay, the
 *      phase and the gain corrected per signal by the engine (df_engine.c), at start, after a retune or a gain
 *      change, on a slip and on a timer;
 *    - the engine on every 0.1 s block: the bearing and how far to trust it, every channel's level and whether it was
 *      used (df_proto.h's DFDO), CH0's spectrum (DFSP), the state (DFST);
 *    - one client on the abstract socket "@x6x00-df" (SOCK_SEQPACKET), the GUI's page I; text commands back.
 *
 *  Receive only: nothing here transmits. The noise source is inside the Kraken and disconnects the antennas while
 *  it is on (so nothing else may listen on a Kraken stick while this runs).
 *
 *    x6x00-df [-F] [-S name] [-p] [-f HZ] [-s RATE] [-g TENTHS] [-t OFFSET,BW] [-a uca|ula,N,MM] [-m METHOD]
 *             [-i SECONDS] [-n CHANNELS] [-D] [-T BEARING[,DEG_PER_S[,DEAD_CH]]]
 *      -F  in the foreground (else it goes to the background; its log /tmp/x6x00-df.log)
 *      -p  print one line per estimate (with -F: a test from a shell)
 *      -D  leave the tuners' PLL dither on (with our patched librtlsdr it is switched off: test 1g compares)
 *      -n  open CH0 to CH(n-1) only (2-5; CH0 always, it has the noise source): a bench with another program on the
 *          last stick. The array keeps its positions: UCA-5 with -n 4 is the circle's first four
 *      -T  a synthetic Kraken instead of the sticks: a carrier at BEARING degrees (turning DEG_PER_S), on 6 s
 *          and off 3 s, each channel with its own delay, phase and gain, and the noise source; DEAD_CH's antenna
 *          gets 30 dB less of the signal. For page I without a Kraken, and for tests
 */

#define _GNU_SOURCE
#include "df.h"
#include "df_proto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#ifndef DF_NO_RTL
#include <rtl-sdr.h>
/* the PLL's dither switch: in our librtlsdr with buildroot's rtlsdr-blog patch 0002 (KrakenRF's has it too). It is
 * a must: with the dither on, the radio's Kraken's bearing swept the whole circle every 3 s; off, it held within 0.6
 * degrees for 30 s (2026-11-17). A weak symbol, so a library without it still links, and the daemon says so */
extern int rtlsdr_set_dithering(rtlsdr_dev_t *dev, int dither) __attribute__((weak));
#else
typedef struct rtlsdr_dev rtlsdr_dev_t;
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NCH_KRAKEN  5
#define RING_BITS   21
#define RING        (1L << RING_BITS)           /* samples a channel keeps: 2 s at 1.024 MS/s, 0.87 s at 2.4 */
#define BUFS        15                          /* transfers queued per stick */
#define MAXLAG      4096                        /* whole-sample delays looked for around the first guess */
#define SETTLE_S    0.2                         /* after the noise source is switched */
#define HOLD_S      30.0

/* ------------------------------------------------------------------------------------------------ the channels */

typedef struct {
    char                serial[16];
    rtlsdr_dev_t       *dev;
    uint8_t            *ring;                   /* RING samples, I then Q */
    _Atomic int64_t     wr;                     /* samples written */
    _Atomic double      t0;                     /* when its sample 0 came, by the earliest transfers since a reset */
    _Atomic int         t0_reset;
    atomic_bool         ended;                  /* its read ended by itself (pulled out) */
    _Atomic double      last_t;
    pthread_t           th;
    bool                th_on;
} chan_t;

static chan_t   ch[DF_MAXCH];
static int      nch;                            /* streaming */
static int      n_open = NCH_KRAKEN;            /* -n: CH0 to CH(n-1) only (a bench with B on CH4) */
static bool     dither_on;                      /* -D: the tuners' PLL dither left on (test 1g) */
static bool     synth;
static double   synth_bearing = 40, synth_rate = 0;
static int      synth_dead = -1;
static atomic_bool synth_noise;                 /* the synthetic noise source */
static atomic_bool quit_now;
static atomic_bool streaming;

/* settings (the worker's own; commands are queued) */
static uint32_t fc = 433920000, rate = 1024000, bw = 12500;
static int32_t  f_off = 100000;
static int      gain = 280, method = DF_MUSIC, sql = 80, cal_every = 120;
static unsigned user_off;
static df_array_t arr;

/* state for DFST */
static int      st = DF_ST_NODEV, cals, cal_fails, slips, overruns, cpu_pct;
static char     why[64];
static char     note[64];                       /* what stays true while open (channels the USB could not feed) */
static df_cal_t cal;
static bool     have_cal;
static double   cal_at = -1;
static int64_t  kb[DF_MAXCH];                   /* channel m read kb[m] later than CH0 */
static df_est_t last;
static double   last_good_at = -1, last_bearing = -1, last_mirror = -1, last_uncert;
/* What HOLD shows once the signal is under the squelch: the last estimate over it whose fit is worth holding, with its
 * own fit and +-. An estimate taken across a burst's end (half signal, half noise) is over the squelch too, and held as
 * it was it put a wild bearing on screen at a red fit: 198.7 degrees after bursts at 057 (2026-11-17, page I at 915
 * MHz). With none such since the signal came, the last one over the squelch, as before */
#define HOLD_FIT    0.50
static double   held_bearing = -1, held_mirror = -1, held_uncert, held_fit;
static bool     slip_shown;

static double now_s(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    va_list ap;
    time_t  t = time(NULL);
    char    ts[16];

    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    fprintf(stderr, "%s df: ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* a transfer in: into the ring, its time noted */
static void put(chan_t *c, const uint8_t *buf, uint32_t len) {
    int64_t w = atomic_load(&c->wr), n = len / 2;
    size_t  at = (size_t)(w & (RING - 1)), first = (size_t)(RING - (int64_t)at) < (size_t)n ? (size_t)(RING - at) : (size_t)n;
    double  t = now_s();

    memcpy(c->ring + 2 * at, buf, 2 * first);
    if ((int64_t)first < n) memcpy(c->ring, buf + 2 * first, 2 * ((size_t)n - first));
    atomic_store(&c->wr, w + n);
    atomic_store(&c->last_t, t);

    double est = t - (double)(w + n) / rate;   /* its sample 0's time if this transfer had no delay at all */

    if (atomic_exchange(&c->t0_reset, 0) || est < atomic_load(&c->t0)) atomic_store(&c->t0, est);
}

/* ------------------------------------------------------------------------------------------- the real Kraken */

#ifndef DF_NO_RTL
static void on_read(unsigned char *buf, uint32_t len, void *ctx) {
    chan_t *c = ctx;

    if (!atomic_load(&streaming)) {
        rtlsdr_cancel_async(c->dev);
        return;
    }
    put(c, buf, len);
}

/* 64 KiB transfers (DF_XFER): 15 of them a stick, 4.8 MB for five (usbfs_memory_mb is 16) */
static void *reader(void *arg) {
    chan_t     *c = arg;
    const char *e = getenv("DF_XFER");
    uint32_t    len = e ? (uint32_t)atoi(e) & ~511u : 65536;

    if (len < 4096) len = 65536;

    rtlsdr_reset_buffer(c->dev);
    rtlsdr_read_async(c->dev, on_read, c, BUFS, len);
    if (atomic_load(&streaming)) atomic_store(&c->ended, true);
    return NULL;
}

static int nearest_gain(rtlsdr_dev_t *d, int tenths) {
    int g[64], n = rtlsdr_get_tuner_gains(d, NULL), best = 0;

    if (n <= 0 || n > 64) return tenths;
    rtlsdr_get_tuner_gains(d, g);
    for (int i = 1; i < n; i++)
        if (abs(g[i] - tenths) < abs(g[best] - tenths)) best = i;
    return g[best];
}

static bool read_line(const char *path, char *b, size_t n) {
    FILE *f = fopen(path, "r");
    bool  ok = f && fgets(b, (int)n, f);

    if (f) fclose(f);
    if (ok) b[strcspn(b, "\r\n")] = 0;
    return ok;
}

/* the five serials on USB? From sysfs: no USB traffic to sticks another program may hold */
static bool kraken_there(void) {
    DIR           *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    unsigned       got = 0;

    while (d && (e = readdir(d))) {
        char p[300], v[8], pr[8], sn[32];

        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        if (!read_line(p, v, sizeof(v)) || strcmp(v, "0bda")) continue;
        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idProduct", e->d_name);
        if (!read_line(p, pr, sizeof(pr)) || (strcmp(pr, "2838") && strcmp(pr, "2832"))) continue;
        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/serial", e->d_name);
        if (!read_line(p, sn, sizeof(sn))) continue;
        int k = atoi(sn) - 1000;

        if (k >= 0 && k < NCH_KRAKEN && strlen(sn) == 4) got |= 1u << k;
    }
    if (d) closedir(d);
    if (got != (1u << NCH_KRAKEN) - 1) {        /* say which are missing (page I shows it) */
        int n = snprintf(why, sizeof(why), "waiting for the Kraken's sticks:");

        for (int k = 0; k < NCH_KRAKEN && n < (int)sizeof(why) - 6; k++)
            if (!(got >> k & 1)) n += snprintf(why + n, sizeof(why) - (size_t)n, " %d", 1000 + k);
        return false;
    }
    return true;
}

static void hw_close(void) {
    atomic_store(&streaming, false);
    for (int m = 0; m < nch; m++)
        if (ch[m].dev) rtlsdr_cancel_async(ch[m].dev);
    for (int m = 0; m < nch; m++)
        if (ch[m].th_on) pthread_join(ch[m].th, NULL), ch[m].th_on = false;
    if (nch && ch[0].dev) rtlsdr_set_bias_tee_gpio(ch[0].dev, 0, 0);     /* the noise source off */
    for (int m = 0; m < nch; m++)
        if (ch[m].dev) rtlsdr_close(ch[m].dev), ch[m].dev = NULL;
}

/* Each stick opened, set up and started before the next (as five rtl_test started 2 s apart all streamed; four
 * opened first and started 0.3 or 1 s apart left the fourth with nothing, 2026-11-17), DF_STAGGER_MS apart (1000) */
static int hw_open(void) {
    const char *e = getenv("DF_STAGGER_MS");
    int         stagger = e ? atoi(e) : 1000;

    nch = 0;
    atomic_store(&streaming, true);
    for (int m = 0; m < n_open; m++) {
        chan_t *c = &ch[m];
        int     idx;

        snprintf(c->serial, sizeof(c->serial), "%d", 1000 + m);
        if ((idx = rtlsdr_get_index_by_serial(c->serial)) < 0 || rtlsdr_open(&c->dev, (uint32_t)idx) != 0) {
            c->dev = NULL;
            if (m >= 3) {                       /* CH0 and two more: a bearing from those, the rest GONE */
                snprintf(note, sizeof(note), "CH%d-%d would not open (another program has them)", m, n_open - 1);
                logf_("%s", note);
                break;
            }
            snprintf(why, sizeof(why), "CH%d (serial %.6s) would not open: held by another program?", m, c->serial);
            hw_close();
            nch = 0;
            return -1;
        }
        nch = m + 1;
        rtlsdr_set_bias_tee(c->dev, 0);         /* GPIO 0: on CH0 the noise source, off */
        rtlsdr_set_sample_rate(c->dev, rate);
        rtlsdr_set_center_freq(c->dev, fc);
        rtlsdr_set_tuner_gain_mode(c->dev, 1);
        rtlsdr_set_tuner_gain(c->dev, nearest_gain(c->dev, gain));
        rtlsdr_set_agc_mode(c->dev, 0);
        if (rtlsdr_set_dithering && !dither_on) rtlsdr_set_dithering(c->dev, 0);    /* retunes to fc */
        if (getenv("DF_TESTMODE")) rtlsdr_set_testmode(c->dev, 1);                  /* a bench check: the counter */
        atomic_store(&c->wr, 0);
        atomic_store(&c->ended, false);
        atomic_store(&c->t0_reset, 1);
        atomic_store(&c->last_t, now_s() + stagger / 1000.0 * (n_open - m));     /* not "stopped" while the rest open */
        c->th_on = pthread_create(&c->th, NULL, reader, c) == 0;
        if (m + 1 < n_open) usleep((useconds_t)stagger * 1000);
    }
    gain = nearest_gain(ch[0].dev, gain);
    if (!rtlsdr_set_dithering && !note[0])
        snprintf(note, sizeof(note), "librtlsdr without the dither switch: bearings will wander");
    logf_("Kraken open: %u Hz, %u S/s, gain %.1f dB, PLL dither %s", fc, rate, gain / 10.0,
          !rtlsdr_set_dithering ? "as the library leaves it (no switch)" : dither_on ? "on" : "off");
    return 0;
}

static void hw_noise(bool on) {
    if (nch && ch[0].dev) rtlsdr_set_bias_tee_gpio(ch[0].dev, 0, on);
}

/* channels from k on let go (they streamed nothing): their reads cancelled, closed */
static void hw_drop_from(int k) {
    for (int m = k; m < nch; m++)
        if (ch[m].dev) rtlsdr_cancel_async(ch[m].dev);
    for (int m = k; m < nch; m++)
        if (ch[m].th_on) pthread_join(ch[m].th, NULL), ch[m].th_on = false;
    for (int m = k; m < nch; m++)
        if (ch[m].dev) rtlsdr_close(ch[m].dev), ch[m].dev = NULL;
    nch = k;
}

static void hw_tune(void) {
    for (int m = 0; m < nch; m++) rtlsdr_set_center_freq(ch[m].dev, fc);
}

static void hw_gain(void) {
    gain = nearest_gain(ch[0].dev, gain);
    for (int m = 0; m < nch; m++) rtlsdr_set_tuner_gain(ch[m].dev, gain);
}
#else
static void hw_drop_from(int k) { (void)k; }
static bool kraken_there(void) { return false; }
static void hw_close(void) {}
static int  hw_open(void) { return -1; }
static void hw_noise(bool on) { (void)on; }
static void hw_tune(void) {}
static void hw_gain(void) {}
#endif

/* ------------------------------------------------------------------------------------- the synthetic Kraken */

#define SYN_CHUNK   8192
#define SYN_TAB     65536

static float          syn_rx[SYN_TAB], syn_src[SYN_TAB];   /* receiver noise, the noise source: unit gaussian */
static float complex  syn_cw[4096];             /* the transmitter: 12 counts, a turn in 4096 steps */
static pthread_t      syn_th;
static bool           syn_on;
static int            syn_o[DF_MAXCH];          /* where channel m's sample 0 is in the source's time */
static int            syn_start[DF_MAXCH];      /* when its stream started (what the transfers' times tell) */
static float complex  syn_lo[DF_MAXCH];         /* each tuner's phase and gain: new at every retune */
static _Atomic int    syn_retuned;

static double gauss(unsigned *s) {
    double u = (rand_r(s) + 1.0) / (RAND_MAX + 2.0), v = (rand_r(s) + 1.0) / (RAND_MAX + 2.0);

    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void syn_new_lo(unsigned *s) {
    for (int m = 0; m < nch; m++) {
        double ph = 2 * M_PI * (rand_r(s) / (double)RAND_MAX), g = 0.8 + 0.45 * (rand_r(s) / (double)RAND_MAX);

        syn_lo[m] = (float complex)(g * cexp(I * ph));
    }
}

static void *syn_thread(void *arg) {
    (void)arg;
    unsigned       seed = 12345;
    int            span, omin = 0, omax = 0;
    float complex *src;
    double         t_start = now_s();
    int64_t        j = 0;                       /* every channel's next sample */
    uint8_t       *out = malloc(2 * SYN_CHUNK);

    for (int m = 0; m < nch; m++) {
        if (syn_o[m] < omin) omin = syn_o[m];
        if (syn_o[m] > omax) omax = syn_o[m];
    }
    span = omax - omin;
    src = malloc(sizeof(*src) * (size_t)(SYN_CHUNK + span));
    while (atomic_load(&streaming)) {
        double due = t_start + (double)(j + SYN_CHUNK) / rate, t = now_s();

        if (due > t) usleep((useconds_t)((due - t) * 1e6));
        if (atomic_exchange(&syn_retuned, 0)) syn_new_lo(&seed);

        /* the source's time u = j + o_m on channel m; the transmitter: a carrier at the target's offset, on 6 s,
         * off 3 s */
        int64_t u0 = j + omin;
        double  tsec = (double)u0 / rate, bearing = fmod(synth_bearing + synth_rate * tsec + 720, 360);
        bool    on = fmod(tsec, 9.0) < 6.0, noise = atomic_load(&synth_noise);
        double  lambda = DF_C / (fc + f_off);
        double complex sv[DF_MAXCH];

        df_steer_at(&arr, lambda, bearing, sv);
        if (noise) {
            for (int i = 0; i < SYN_CHUNK + span; i++)
                src[i] = 40.0f * (syn_src[(u0 + i) & (SYN_TAB - 1)] + I * syn_src[(u0 + i + 7919) & (SYN_TAB - 1)]) / 1.4142f;
        } else if (on) {                        /* a carrier: a table and a phase that counts 2^32 a turn */
            double   turns = fmod((double)f_off * (double)u0 / rate, 1.0);
            uint32_t ph = (uint32_t)(int64_t)llround((turns < 0 ? turns + 1 : turns) * 4294967296.0);
            uint32_t inc = (uint32_t)(int64_t)llround((double)f_off / rate * 4294967296.0);

            for (int i = 0; i < SYN_CHUNK + span; i++, ph += inc) src[i] = syn_cw[ph >> 20];
        } else
            memset(src, 0, sizeof(*src) * (size_t)(SYN_CHUNK + span));
        int r0 = rand_r(&seed) & (SYN_TAB - 1);

        for (int m = 0; m < nch; m++) {
            const float complex *s = src + (syn_o[m] - omin);
            float complex        a = noise ? syn_lo[m] : syn_lo[m] * (float complex)sv[m] * (m == synth_dead ? 0.03f : 1.0f);
            int                  r = (r0 + 4099 * (m + 1)) & (SYN_TAB - 1);

            for (int i = 0; i < SYN_CHUNK; i++) {
                float complex x = s[i] * a;
                float         re = crealf(x) + 4.0f * syn_rx[(r + 2 * i) & (SYN_TAB - 1)] + 127.5f;
                float         im = cimagf(x) + 4.0f * syn_rx[(r + 2 * i + 1) & (SYN_TAB - 1)] + 127.5f;

                out[2 * i] = (uint8_t)(re < 0 ? 0 : re > 255 ? 255 : re);
                out[2 * i + 1] = (uint8_t)(im < 0 ? 0 : im > 255 ? 255 : im);
            }
            put(&ch[m], out, 2 * SYN_CHUNK);
        }
        for (int m = 0; m < nch; m++) atomic_store(&ch[m].t0, t_start + (double)syn_start[m] / rate);
        j += SYN_CHUNK;
    }
    free(src);
    free(out);
    return NULL;
}

static int syn_open(void) {
    unsigned s = 777;
    static const int o[DF_MAXCH] = {0, -1234, 517, 2890, -77, 33, 1500, -900};
    static const int hw[DF_MAXCH] = {0, 3, -2, 7, 1, 0, 2, -1};    /* the cables' and filters' own delays */

    nch = n_open;
    for (int i = 0; i < SYN_TAB; i++) syn_rx[i] = (float)gauss(&s), syn_src[i] = (float)gauss(&s);
    for (int i = 0; i < 4096; i++) syn_cw[i] = (float complex)(12.0 * cexp(I * 2 * M_PI * i / 4096));
    for (int m = 0; m < nch; m++) {
        snprintf(ch[m].serial, sizeof(ch[m].serial), "SYN%d", m);
        syn_o[m] = o[m] - hw[m];
        syn_start[m] = o[m];
        atomic_store(&ch[m].wr, 0);
        atomic_store(&ch[m].ended, false);
    }
    syn_new_lo(&s);
    atomic_store(&streaming, true);
    syn_on = pthread_create(&syn_th, NULL, syn_thread, NULL) == 0;
    logf_("synthetic Kraken: transmitter at %.0f degrees, turning %.1f a second, dead channel %d", synth_bearing,
          synth_rate, synth_dead);
    return 0;
}

static void syn_close(void) {
    atomic_store(&streaming, false);
    if (syn_on) pthread_join(syn_th, NULL), syn_on = false;
}

/* ------------------------------------------------------------------------------------------- the device, either */

static bool dev_there(void) { return synth || kraken_there(); }
static bool dev_is_open;                       /* apply() leaves the sticks and the engine alone while not */
static int  dev_open(void) {
    int r = synth ? syn_open() : hw_open();

    dev_is_open = r == 0;
    return r;
}
static void dev_close(void) {
    synth ? syn_close() : hw_close();
    dev_is_open = false;
}
static void dev_noise(bool on) { synth ? atomic_store(&synth_noise, on) : hw_noise(on); }
static void dev_tune(void) { synth ? atomic_store(&syn_retuned, 1) : hw_tune(); }
static void dev_gain(void) { synth ? atomic_store(&syn_retuned, 1) : hw_gain(); }

/* ------------------------------------------------------------------------------------------------- the socket */

static int             ls = -1, cl = -1;
static pthread_mutex_t cmd_lock = PTHREAD_MUTEX_INITIALIZER;
static char            cmds[16][160];
static int             ncmds;
static double          cmd_t;                   /* the last command's time (cmd_lock) */
static bool            had_client;              /* (cmd_lock) */
/* Started by page I (not -F): it ends when page I lets go, also while it still waits for the Kraken, and on its own
 * when no client has been there for NO_CLIENT_S (the GUI gone without a Q: a crash, a restart off page I). Page I
 * connects again within a second or two while it is open. Before 2026-11-17 a Q sent while it waited for the sticks
 * was queued and never read: the daemon outlived page I and took the whole Kraken when it came back on USB */
static bool            on_its_own;
#define NO_CLIENT_S 15

static void send_msg(const void *m, size_t n) {
    if (cl >= 0 && send(cl, m, n, MSG_DONTWAIT | MSG_NOSIGNAL) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(cl);
        cl = -1;
    }
}

static void *sock_thread(void *arg) {
    double lost_t = now_s();

    (void)arg;
    while (!atomic_load(&quit_now)) {
        struct pollfd p[2] = {{.fd = ls, .events = POLLIN}, {.fd = cl, .events = POLLIN}};

        if (!on_its_own && cl < 0 && had_client && now_s() - lost_t > NO_CLIENT_S) {
            logf_("no client for %d s: ending", NO_CLIENT_S);
            atomic_store(&quit_now, true);
            break;
        }
        if (poll(p, cl >= 0 ? 2 : 1, 500) <= 0) continue;
        if (p[0].revents & POLLIN) {
            int c = accept(ls, NULL, NULL);

            if (c >= 0) {                       /* one client: a new one replaces the old */
                if (cl >= 0) close(cl);
                cl = c;
                pthread_mutex_lock(&cmd_lock);
                had_client = true;
                cmd_t = now_s();
                pthread_mutex_unlock(&cmd_lock);
                logf_("a client");
            }
        }
        if (cl >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char b[160];
            ssize_t n = recv(cl, b, sizeof(b) - 1, 0);

            if (n <= 0) {
                close(cl);
                cl = -1;
                lost_t = now_s();
                continue;
            }
            b[n] = 0;
            if (b[0] == 'Q') {                  /* at once: the worker may be waiting for the sticks */
                atomic_store(&quit_now, true);
                break;
            }
            pthread_mutex_lock(&cmd_lock);
            if (ncmds < 16) snprintf(cmds[ncmds++], sizeof(cmds[0]), "%s", b);
            cmd_t = now_s();
            pthread_mutex_unlock(&cmd_lock);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------- the worker */

static df_engine_t eng;
static int         blk_n;
static uint8_t    *blk[DF_MAXCH];
static int64_t     J;                           /* CH0's next block */
static bool        eng_on;

static int64_t wr_of(int m) { return atomic_load(&ch[m].wr); }

/* n samples of channel m from its own count a, out of the ring */
static void copy_ch(int m, int64_t a, int n, uint8_t *dst) {
    size_t at = (size_t)(a & (RING - 1)), first = RING - at < (size_t)n ? RING - at : (size_t)n;

    memcpy(dst, ch[m].ring + 2 * at, 2 * first);
    if (first < (size_t)n) memcpy(dst + 2 * first, ch[m].ring, 2 * ((size_t)n - first));
}

/* the channels' bytes from j (CH0's count) on, each kb[m] later, into dst; false if not written yet or gone */
static bool read_at(int64_t j, int n, uint8_t *const dst[]) {
    for (int m = 0; m < nch; m++) {
        int64_t a = j + kb[m];

        if (a < 0 || a + n > wr_of(m) || a < wr_of(m) - RING) return false;
    }
    for (int m = 0; m < nch; m++) copy_ch(m, j + kb[m], n, dst[m]);
    return true;
}

/* wait till channel data from j for n samples is there (and none of it gone); false: timed out or quitting */
static bool wait_for(int64_t j, int n, double timeout) {
    double until = now_s() + timeout;

    while (!atomic_load(&quit_now) && now_s() < until) {
        bool ok = true;

        for (int m = 0; m < nch && ok; m++) ok = j + kb[m] + n <= wr_of(m);
        if (ok) return true;
        for (int m = 0; m < nch; m++)
            if (atomic_load(&ch[m].ended)) return false;
        usleep(4000);
    }
    return false;
}

static void engine_setup(void) {
    if (eng_on) df_engine_free(&eng);
    memset(&eng, 0, sizeof(eng));
    eng.nch = arr.n <= nch ? arr.n : nch;
    eng.fs = rate;
    eng.fc = fc;
    eng.f = f_off;
    eng.bw = bw;
    eng.arr = arr;
    if (eng.arr.n > eng.nch) eng.arr.n = eng.nch;
    eng.method = (df_method_t)method;
    eng.sql_db = sql / 10.0;
    eng.cal_every_s = cal_every > 0 ? cal_every : -1;
    eng.user_off = user_off;
    blk_n = (int)(rate / 10);
    eng_on = df_engine_init(&eng, blk_n) == 0;
    if (!eng_on) logf_("engine: could not start (%d channels, array of %d)", eng.nch, arr.n);
    if (have_cal) df_engine_set_cal(&eng, &cal);
    for (int m = 0; m < DF_MAXCH; m++) {
        free(blk[m]);
        blk[m] = m < nch ? malloc(2 * (size_t)blk_n) : NULL;
    }
}

static void send_state(void);

/* the whole-sample delays from the transfers' times, before any noise: within a few ms */
static void coarse_align(void) {
    double t00 = atomic_load(&ch[0].t0);

    for (int m = 0; m < nch; m++) kb[m] = llround((t00 - atomic_load(&ch[m].t0)) * rate);
}

static float pw_mean(const float complex *x, int n) {
    double s = 0;

    for (int i = 0; i < n; i++) s += crealf(x[i]) * crealf(x[i]) + cimagf(x[i]) * cimagf(x[i]);
    return (float)(s / n);
}

/* One noise block: the noise source on, DF_CAL_N samples a channel after it settles, off again. search: look for
 * the whole delays (the first time, after a slip); else they are known and only checked */
static bool calibrate(bool search) {
    static float complex *x[DF_MAXCH];
    float                 pre[DF_MAXCH];
    int                   n = DF_CAL_N;
    df_cal_t              c;
    int64_t               j;
    bool                  ok = false;

    for (int m = 0; m < nch; m++)
        if (!x[m] && !(x[m] = malloc(sizeof(float complex) * (size_t)n))) return false;
    st = DF_ST_CAL;
    send_state();

    /* each channel's level before, from its newest 16k samples */
    for (int m = 0; m < nch; m++) {
        uint8_t b[2 * 16384];

        copy_ch(m, wr_of(m) - 16384, 16384, b);
        df_u8_to_cf(b, 16384, x[m]);
        pre[m] = pw_mean(x[m], 16384);
    }

    dev_noise(true);
    j = wr_of(0) + (int64_t)(SETTLE_S * rate);
    if (!wait_for(j, n, 3)) {
        dev_noise(false);
        goto failed;
    }
    {
        uint8_t *tmp = malloc(2 * (size_t)n);

        for (int m = 0; m < nch && tmp; m++) {
            copy_ch(m, j + kb[m], n, tmp);
            df_u8_to_cf(tmp, n, x[m]);
        }
        free(tmp);
    }
    dev_noise(false);
    J = wr_of(0) + (int64_t)(SETTLE_S * rate);  /* estimates again once the antennas are back */

    float rises[DF_MAXCH];

    for (int m = 0; m < nch; m++) {
        float rise = rises[m] = 10 * log10f(pw_mean(x[m], n) / (pre[m] + 1e-12f));

        if (rise < 6) {
            snprintf(why, sizeof(why), "CH%d: the noise source rose only %.1f dB", m, rise);
            goto failed;
        }
    }
    {
        static const int zero[DF_MAXCH];
        int              rc = -1;

        if (!search) rc = df_cal_run(x, nch, n, MAXLAG, zero, &c);
        if (rc) rc = df_cal_run(x, nch, n, MAXLAG, NULL, &c);
        if (rc) {
            snprintf(why, sizeof(why), "%.63s", c.why);
            logf_("noise rose %.0f %.0f %.0f %.0f dB", rises[0], nch > 1 ? rises[1] : 0, nch > 2 ? rises[2] : 0,
                  nch > 3 ? rises[3] : 0);
            goto failed;
        }
    }
    for (int m = 1; m < nch; m++) kb[m] += c.k[m];
    cal = c;
    have_cal = true;
    cal_at = now_s();
    cals++;
    why[0] = 0;
    ok = true;
    if (eng_on) df_engine_set_cal(&eng, &cal);
    {
        char l[260];
        int  n = 0;

        n += snprintf(l, sizeof(l), " noise +%.0f dB on CH0;", rises[0]);
        for (int m = 1; m < nch; m++)
            n += snprintf(l + n, sizeof(l) - (size_t)n, " CH%d %lld %+.1f deg %.2f dB (+%.0f, rip %.1f res %.2f)", m,
                          (long long)kb[m], cal.phi[m] * 180 / M_PI, 20 * log10(cal.gain[m]), rises[m],
                          cal.ripple_deg[m], cal.resid_deg[m]);
        logf_("calibrated:%s", l);
    }
    slip_shown = false;
    return true;
failed:
    cal_fails++;
    eng.since_cal = 0;                          /* the timer waits its whole interval again (not a retry a block) */
    if (!why[0]) snprintf(why, sizeof(why), "the noise block did not come in");
    logf_("calibration failed: %s", why);
    J = wr_of(0);
    return ok;
}

/* ------------------------------------------------------------------------------------------------- messages */

static int16_t ddeg(double d) { return d < 0 ? -1 : (int16_t)lrint(fmod(d, 360) * 10) % 3600; }
static int16_t clamp16(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v)); }

static int ch_cond(int m) {
    if (m >= nch || atomic_load(&ch[m].ended)) return DF_CH_GONE;
    if (user_off & (1u << m)) return DF_CH_OFF;
    if (!have_cal) return DF_CH_NOCAL;
    if (m < eng.nch && last.bad_ch == m) return DF_CH_SLIP;
    if (last.clip[m] > 0.01) return DF_CH_CLIP;
    if (m >= eng.nch) return DF_CH_OFF;
    if (eng.low[m]) return DF_CH_LOW;
    if (last.ch_db[m] < eng.sql_db) return DF_CH_QUIET;
    return DF_CH_OK;
}

static void send_state(void) {
    df_st_msg_t s;

    memset(&s, 0, sizeof(s));
    memcpy(s.tag, "DFST", 4);
    s.ver = DF_PROTO_VER;
    s.state = (uint8_t)st;
    s.nch = (uint8_t)nch;
    s.synth = synth;
    s.method = (uint8_t)method;
    s.arr_kind = (uint8_t)arr.kind;
    s.arr_n = (uint8_t)arr.n;
    s.user_off = (uint8_t)user_off;
    s.fc_hz = fc;
    s.f_off_hz = f_off;
    s.bw_hz = bw;
    s.rate = rate;
    s.gain_tenths = (int16_t)gain;
    s.sql_tenths = (int16_t)sql;
    s.cal_every_s = (uint16_t)(cal_every > 0 ? cal_every : 0);
    s.cal_age_s = cal_at < 0 ? 0xffff : (uint16_t)fmin(65534, now_s() - cal_at);
    s.cals = (uint32_t)cals;
    s.cal_fails = (uint32_t)cal_fails;
    s.slips = (uint32_t)slips;
    s.overruns = (uint32_t)overruns;
    s.arr_size_mm = (uint16_t)lrint(arr.size * 1000);
    for (int m = 0; m < arr.n && m < DF_PROTO_MAXCH; m++) {
        s.arr_x_mm[m] = clamp16(arr.x[m] * 1000);
        s.arr_y_mm[m] = clamp16(arr.y[m] * 1000);
    }
    s.cpu_pct = (uint8_t)cpu_pct;
    snprintf(s.why, sizeof(s.why), "%s", why[0] ? why : note);
    for (int m = 0; m < nch && m < DF_PROTO_MAXCH; m++) {
        df_ch_t *c = &s.ch[m];

        snprintf(c->serial, sizeof(c->serial), "%.7s", ch[m].serial);
        c->cond = (uint8_t)ch_cond(m);
        c->used = (last.used >> m) & 1;
        if (have_cal) {
            c->cal_k = clamp16((double)kb[m]);
            c->cal_phase_ddeg = clamp16(remainder(cal.phi[m] * 180 / M_PI, 360) * 10);
            c->cal_gain_mdb = clamp16(20 * log10(cal.gain[m] > 0 ? cal.gain[m] : 1) * 100);
            c->cal_margin_ddb = clamp16(cal.margin_db[m] * 10);
            c->cal_resid_cdeg = clamp16(cal.resid_deg[m] * 100);
            c->noise_ddbfs = clamp16(cal.level_db[m] * 10);
        }
    }
    send_msg(&s, sizeof(s));
}

static void send_estimate(const df_est_t *e, uint32_t seq) {
    df_do_msg_t d;
    double      t = now_s();

    memset(&d, 0, sizeof(d));
    memcpy(d.tag, "DFDO", 4);
    d.seq = seq;
    d.gate = e->gate;
    d.bad_ch = (int8_t)e->bad_ch;
    d.used_mask = (uint8_t)e->used;
    d.fit_pct = (uint8_t)lrint(fmin(1, fmax(0, e->doa.fit)) * 100);
    if (e->gate) {
        if (last_good_at < 0 || t - last_good_at >= HOLD_S) held_bearing = -1;     /* a signal anew */
        last_good_at = t;
        last_bearing = e->doa.bearing;
        last_mirror = e->doa.mirror;
        last_uncert = e->uncert_deg;
        if (e->doa.fit >= HOLD_FIT) {
            held_bearing = e->doa.bearing;
            held_mirror = e->doa.mirror;
            held_uncert = e->uncert_deg;
            held_fit = e->doa.fit;
        }
    }
    if (!e->gate && held_bearing >= 0) {    /* HOLD: the held one, its fit and +- */
        d.bearing_ddeg = ddeg(held_bearing);
        d.mirror_ddeg = ddeg(held_mirror);
        d.uncert_ddeg = clamp16(held_uncert * 10);
        d.fit_pct = (uint8_t)lrint(fmin(1, fmax(0, held_fit)) * 100);
    } else {
        d.bearing_ddeg = ddeg(last_bearing);
        d.mirror_ddeg = ddeg(last_mirror);
        d.uncert_ddeg = clamp16(last_uncert * 10);
    }
    d.level_ddb = clamp16(e->level_db * 10);
    d.snr_ddb = clamp16(e->doa.snr_db * 10);
    d.eig_ddb = clamp16(e->doa.eig_db * 10);
    d.papr_ddb = clamp16(e->doa.papr_db * 10);
    d.age_ds = last_good_at < 0 ? 0xffff : (uint16_t)fmin(65534, (t - last_good_at) * 10);
    d.npeaks = (uint8_t)(e->doa.npeaks > 3 ? 3 : e->doa.npeaks);
    for (int i = 0; i < d.npeaks; i++) d.peaks_ddeg[i] = ddeg(e->doa.peaks[i]);
    for (int m = 0; m < nch && m < DF_PROTO_MAXCH; m++) {
        d.ch_ddb[m] = clamp16(e->ch_db[m] * 10);
        d.ch_clip_pm[m] = (uint8_t)fmin(255, e->clip[m] * 1000);
    }
    for (int g = 0; g < 360 && e->have_doa; g++) {     /* none: all 0 (too few antennas left) */
        double p = e->doa.p_db[g * DF_GRID / 360];

        d.p[g] = (uint8_t)fmax(0, fmin(255, 255 * (1 + p / 40)));
    }
    send_msg(&d, sizeof(d));
}

static void send_spectrum(uint32_t seq) {
    df_sp_msg_t s;

    memset(&s, 0, sizeof(s));
    memcpy(s.tag, "DFSP", 4);
    s.seq = seq;
    s.fc_hz = fc;
    s.rate = rate;
    s.floor_ddbfs = clamp16(eng.floor_db * 10);
    s.n = DF_PROTO_SPEC;
    for (int i = 0; i < DF_PROTO_SPEC; i++) {
        double v = fmax(eng.spec_db[2 * i], eng.spec_db[2 * i + 1]);

        s.db[i] = (uint8_t)fmax(0, fmin(255, (v - eng.floor_db + 20) * 2));
    }
    send_msg(&s, sizeof(s));
}

/* ------------------------------------------------------------------------------------------------- commands */

static bool parse_array(const char *a, df_array_t *out) {
    char kind[16];
    int  n, mm, used;

    if (sscanf(a, "%15s %d%n", kind, &n, &used) < 2 || n < 2 || n > DF_MAXCH) return false;
    a += used;
    if (!strcmp(kind, "uca") || !strcmp(kind, "ula")) {
        if (sscanf(a, "%d", &mm) != 1 || mm < 10 || mm > 20000) return false;
        if (kind[2] == 'a' && kind[1] == 'c') df_array_uca(out, n, mm / 1000.0);
        else df_array_ula(out, n, mm / 1000.0);
        return true;
    }
    if (!strcmp(kind, "custom")) {
        memset(out, 0, sizeof(*out));
        out->kind = DF_CUSTOM;
        out->n = n;
        for (int m = 0; m < n; m++) {
            int x, y;

            if (sscanf(a, "%d %d%n", &x, &y, &used) != 2) return false;
            a += used;
            out->x[m] = x / 1000.0;
            out->y[m] = y / 1000.0;
        }
        return true;
    }
    return false;
}

/* what the client asked: 0 nothing more, 1 calibrate (delays known), 2 open again (the rate), 3 quit */
static int apply(const char *c) {
    double     a = 0, b = 0;
    df_array_t na;

    switch (c[0]) {
        case 'f':
            if (sscanf(c + 1, "%lf", &a) == 1 && a >= 24e6 && a <= 1766e6 && (uint32_t)a != fc) {
                fc = (uint32_t)a;
                if (dev_is_open) dev_tune();
                eng.fc = fc;
                if (eng_on) df_engine_retarget(&eng, f_off);
                return 1;
            }
            break;
        case 't':
            if (sscanf(c + 1, "%lf %lf", &a, &b) == 2 && fabs(a) < rate / 2.0 && b >= 1000 && b <= rate / 4.0) {
                bool newbw = (uint32_t)b != bw;

                f_off = (int32_t)a;
                bw = (uint32_t)b;
                if (newbw && dev_is_open) engine_setup();
                else if (eng_on) df_engine_retarget(&eng, f_off);
            }
            break;
        case 's':
            if (sscanf(c + 1, "%lf", &a) == 1 && (a == 250000 || a == 1024000 || a == 2048000 || a == 2400000) &&
                (uint32_t)a != rate) {
                rate = (uint32_t)a;
                if (fabs((double)f_off) > rate * 0.4) f_off = 0;
                return 2;
            }
            break;
        case 'g':
            if (sscanf(c + 1, "%lf", &a) == 1 && a >= 0 && a <= 500 && (int)a != gain) {
                gain = (int)a;
                if (dev_is_open) dev_gain();
                return 1;
            }
            break;
        case 'a':
            if (parse_array(c + 1, &na)) {
                arr = na;
                if (dev_is_open) engine_setup();
            }
            break;
        case 'm':
            if (sscanf(c + 1, "%lf", &a) == 1 && a >= 0 && a <= 3) eng.method = (df_method_t)(method = (int)a);
            break;
        case 'q':
            if (sscanf(c + 1, "%lf", &a) == 1) eng.sql_db = (sql = (int)a) / 10.0;
            break;
        case 'i':
            if (sscanf(c + 1, "%lf", &a) == 1 && a >= 0) eng.cal_every_s = (cal_every = (int)a) > 0 ? cal_every : -1;
            break;
        case 'x':
            if (sscanf(c + 1, "%lf", &a) == 1) eng.user_off = user_off = (unsigned)a & 0xff;
            break;
        case 'c': return 1;
        case 'Q': return 3;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------- main loop */

static void on_term(int s) {
    (void)s;
    atomic_store(&quit_now, true);
}

static void print_est(const df_est_t *e) {
    printf("%s lvl %5.1f  %s", st == DF_ST_TRACK ? "TRACK " : st == DF_ST_HOLD ? "HOLD  " : st == DF_ST_SLIP ? "SLIP  "
                               : st == DF_ST_LISTEN ? "LISTEN" : st == DF_ST_NOCAL ? "NOCAL " : "?     ",
           e->level_db, e->gate ? "" : "(");
    printf("brg %5.1f +-%4.1f fit %3.0f%% eig %4.1f", e->doa.bearing, e->uncert_deg, e->doa.fit * 100, e->doa.eig_db);
    printf("%s  ch", e->gate ? " " : ")");
    for (int m = 0; m < nch; m++) printf(" %c%4.1f", (e->used >> m) & 1 ? ' ' : '-', e->ch_db[m]);
    if (e->bad_ch >= 0) printf("  slip? CH%d", e->bad_ch);
    printf("\n");
    fflush(stdout);
}

/* Commands while nothing is open: only the settings taken, for the next open */
static void take_cmds_closed(void) {
    pthread_mutex_lock(&cmd_lock);
    for (int i = 0; i < ncmds; i++) (void)apply(cmds[i]);
    ncmds = 0;
    pthread_mutex_unlock(&cmd_lock);
}

/* Started by page I: its settings (replayed as soon as it connects) before the sticks open, so they open and calibrate
 * once, on its band, not first on the defaults; 4 s at most */
static void wait_settings(void) {
    for (int i = 0; i < 40 && !atomic_load(&quit_now); i++) {
        pthread_mutex_lock(&cmd_lock);
        bool settled = had_client && now_s() - cmd_t > 0.4;

        pthread_mutex_unlock(&cmd_lock);
        if (settled) break;
        usleep(100000);
    }
    take_cmds_closed();
}

static void run(bool print) {
    uint32_t seq = 0;
    double   last_st = 0, cpu_t = now_s(), cpu_wall = 0;
    struct timespec cpu0;

    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu0);
    while (!atomic_load(&quit_now)) {
        /* find it, open it */
        st = DF_ST_NODEV;
        why[0] = 0;
        while (!atomic_load(&quit_now) && !dev_there()) {
            take_cmds_closed();
            send_state();
            sleep(2);
        }
        if (on_its_own) take_cmds_closed();
        else wait_settings();
        if (atomic_load(&quit_now)) break;
        st = DF_ST_OPENING;
        send_state();
        have_cal = false;
        cal_at = -1;
        why[0] = note[0] = 0;
        if (dev_open()) {
            st = DF_ST_ERROR;
            logf_("%s", why);
            send_state();
            sleep(3);
            continue;
        }
        engine_setup();
        usleep(800000);                         /* the transfers' times for the first guess at the delays */
        {
            char b[160];
            int  n = 0;

            for (int m = 0; m < nch; m++) n += snprintf(b + n, sizeof(b) - (size_t)n, " %lld", (long long)wr_of(m));
            logf_("samples in so far:%s", b);
        }
        /* The X6200's USB host (MUSB: five receive endpoints, the Kraken's hub takes one) feeds four RTL streams at
         * most, B's included: a fifth one gets nothing at all (2026-11-17, any rate, whichever opened last). Those
         * that sent nothing are let go and shown GONE; the bearing comes from the rest (CH0, the noise source's,
         * opens first so it is never the one) */
        if (!synth) {
            int first0 = nch;

            for (int m = 0; m < nch; m++)
                if (wr_of(m) == 0 && first0 == nch) first0 = m;
            if (first0 < nch && first0 >= 2) {
                hw_drop_from(first0);
                snprintf(note, sizeof(note), "CH%d-%d sent nothing: the radio's USB feeds 4 RTLs, B's too",
                         first0, n_open - 1);
                logf_("%s", note);
                engine_setup();
            }
        }
        coarse_align();
        calibrate(true);

        double cal_try = now_s();

        bool reopen = false;

        while (!atomic_load(&quit_now) && !reopen) {
            /* commands */
            int todo = 0;

            pthread_mutex_lock(&cmd_lock);
            for (int i = 0; i < ncmds; i++) {
                int r = apply(cmds[i]);

                if (r > todo) todo = r;
            }
            ncmds = 0;
            pthread_mutex_unlock(&cmd_lock);
            if (todo == 3) {
                atomic_store(&quit_now, true);
                break;
            }
            if (todo == 2) {
                reopen = true;
                break;
            }
            if (todo == 1 || (!have_cal && now_s() - cal_try > 10)) {
                if (!have_cal) coarse_align();
                calibrate(!have_cal);
                cal_try = now_s();
                send_state();
            }

            /* gone? */
            for (int m = 0; m < nch; m++)
                if (atomic_load(&ch[m].ended) || now_s() - atomic_load(&ch[m].last_t) > 3) {
                    snprintf(why, sizeof(why), "CH%d (serial %.6s) stopped sending", m, ch[m].serial);
                    reopen = true;
                }
            if (reopen) break;

            /* a block */
            if (!wait_for(J, blk_n, 0.5)) continue;
            int64_t behind = wr_of(0) - J;

            if (behind > RING / 2) {            /* fell behind: start again at the newest */
                overruns++;
                J = wr_of(0) - blk_n;
                for (int m = 1; m < nch; m++)
                    if (J + kb[m] + blk_n > wr_of(m)) J = wr_of(m) - kb[m] - blk_n;
            }
            if (!read_at(J, blk_n, blk)) {
                J = wr_of(0) - blk_n;
                continue;
            }
            J += blk_n;
            if (!eng_on) continue;

            df_est_t e;

            df_engine_block(&eng, (const uint8_t *const *)blk, blk_n, &e);
            last = e;
            double t = now_s();

            if (e.cal_wanted == 1) {
                slips++;
                slip_shown = true;
            }
            st = !have_cal ? DF_ST_NOCAL
               : slip_shown ? DF_ST_SLIP
               : e.gate ? DF_ST_TRACK
               : last_good_at >= 0 && t - last_good_at < HOLD_S ? DF_ST_HOLD
               : DF_ST_LISTEN;
            send_estimate(&e, seq);
            send_spectrum(seq);
            seq++;
            if (print) print_est(&e);
            if (e.cal_wanted) calibrate(e.cal_wanted == 1);

            /* the state twice a second, the CPU once a second */
            if (t - last_st >= 0.5) {
                last_st = t;
                send_state();
            }
            if (t - cpu_t >= 1) {
                struct timespec c1;

                clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
                cpu_wall = (c1.tv_sec - cpu0.tv_sec) + (c1.tv_nsec - cpu0.tv_nsec) * 1e-9;
                cpu_pct = (int)lrint(100 * cpu_wall / (t - cpu_t));
                cpu0 = c1;
                cpu_t = t;
            }
        }
        dev_close();
        if (reopen && !atomic_load(&quit_now)) {
            logf_("open again: %s", why[0] ? why : "a new rate");
            st = DF_ST_ERROR;
            send_state();
            sleep(1);
        }
    }
    dev_close();
    st = DF_ST_NODEV;
    send_state();
    logf_("ended");
}

int main(int argc, char **argv) {
    const char *sock_name = DF_SOCK;
    bool        front = false, print = false;
    int         o, an = 5;
    double      a = 0, b = 0;

    df_array_uca(&arr, 5, 0.294);               /* the calculator at 433.92 MHz, s 0.5 */
    while ((o = getopt(argc, argv, "FS:pf:s:g:t:a:m:i:n:DT:")) != -1) {
        switch (o) {
            case 'F': front = true; break;
            case 'S': sock_name = optarg; break;
            case 'p': print = true; break;
            case 'f': fc = (uint32_t)atof(optarg); break;
            case 's': rate = (uint32_t)atof(optarg); break;
            case 'g': gain = atoi(optarg); break;
            case 't':
                if (sscanf(optarg, "%lf,%lf", &a, &b) >= 1) f_off = (int32_t)a;
                if (b > 0) bw = (uint32_t)b;
                break;
            case 'a': {
                char k[8];
                int  mm;

                if (sscanf(optarg, "%7[a-z],%d,%d", k, &an, &mm) != 3 || an < 2 || an > DF_MAXCH) return 1;
                if (!strcmp(k, "ula")) df_array_ula(&arr, an, mm / 1000.0);
                else df_array_uca(&arr, an, mm / 1000.0);
                break;
            }
            case 'm': method = atoi(optarg) & 3; break;
            case 'i': cal_every = atoi(optarg); break;
            case 'n':
                if ((n_open = atoi(optarg)) < 2 || n_open > NCH_KRAKEN) return 1;
                break;
            case 'D': dither_on = true; break;
            case 'T':
                synth = true;
                sscanf(optarg, "%lf,%lf,%d", &synth_bearing, &synth_rate, &synth_dead);
                break;
            default: return 1;
        }
    }

    /* the name first: a second copy stops here (Linux: an abstract name; elsewhere no socket, -p only) */
#ifdef __linux__
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    socklen_t          alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(sock_name));

    if (strlen(sock_name) > sizeof(sa.sun_path) - 2) return 1;
    memcpy(sa.sun_path + 1, sock_name, strlen(sock_name));
    ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ls < 0) return 1;
    if (bind(ls, (struct sockaddr *)&sa, alen) != 0) {
        printf("DF RUNNING\n");
        return 0;
    }
    listen(ls, 2);
#else
    (void)sock_name;
    front = true;
#endif
    on_its_own = front;
    if (!front) {
        pid_t p = fork();

        if (p < 0) return 1;
        if (p > 0) _exit(0);
        setsid();
        if ((p = fork()) < 0) return 1;
        if (p > 0) _exit(0);
        if (chdir("/") != 0) return 1;
        int nul = open("/dev/null", O_RDWR), lg = open("/tmp/x6x00-df.log", O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
        if (lg >= 0) dup2(lg, 2);
    }
    setvbuf(stderr, NULL, _IOLBF, 0);

    struct sigaction s = {.sa_handler = on_term};

    sigemptyset(&s.sa_mask);
    sigaction(SIGTERM, &s, NULL);
    sigaction(SIGINT, &s, NULL);
    signal(SIGPIPE, SIG_IGN);
    for (int m = 0; m < DF_MAXCH; m++)
        if (!(ch[m].ring = malloc(2 * (size_t)RING))) return 1;

    pthread_t th;
    bool      th_on = ls >= 0 && pthread_create(&th, NULL, sock_thread, NULL) == 0;

    logf_("started%s: %u Hz, target %+d Hz / %u Hz, %u S/s", synth ? " (synthetic)" : "", fc, f_off, bw, rate);
    run(print);
    if (th_on) pthread_join(th, NULL);
    return 0;
}
