/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An SDR's spectrum (rtl_spectrum.h): an RTL-SDR stick, a HackRF or a bladeRF, "the stick" below. It is
 *  the daemon's, /usr/bin/x6x00-rtlspec (tools/rtlspec/rtlspec.c): it opens it once and keeps it streaming for as long
 *  as the radio runs, and transmits through a HackRF or a bladeRF when asked (rtl_spectrum_tx). A thread of its
 *  own here connects to it over its socket (starting it when none answers), asks for rows while they are wanted and
 *  takes them in, with the audio and the alerts. The daemon is nobody's child: a GUI started again (a hot install,
 *  a crash) connects to the same daemon and the same open stick. Nothing here closes the stick, ends the daemon or
 *  starts it over, but for a daemon of another protocol version (an update since it started), told to end so this
 *  GUI's own is started. AIRSPACE's screen open: no rows asked for, and its dump1090 is given the stick's samples by
 *  the daemon (rtl_spectrum_feed).
 */

#define _GNU_SOURCE
#include "rtl_spectrum.h"

#include "adsb_rx.h"
#include "kth.h"
#include "audio.h"
#include "rtl_watch.h"
#include "sdr_pool.h"
#include "scheduler.h"
#include "cfg/cfg_api.h"
#include "radio.h"

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER    "/usr/bin/x6x00-rtlspec"
#define SOCK_NAME "x6x00-rtlspec"       /* the daemon's socket, in the abstract namespace */
#define PROTO     2                     /* its protocol: "RTLSPEC 2 ..." */
#define LOG       "/tmp/rtlspec.log"
#define MAX_BINS  8192
#define BINS      2048
#define ROWS      20            /* a second */
#define RATE        2116800     /* 48 x 44.1 kHz: the daemon's audio comes out at the codec's rate */

/* Each device as wide as the radio can take it (the user, 2026-11-16: "ensure the spectrogram auto adjusts to max size
 * for any of the connected devices based on their bandwidth"): a whole number of 264.6 kHz (the daemon's audio chain),
 * under what the HOST port carries (about 5.8 MB/s, measured: 2.9 MS/s at the 2 bytes a sample all three send) and what
 * the device keeps up with. An RTL-SDR 9 x 264.6 kHz (2.38 MS/s: past 2.4 it drops samples), a HackRF or a bladeRF 10 x
 * (2.65 MS/s, 5.3 MB/s); B's picture is 5/6 of it (app_sentry.c rtl_view_hz): 2.0 and 2.2 MHz against 1.8 */
static uint32_t rate_for(rtl_dev_t d) {
    return d == RTL_DEV_RTL ? 2381400u : d == RTL_DEV_HACKRF || d == RTL_DEV_BLADERF ? 2646000u : RATE;
}
static uint32_t asked_rate;             /* the rate said to the daemon last */
#define RING        (44100 * 2) /* audio waiting to play: two seconds at most */
#define CHUNK       1024        /* audio_play() needs room for the whole write: a bigger one may never find it */
#define ST_FILL     17640       /* the stick's audio buffered, 400 ms: its 50 ms messages come in clumps (and BOTH's
                                 * X6200 100 ms fragments take it in bursts); at 150 ms it ran dry, a gap each time
                                 * (up to 130 ms, two of three 10 s tone tests, 2026-11-15); at 300 ms its lowest in
                                 * each 10 s came down to 7-31 ms for minutes after a GUI start, while the stick's
                                 * audio caught up (2026-11-15) */
#define ST_SLACK    4410        /* and let drift 100 ms either way before a sample is dropped or said twice */

static pthread_t       thread;
static bool            started;
static atomic_bool     want;
static atomic_bool     yielded;          /* AIRSPACE's screen open: its dump1090 has the stick's samples */
static atomic_bool     lent_df;          /* page I has the KrakenSDR B was on: the daemon ended and not started */

/* Wanted and not yielded: rows asked for */
static bool kept(void) {
    return atomic_load(&want) && !atomic_load(&yielded);
}
static atomic_int      state = RTL_SPEC_OFF;
static atomic_llong    want_hz = 162475000;
static atomic_int      want_gain = 280;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;       /* the row, the tuner, cmd_fd */
static float           row[MAX_BINS];
static uint16_t        row_n;
static uint32_t        row_rate, row_seq;
static int64_t         row_centre;
static bool            row_fresh;
static char            tuner[16];
static int             cmd_fd = -1;

/* The device, as the daemon's hello says it (rtlspec.c): its kind, what it tunes, whether it transmits */
static atomic_int      dev_kind = RTL_DEV_NONE;
static atomic_int      use_kind = RTL_DEV_NONE;     /* rtl_spectrum_use's */
static atomic_int      use_rtl;                     /* and the RTL's place (RTL<n>) when it is an RTL */
static char            use_sn[24], use_loc[24];     /* and that RTL's serial and USB location as the pool had them when
                                                     * it was asked for: the daemon opens that stick or none, so a
                                                     * place that shifts (a stick gone in a port reset) moves nothing */
static pthread_mutex_t use_lock = PTHREAD_MUTEX_INITIALIZER;

/* The serial of the RTL at place n as the pool sees it now, "" none */
static void sn_at(int n, char *sn, size_t len) {
    sdr_unit_t u[16];
    int        k = sdr_pool_units(u, 16);

    sn[0] = 0;
    for (int i = 0; i < k; i++)
        if (u[i].kind == RTL_DEV_RTL && u[i].rtl == n) snprintf(sn, len, "%s", u[i].sn);
}

/* "k RTL<n> SERIAL LOC" (or "k RTL<n>" with no serial known) */
static void k_rtl_cmd(char *cmd, size_t n) {
    pthread_mutex_lock(&use_lock);
    if (use_sn[0]) snprintf(cmd, n, "k RTL%d %s %s\n", atomic_load(&use_rtl), use_sn, use_loc[0] ? use_loc : "-");
    else snprintf(cmd, n, "k RTL%d\n", atomic_load(&use_rtl));
    pthread_mutex_unlock(&use_lock);
}
static atomic_int      dev_rtl  = -1;               /* the open RTL's place, the daemon's word (-1 none) */
static atomic_llong    dev_lo = RTL_SPEC_MIN_HZ, dev_hi = RTL_SPEC_MAX_HZ;
static atomic_bool     dev_tx;

/* Transmitting (rtl_spectrum_tx): what was asked, the daemon's word on it, the hand mic's level */
static atomic_int      tx_state = RTL_TX_OFF;
static atomic_bool     tx_mic;          /* the mic thread sends the voice: the base in record mode */
static atomic_int      tx_mic_cdb = -9900;     /* hundredths of dBFS */
static char            tx_said[48];
static uint32_t        tx_said_n;
static pthread_t       tx_thr;
static bool            tx_thr_on;

/* SAME alerts from the watched channel (NOAA Weather Radio): the last one, also kept in ALERT_LOG */
#define ALERT_LOG "/mnt/wx_alerts.log"
static atomic_llong    watch_hz;
static atomic_llong    rds_hz;          /* the channel decoded for RDS, 0 none */
static rtl_rds_t       rds_now;         /* what came for it, under lock */
static atomic_bool     acars_on;        /* "c 1" asked */
static rtl_acars_t     acars_ring[RTL_ACARS_KEEP];      /* under lock: newest at acars_n - 1 (mod) */
static uint32_t        acars_n;         /* messages taken so far: the newest is acars_ring[(acars_n - 1) % KEEP] */
static char            alert[300];
static time_t          alert_at;
static uint32_t        alert_seq;

/* Listening: the helper's audio, played on the speaker in the recorder's way (the base in play mode) */
static atomic_llong    listen_hz;
static rtl_dmr_t       dmr_now;         /* the listened channel's DMR (RDMR), under lock */
static rtl_dsd_t       dsd_now;         /* its other digital voice (RDSD), under lock */
static atomic_llong    met_hz;          /* B's channel's Meteor satellite ("m"), 0 none */
static rtl_lrpt_t      lrpt_now;        /* the LRPT decoder's state (RLPT), under lock */
#define LP_LINE 784                     /* a half-size line of the satellite's pictures */
#define LP_KEEP 128                     /* strips kept of each channel: 512 half-size lines, more than B shows */
static uint8_t        *lp_px[6];        /* each channel's last strips (4 lines of 784), under lock */
static int32_t         lp_strip[6][LP_KEEP];    /* the strip in each place, -1 none */
static int32_t         lp_top = -1;     /* the newest strip of any channel */
static unsigned        lp_pass;
static uint32_t        lp_ver;
static atomic_int      listen_mode;      /* 0 not listening */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t         ring[RING];
static int             ring_head, ring_len;
static pthread_t       player;
static bool            player_started;
static atomic_bool     playing;          /* the player has the speaker */
static atomic_bool     mux;              /* BOTH: the X6200's receiver mixed in (audio_rx_tap), on the headphones */
static atomic_bool     on_bt;            /* the player's audio on the Bluetooth headphones now, not the speaker */
static atomic_bool     on_moni;          /* BOTH's monitor test now: the base in normal mode, the stick to its monitor */
static atomic_bool     mute;             /* OFF: the speaker taken and given nothing, neither radio heard */
static atomic_bool     player_moni;      /* the player's monitor test on: the base left in normal mode */
static atomic_int      g_stick = 1000, g_x62 = 1000;   /* each receiver's share in the mix, thousandths (BOTH) */
/* BOTH's test (on while /tmp/both_tones exists, looked for each second): a steady tone on each receiver's line, as
 * close to where its audio comes in as can be, to hear the mix: 700 Hz in place of the stick's audio as the daemon
 * sends it (through the buffer and its timing, as a station would be), 1300 Hz added to what the base sends of the
 * X6200's; the stick's squelch held open */
#define TONE_STICK_HZ 700
#define TONE_X62_HZ   1300
#define TONE_STICK_A  8000
#define TONE_X62_A    3000
static atomic_bool     tones;
static uint32_t lv_tick_or_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static double          tone_ph_stick, tone_ph_x62;

/* The squelch while listening: the channel's strongest bin over the row's noise floor (its median), in dB, at least
 * sql_db to open (0: always open); it closes SQL_HANG rows after falling 3 dB under that, so a voice's pauses do not
 * chop it */
#define SQL_HANG    10          /* rows: half a second */
static atomic_int      sql_db;
static atomic_bool     sql_open = true;
static atomic_int      chan_over = -10;  /* the listened channel over the noise, tenths of dB; -10 none */

/* The stick's audio for a picture of it (the Sentry's G WALKIE voiceprint on a stick channel): its own ring */
#define RSCOPE 8192
static int16_t         rscope[RSCOPE];
static int             rscope_head, rscope_len;
static atomic_bool     rscope_on;
static pthread_mutex_t rscope_lock = PTHREAD_MUTEX_INITIALIZER;

/* A command to the daemon, one message: never waiting for it, never a SIGPIPE */
static void say(const char *cmd) {
    pthread_mutex_lock(&lock);
    if (cmd_fd >= 0) (void)send(cmd_fd, cmd, strlen(cmd), MSG_DONTWAIT | MSG_NOSIGNAL);
    pthread_mutex_unlock(&lock);
}

static int daemon_connect(void) {
    struct sockaddr_un a   = {.sun_family = AF_UNIX};
    socklen_t          len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCK_NAME));
    int                fd  = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (fd < 0) return -1;
    memcpy(a.sun_path + 1, SOCK_NAME, strlen(SOCK_NAME));
    if (connect(fd, (struct sockaddr *)&a, len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The daemon on the other end of fd runs another helper than the one on disk now (replaced since it started: an
 * update, a hot install). Its binary is compared with the file, so an update that brings the same helper leaves the
 * daemon, and the stick, alone */
static bool daemon_outdated(int fd) {
    struct ucred cr;
    socklen_t    len = sizeof(cr);
    char         exe[32], x[4096], y[4096];
    FILE        *a, *b;
    bool         differ = false;

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0 || cr.pid <= 0) return false;
    snprintf(exe, sizeof(exe), "/proc/%d/exe", (int)cr.pid);
    a = fopen(exe, "rb");
    b = fopen(HELPER, "rb");
    if (a && b) {
        size_t n, m;

        do {
            n = fread(x, 1, sizeof(x), a);
            m = fread(y, 1, sizeof(y), b);
            differ = n != m || memcmp(x, y, n) != 0;
        } while (!differ && n > 0);
    }
    if (a) fclose(a);
    if (b) fclose(b);
    return differ;
}

/* The daemon started, on what is wanted now: it goes into the background at once (the child waited for here is gone
 * in a moment), nobody's child, so no GUI's end is its end. One already running: this one ends at once */
static void daemon_start(void) {
    char  f[24], g[16], n[16], r[16], s[16], d[16], k[64] = "";
    pid_t p;

    snprintf(f, sizeof(f), "%lld", (long long)atomic_load(&want_hz));
    snprintf(g, sizeof(g), "%d", atomic_load(&want_gain));
    snprintf(n, sizeof(n), "%d", BINS);
    snprintf(r, sizeof(r), "%d", ROWS);
    snprintf(s, sizeof(s), "%d", RATE);
    snprintf(d, sizeof(d), "%d", atomic_load(&use_rtl));
    if (atomic_load(&use_kind) == RTL_DEV_RTL) {    /* B's stick from the start, by its serial: not RTL0 meanwhile */
        pthread_mutex_lock(&use_lock);
        if (use_sn[0]) snprintf(k, sizeof(k), "%s@%s", use_sn, use_loc);
        pthread_mutex_unlock(&use_lock);
    }
    if ((p = fork()) < 0) return;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR), log = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
        if (log >= 0) dup2(log, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (k[0]) execl(HELPER, HELPER, "-f", f, "-g", g, "-n", n, "-r", r, "-s", s, "-d", d, "-K", k, (char *)NULL);
        else execl(HELPER, HELPER, "-f", f, "-g", g, "-n", n, "-r", r, "-s", s, (char *)NULL);
        _exit(127);
    }
    waitpid(p, NULL, 0);
}

/* A row in: the squelch decided on it (the reader's thread) */
static void squelch_row(const float *db, uint16_t n, uint32_t rate, int64_t centre) {
    static float sorted[MAX_BINS];
    static int   quiet;
    int          sql = atomic_load(&sql_db), mode = atomic_load(&listen_mode);

    if (!mode || !n || !rate) {
        atomic_store(&sql_open, true);
        atomic_store(&chan_over, -10);
        quiet = 0;
        return;
    }
    int64_t hz    = atomic_load(&listen_hz), low = centre - (int64_t)rate / 2;
    int64_t half  = mode == 3 ? 90000 : mode == 2 ? 4000 : 6000;      /* the channel: WFM, AM, NFM */
    double  per   = (double)rate / n;
    int     lo    = (int)((hz - half - low) / per), hi = (int)((hz + half - low) / per);

    if (hi < 0 || lo >= n) {            /* not in the picture (a retune on its way): shut */
        atomic_store(&sql_open, false);
        return;
    }
    if (lo < 0) lo = 0;
    if (hi >= n) hi = n - 1;
    float peak = -1e9f;
    for (int k = lo; k <= hi; k++)
        if (db[k] > peak) peak = db[k];
    memcpy(sorted, db, (size_t)n * sizeof(float));
    float over = peak - float_kth(sorted, n, n / 2);     /* over the row's median: its noise */
    bool  open = atomic_load(&sql_open);

    atomic_store(&chan_over, (int)(over * 10));
    if (!sql) {                         /* no squelch: always open, the level measured all the same */
        atomic_store(&sql_open, true);
        quiet = 0;
        return;
    }

    if (over >= sql) {
        open  = true;
        quiet = 0;
    } else if (over >= sql - 3) {
        quiet = 0;                      /* held open: a little under, not gone */
    } else if (open && ++quiet >= SQL_HANG) {
        open = false;
    }
    atomic_store(&sql_open, open);
}

/* The daemon's word for the stick's state, as this side says it */
static rtl_spec_state_t daemon_state(const char *s) {
    if (!strcmp(s, "NOSTICK")) return RTL_SPEC_NO_STICK;
    if (!strcmp(s, "BUSY")) return RTL_SPEC_BUSY;
    if (!strcmp(s, "STALLED")) return RTL_SPEC_STALLED;
    if (!strcmp(s, "ADSB")) return RTL_SPEC_OFF;
    if (!strcmp(s, "TX")) return RTL_SPEC_TX;
    if (!strcmp(s, "NEEDS")) return RTL_SPEC_NEEDS;
    return RTL_SPEC_OPENING;            /* OPENING; ON: ON once its rows come */
}

/* A SAME header decided: kept, and in the log */
static void take_alert(const char *t) {
    pthread_mutex_lock(&lock);
    snprintf(alert, sizeof(alert), "%s", t);
    alert_at = time(NULL);
    alert_seq++;
    pthread_mutex_unlock(&lock);
    FILE *f = fopen(ALERT_LOG, "a");
    if (f) {
        char      when[32];
        struct tm tm;
        time_t    now = time(NULL);

        gmtime_r(&now, &tm);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%SZ", &tm);
        fprintf(f, "%s %.3f MHz %s\n", when, atomic_load(&watch_hz) / 1e6, t);
        fclose(f);
    }
}

static void b_gone_follow(void);
static bool b_want_there(const sdr_unit_t *u, int n);

static void *spec_thread(void *arg) {
    static uint8_t buf[24 + MAX_BINS * 4 + 64];
    int            fd = -1, asked = -1, failed = 0;
    char           last_st[16] = "";    /* the daemon's last word on the stick: it says it again only when it changes */
    (void)arg;

    for (;;) {
        bool k = kept();

        /* AIRSPACE's dump1090 runs only while its screen is open, on the daemon's samples (adsb_rx.c) */
        adsb_rx_lend(!atomic_load(&yielded) || atomic_load(&acars_on));     /* ACARS: the SDR on 130 MHz */
        if (atomic_load(&lent_df)) {    /* B lent to page I (df_rx.c): its daemon told to end (it closes the stick),
                                         * none started, B holding nothing in the pool till it is given back */
            if (fd >= 0) {
                (void)send(fd, "Q\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
                pthread_mutex_lock(&lock);
                cmd_fd = -1;
                pthread_mutex_unlock(&lock);
                close(fd);
                fd = -1;
                last_st[0] = 0;
                atomic_store(&tx_state, RTL_TX_OFF);
            }
            atomic_store(&dev_kind, RTL_DEV_NONE);
            atomic_store(&dev_rtl, -1);
            sdr_pool_b(RTL_DEV_NONE, -1, RTL_DEV_NONE, -1);
            state = RTL_SPEC_BUSY;
            usleep(200000);
            continue;
        }
        if (fd < 0) {
            if ((fd = daemon_connect()) < 0 && access(HELPER, X_OK) == 0) {
                daemon_start();
                for (int i = 0; i < 30 && (fd = daemon_connect()) < 0; i++) usleep(100000);
            }
            if (fd < 0) {               /* a helper that does not start (or not as a daemon): tried less and less */
                state = !k ? RTL_SPEC_OFF : RTL_SPEC_NO_HELPER;
                sleep(failed < 5 ? 2 << failed : 60);
                failed++;
                continue;
            }
            failed = 0;
            if (daemon_outdated(fd)) {  /* the helper updated: the old daemon told to end, the new one started */
                (void)send(fd, "Q\n", 2, MSG_DONTWAIT | MSG_NOSIGNAL);
                close(fd);
                fd = -1;
                sleep(3);
                continue;
            }
            pthread_mutex_lock(&lock);
            cmd_fd = fd;
            pthread_mutex_unlock(&lock);
            asked = -1;

            /* What this GUI wants, said again: a daemon that ran before it is as the last GUI left it */
            char cmd[64];
            asked_rate = rate_for((rtl_dev_t)atomic_load(&dev_kind));
            snprintf(cmd, sizeof(cmd), "s %u\n", asked_rate);
            say(cmd);
            snprintf(cmd, sizeof(cmd), "g %d\n", atomic_load(&want_gain));
            say(cmd);
            snprintf(cmd, sizeof(cmd), "f %lld\n", (long long)atomic_load(&want_hz));
            say(cmd);
            if (atomic_load(&watch_hz)) {
                snprintf(cmd, sizeof(cmd), "w %lld\n", (long long)atomic_load(&watch_hz));
                say(cmd);
            }
            if (atomic_load(&rds_hz)) {
                snprintf(cmd, sizeof(cmd), "d %lld\n", (long long)atomic_load(&rds_hz));
                say(cmd);
            }
            if (atomic_load(&acars_on)) say("c 1\n");
            if (atomic_load(&met_hz)) {
                snprintf(cmd, sizeof(cmd), "m %lld\n", (long long)atomic_load(&met_hz));
                say(cmd);
            }
            say("p\n");                /* the pictures' strips again, if a pass goes on */
            if (atomic_load(&listen_mode)) {
                snprintf(cmd, sizeof(cmd), "l %lld %d\n", (long long)atomic_load(&listen_hz),
                         atomic_load(&listen_mode));
                say(cmd);
            }
            if (atomic_load(&use_kind) != RTL_DEV_NONE) {
                rtl_dev_t u = (rtl_dev_t)atomic_load(&use_kind);

                if (u == RTL_DEV_RTL) k_rtl_cmd(cmd, sizeof(cmd));
                else snprintf(cmd, sizeof(cmd), "k %s\n", u == RTL_DEV_HACKRF ? "HACKRF" : "BLADERF");
                say(cmd);
            }
        }
        if ((int)k != asked) {          /* rows asked for while wanted, and only then */
            say(k ? "r 1\n" : "r 0\n");
            asked = k;
        }
        /* the SDR pool told what B has and asked for (it gives the rest to AIRSPACE and the watch channels) */
        sdr_pool_b(atomic_load(&dev_kind) == RTL_DEV_NONE ? RTL_DEV_NONE : (rtl_dev_t)atomic_load(&dev_kind),
                   atomic_load(&dev_rtl), (rtl_dev_t)atomic_load(&use_kind), atomic_load(&use_rtl));
        {   /* B asked for an RTL the daemon could not open (AIRSPACE's spare had it a moment ago, and lets it go):
             * asked again each 3 s while another RTL is open */
            static uint32_t again;
            struct timespec ts;

            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
            sdr_unit_t pu[16];
            int        pn = sdr_pool_units(pu, 16);

            if (atomic_load(&use_kind) == RTL_DEV_RTL && atomic_load(&dev_kind) == RTL_DEV_RTL &&
                atomic_load(&dev_rtl) >= 0 && atomic_load(&dev_rtl) != atomic_load(&use_rtl) && now - again > 3000 &&
                b_want_there(pu, pn)) {        /* one gone for good: b_gone_follow's, not asked for in a loop */
                char cmd[80], sn[24], want[24];

                again = now;
                sn_at(atomic_load(&dev_rtl), sn, sizeof(sn));
                pthread_mutex_lock(&use_lock);
                snprintf(want, sizeof(want), "%s", use_sn);
                pthread_mutex_unlock(&use_lock);
                if (want[0] && !strcmp(sn, want)) {
                    /* the stick asked for by its serial, at another place now (one before it went or came): its
                     * place taken, so the pool sees B on its real stick; nothing asked again */
                    atomic_store(&use_rtl, atomic_load(&dev_rtl));
                } else {
                    k_rtl_cmd(cmd, sizeof(cmd));
                    say(cmd);
                }
            }
        }
        b_gone_follow();
        if (!k) state = RTL_SPEC_OFF;
        else if (state == RTL_SPEC_OFF && last_st[0]) state = daemon_state(last_st);   /* wanted again (AIRSPACE closed) */

        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 200);
        ssize_t       n;

        if (r == 0 || (r < 0 && errno == EINTR)) continue;
        n = r > 0 ? recv(fd, buf, sizeof(buf) - 1, 0) : -1;
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) {                   /* the daemon went (told to end, or it died): connected again, or started */
            last_st[0] = 0;
            atomic_store(&tx_state, RTL_TX_OFF);    /* its transmission ended with it */
            pthread_mutex_lock(&lock);
            cmd_fd = -1;
            pthread_mutex_unlock(&lock);
            close(fd);
            fd = -1;
            sleep(1);
            continue;
        }

        if (n > 8 && memcmp(buf, "RTLSPEC ", 8) == 0) {
            char               st[16] = "", tn[16] = "", kd[16] = "";
            int                ver = 0, nb = 0, tx = 0, rtl = -1;
            unsigned           rate = 0;
            unsigned long long lo = 0, hi = 0;

            buf[n] = 0;
            int got = sscanf((char *)buf, "RTLSPEC %d %15s %15s %u %d %15s %llu %llu %d %d", &ver, st, tn, &rate,
                             &nb, kd, &lo, &hi, &tx, &rtl);

            if (got >= 1 && ver != PROTO) {
                /* another version of the daemon, running since before an update: told to end (it closes the stick),
                 * and this GUI's own started once it has */
                say("Q\n");
                pthread_mutex_lock(&lock);
                cmd_fd = -1;
                pthread_mutex_unlock(&lock);
                close(fd);
                fd = -1;
                sleep(3);
                continue;
            }
            if (got < 5) continue;
            if (got >= 9) {             /* the device's kind, range and transmitter (a daemon of before says none) */
                atomic_store(&dev_rtl, got >= 10 ? rtl : !strcmp(kd, "RTL") ? 0 : -1);    /* its RTL<n> */
                atomic_store(&dev_kind, !strcmp(kd, "HACKRF") ? RTL_DEV_HACKRF : !strcmp(kd, "BLADERF") ? RTL_DEV_BLADERF
                                        : !strcmp(kd, "RTL") ? RTL_DEV_RTL : RTL_DEV_NONE);
                bool none = !strcmp(kd, "-") || !strcmp(kd, "REPLAY");      /* an RTL-SDR's till one says */

                atomic_store(&dev_lo, none || !lo ? RTL_SPEC_MIN_HZ : (long long)lo);
                atomic_store(&dev_hi, none || !hi ? RTL_SPEC_MAX_HZ : (long long)hi);
                atomic_store(&dev_tx, tx != 0);
                if (!none && rate_for((rtl_dev_t)atomic_load(&dev_kind)) != asked_rate) {  /* its own width */
                    char rc[24];

                    asked_rate = rate_for((rtl_dev_t)atomic_load(&dev_kind));
                    snprintf(rc, sizeof(rc), "s %u\n", asked_rate);
                    say(rc);
                }
                if (tx == 2) atomic_store(&tx_state, RTL_TX_ON);
            } else {
                atomic_store(&dev_kind, strcmp(st, "NOSTICK") ? RTL_DEV_RTL : RTL_DEV_NONE);
                atomic_store(&dev_lo, RTL_SPEC_MIN_HZ);
                atomic_store(&dev_hi, RTL_SPEC_MAX_HZ);
                atomic_store(&dev_tx, false);
            }
            snprintf(last_st, sizeof(last_st), "%s", st);
            pthread_mutex_lock(&lock);
            snprintf(tuner, sizeof(tuner), "%s", strcmp(tn, "-") ? tn : "");
            pthread_mutex_unlock(&lock);
            if (k && !(state == RTL_SPEC_ON && !strcmp(st, "ON"))) state = daemon_state(st);
            continue;
        }
        if (n > 4 && memcmp(buf, "RTX ", 4) == 0) {    /* the transmitter: "RTX ON|END|NO <Hz> <why>" */
            char w[8] = "", why[24] = "";
            unsigned long long hz = 0;

            buf[n] = 0;
            if (sscanf((char *)buf, "RTX %7s %llu %23s", w, &hz, why) >= 2) {
                if (!strcmp(w, "ON")) atomic_store(&tx_state, RTL_TX_ON);
                else atomic_store(&tx_state, RTL_TX_OFF);
                pthread_mutex_lock(&lock);
                snprintf(tx_said, sizeof(tx_said), "%s %s", w, strcmp(why, "-") ? why : "");
                tx_said_n++;
                pthread_mutex_unlock(&lock);
                fprintf(stderr, "sdr transmitter: %s", (char *)buf + 4);
                fprintf(stderr, "\n");
            }
            continue;
        }
        if (n >= 8 && memcmp(buf, "RAUD", 4) == 0) {   /* audio: into the ring, the oldest out when it is full */
            uint32_t cnt;
            int16_t  s;

            memcpy(&cnt, buf + 4, 4);
            if (cnt > (uint32_t)(n - 8) / 2) continue;
            pthread_mutex_lock(&ring_lock);
            for (uint32_t i = 0; i < cnt; i++) {
                memcpy(&s, buf + 8 + i * 2, 2);
                if (atomic_load(&tones)) {          /* the test's tone in place of the stick's audio */
                    s = (int16_t)lrint(TONE_STICK_A * sin(tone_ph_stick));
                    tone_ph_stick = fmod(tone_ph_stick + 2 * M_PI * TONE_STICK_HZ / 44100.0, 2 * M_PI);
                }
                ring[(ring_head + ring_len) % RING] = s;
                if (ring_len < RING) ring_len++;
                else ring_head = (ring_head + 1) % RING;
            }
            pthread_mutex_unlock(&ring_lock);
            continue;
        }
        if (n >= 8 && memcmp(buf, "RSAM", 4) == 0) {
            uint32_t len;
            char     t[300];

            memcpy(&len, buf + 4, 4);
            if (len >= sizeof(t) || len > (uint32_t)(n - 8)) continue;
            memcpy(t, buf + 8, len);
            t[len] = 0;
            take_alert(t);
            continue;
        }
        if (n >= 8 && memcmp(buf, "RDMR", 4) == 0) {   /* "<Hz>|<slot>|<cc>|<voice>|<lc>|<flco>|<src>|<dst>|<bs>|<voc>" */
            uint32_t len;
            char     t[160];
            long long hz;
            int      sl, cc, voice, lc, flco, bs, voc;
            unsigned src, dst;

            memcpy(&len, buf + 4, 4);
            if (len >= sizeof(t) || len > (uint32_t)(n - 8)) continue;
            memcpy(t, buf + 8, len);
            t[len] = 0;
            if (sscanf(t, "%lld|%d|%d|%d|%d|%d|%u|%u|%d|%d", &hz, &sl, &cc, &voice, &lc, &flco, &src, &dst, &bs,
                       &voc) != 10 || sl < 0 || sl > 2)
                continue;
            uint32_t now = lv_tick_or_ms();

            pthread_mutex_lock(&lock);
            if (dmr_now.hz != hz) memset(&dmr_now, 0, sizeof(dmr_now));
            dmr_now.hz      = hz;
            if (cc >= 0) dmr_now.cc = cc;
            else if (!dmr_now.at_ms) dmr_now.cc = -1;
            dmr_now.bs      = bs;
            dmr_now.vocoder = voc;
            dmr_now.slot[sl] = (rtl_dmr_slot_t){.voice = voice, .lc = lc, .flco = flco, .src = src, .dst = dst,
                                                .at_ms = now};
            dmr_now.at_ms = now;
            dmr_now.seq++;
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (n >= 8 && memcmp(buf, "RDSD", 4) == 0) {   /* "<Hz>|<protocol>|<slot>|<call>" */
            uint32_t  len;
            char      t[320], *p1, *p2, *p3;
            long long hz;
            int       sl;

            memcpy(&len, buf + 4, 4);
            if (len >= sizeof(t) || len > (uint32_t)(n - 8)) continue;
            memcpy(t, buf + 8, len);
            t[len] = 0;
            if (!(p1 = strchr(t, '|')) || !(p2 = strchr(p1 + 1, '|')) || !(p3 = strchr(p2 + 1, '|'))) continue;
            *p1 = *p2 = *p3 = 0;
            hz = atoll(t);
            sl = atoi(p2 + 1);
            if (sl < 0 || sl > 1) continue;
            pthread_mutex_lock(&lock);
            if (dsd_now.hz != hz || !*(p1 + 1)) memset(&dsd_now, 0, sizeof(dsd_now));
            dsd_now.hz = hz;
            snprintf(dsd_now.proto, sizeof(dsd_now.proto), "%s", p1 + 1);
            snprintf(dsd_now.call[sl], sizeof(dsd_now.call[sl]), "%s", p3 + 1);
            dsd_now.at_ms = lv_tick_or_ms();
            dsd_now.seq++;
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (n >= 8 && memcmp(buf, "RLPT", 4) == 0) {   /* "<Hz>|<pass>|<mode>|<lock>|<offset>|<quality>|<frames>|
                                                        *  <good>|<packets>|<rows,...>|<file>|<dropped>" */
            uint32_t len;
            char     t[300], *f[12], *p = NULL;
            int      k = 0;

            memcpy(&len, buf + 4, 4);
            if (len >= sizeof(t) || len > (uint32_t)(n - 8)) continue;
            memcpy(t, buf + 8, len);
            t[len] = 0;
            for (p = t; k < 12 && (f[k] = p); k++) {
                if (!(p = strchr(p, '|'))) {
                    k++;
                    break;
                }
                *p++ = 0;
            }
            if (k < 12) continue;
            rtl_lrpt_t m = {.hz = strtoll(f[0], NULL, 10), .pass = (unsigned)strtoul(f[1], NULL, 10),
                            .lock = atoi(f[3]) != 0, .offset = atoi(f[4]), .quality = atoi(f[5]),
                            .frames = (uint32_t)strtoul(f[6], NULL, 10), .good = (uint32_t)strtoul(f[7], NULL, 10),
                            .packets = (uint32_t)strtoul(f[8], NULL, 10), .dropped = (uint32_t)strtoul(f[11], NULL, 10)};
            snprintf(m.mode, sizeof(m.mode), "%s", f[2]);
            snprintf(m.file, sizeof(m.file), "%s", f[10]);
            sscanf(f[9], "%d,%d,%d,%d,%d,%d", &m.rows[0], &m.rows[1], &m.rows[2], &m.rows[3], &m.rows[4], &m.rows[5]);
            pthread_mutex_lock(&lock);
            if (m.pass != lp_pass) {    /* another pass: the strips kept were the last one's */
                lp_pass = m.pass;
                lp_top  = -1;
                for (int c = 0; c < 6; c++)
                    for (int i = 0; i < LP_KEEP; i++) lp_strip[c][i] = -1;
                lp_ver++;
            }
            m.at_ms  = lv_tick_or_ms();
            m.seq    = lrpt_now.seq + 1;
            lrpt_now = m;
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (n >= 12 + 4 * LP_LINE && memcmp(buf, "RLPI", 4) == 0) {    /* u8 channel, u8 0, u16 pass, u32 strip,
                                                                        * 784 x 4 */
            uint16_t pass;
            uint32_t strip;
            int      c = buf[4];

            memcpy(&pass, buf + 6, 2);
            memcpy(&strip, buf + 8, 4);
            if (c < 0 || c >= 6 || strip > 100000) continue;
            pthread_mutex_lock(&lock);
            if (pass != (uint16_t)lp_pass) {    /* a pass's strips before its state (a daemon started anew) */
                lp_pass = pass;
                lp_top  = -1;
                for (int ch = 0; ch < 6; ch++)
                    for (int i = 0; i < LP_KEEP; i++) lp_strip[ch][i] = -1;
            }
            if (!lp_px[c] && (lp_px[c] = malloc((size_t)LP_KEEP * 4 * LP_LINE))) {
                for (int i = 0; i < LP_KEEP; i++) lp_strip[c][i] = -1;
            }
            if (lp_px[c]) {
                memcpy(lp_px[c] + (size_t)(strip % LP_KEEP) * 4 * LP_LINE, buf + 12, 4 * LP_LINE);
                lp_strip[c][strip % LP_KEEP] = (int32_t)strip;
                if ((int32_t)strip > lp_top) lp_top = (int32_t)strip;
                lp_ver++;
            }
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (n >= 8 && memcmp(buf, "RRDS", 4) == 0) {   /* "<Hz>|<PI>|<PTY>|<TP>|<TA>|<PS, 8>|<RT>" */
            uint32_t len;
            char     t[200], *f[5], *p;
            int      k = 0;

            memcpy(&len, buf + 4, 4);
            if (len >= sizeof(t) || len > (uint32_t)(n - 8)) continue;
            memcpy(t, buf + 8, len);
            t[len] = 0;
            for (p = t; k < 5 && (f[k] = p) && (p = strchr(p, '|')); k++) *p++ = 0;
            if (k < 5 || !p) continue;
            rtl_rds_t r = {.hz = strtoll(f[0], NULL, 10), .pi = (uint16_t)strtoul(f[1], NULL, 16),
                           .pty = f[2][0] ? atoi(f[2]) : -1, .tp = atoi(f[3]), .ta = atoi(f[4])};

            if (strlen(p) >= 9 && p[8] == '|') {        /* the name, its 8 characters ('|' among them too) */
                memcpy(r.ps, p, 8);
                p += 9;
            } else if (*p == '|') {
                p++;
            }
            snprintf(r.rt, sizeof(r.rt), "%s", p);
            pthread_mutex_lock(&lock);
            if (r.hz == atomic_load(&rds_hz)) {
                r.seq   = rds_now.seq + 1;
                rds_now = r;
            }
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (n >= 8 && memcmp(buf, "RACR", 4) == 0) {   /* "<Hz>|<mode>|<reg>|<ack>|<label>|<block>|<msn>|<flight>|
                                                        *  <more>|<level>|<text>" */
            uint32_t len;
            char     t[420], *f[10], *p;
            int      k = 0;

            memcpy(&len, buf + 4, 4);
            if (len >= sizeof(t) || len > (uint32_t)(n - 8)) continue;
            memcpy(t, buf + 8, len);
            t[len] = 0;
            for (p = t; k < 10 && (f[k] = p) && (p = strchr(p, '|')); k++) *p++ = 0;
            if (k < 10 || !p) continue;
            rtl_acars_t m = {.hz = strtoll(f[0], NULL, 10), .at = time(NULL), .mode = f[1][0], .ack = f[3][0],
                             .block = f[5][0], .more = atoi(f[8]) != 0, .level = atoi(f[9])};

            snprintf(m.reg, sizeof(m.reg), "%s", f[2]);
            snprintf(m.label, sizeof(m.label), "%s", f[4]);
            snprintf(m.msn, sizeof(m.msn), "%s", f[6]);
            snprintf(m.flight, sizeof(m.flight), "%s", f[7]);
            snprintf(m.text, sizeof(m.text), "%s", p);
            snprintf(m.dev, sizeof(m.dev), "%s", rtl_spectrum_dev_tag());     /* the one the daemon has now */
            pthread_mutex_lock(&lock);
            acars_ring[acars_n % RTL_ACARS_KEEP] = m;
            acars_n++;
            pthread_mutex_unlock(&lock);
            continue;
        }
        if (n >= 24 && memcmp(buf, "RSPC", 4) == 0) {
            uint64_t c;
            uint32_t rt;
            uint16_t nb, flags;

            memcpy(&c, buf + 8, 8);
            memcpy(&rt, buf + 16, 4);
            memcpy(&nb, buf + 20, 2);
            memcpy(&flags, buf + 22, 2);
            if (!nb || nb > MAX_BINS || (size_t)n < 24 + (size_t)nb * 4 || !k) continue;
            pthread_mutex_lock(&lock);
            memcpy(row, buf + 24, (size_t)nb * 4);
            row_n      = nb;
            row_rate   = rt;
            row_centre = (int64_t)c;
            row_seq++;
            row_fresh  = flags & 1;
            pthread_mutex_unlock(&lock);
            squelch_row(row, nb, rt, (int64_t)c);   /* row is only written here: read without the lock */
            state = RTL_SPEC_ON;
        }
    }
    return NULL;
}

/* Within what any SDR tunes: the device's own range is the screens' (rtl_spectrum_min_hz), and the daemon keeps the
 * device within it; a frequency asked before the device said what it is is not cut to an RTL-SDR's */
void rtl_spectrum_want(bool on, int64_t hz, int gain) {
    if (hz < RTL_SPEC_ANY_MIN_HZ) hz = RTL_SPEC_ANY_MIN_HZ;
    if (hz > RTL_SPEC_ANY_MAX_HZ) hz = RTL_SPEC_ANY_MAX_HZ;
    atomic_store(&want_hz, hz);
    atomic_store(&want_gain, gain);
    atomic_store(&want, on);
    if (on && !started) {
        started = pthread_create(&thread, NULL, spec_thread, NULL) == 0;
        if (started) pthread_detach(thread);
    }
}

/* AIRSPACE open: its dump1090 has the daemon's samples, so the daemon is connected now even if no Sentry screen ever
 * wanted rows (a GUI started again straight into AIRSPACE: dump1090 found the stick busy, held by the daemon, and
 * AIRSPACE said OPENING ITS PORT till the Sentry was opened; a HackRF or bladeRF it cannot open at all) */
void rtl_spectrum_lend_df(bool on) {
    atomic_store(&lent_df, on);
}

bool rtl_spectrum_lent_df(void) {
    return atomic_load(&lent_df);
}

void rtl_spectrum_yield(bool on) {
    atomic_store(&yielded, on);
    if (on && !started) {
        started = pthread_create(&thread, NULL, spec_thread, NULL) == 0;
        if (started) pthread_detach(thread);
    }
}

bool rtl_spectrum_daemon(void) {
    pthread_mutex_lock(&lock);
    bool on = cmd_fd >= 0;
    pthread_mutex_unlock(&lock);
    return on;
}

bool rtl_spectrum_feed(int fd) {
    bool ok = false;

    pthread_mutex_lock(&lock);
    if (cmd_fd >= 0 && fd < 0) {
        ok = send(cmd_fd, "a 0\n", 4, MSG_DONTWAIT | MSG_NOSIGNAL) == 4;
    } else if (cmd_fd >= 0) {             /* the pipe's end itself, passed over the socket */
        char line[] = "a\n";
        union {
            struct cmsghdr h;
            char           b[CMSG_SPACE(sizeof(int))];
        } cm;
        struct iovec  io = {.iov_base = line, .iov_len = 2};
        struct msghdr m  = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof(cm.b)};

        memset(&cm, 0, sizeof(cm));
        struct cmsghdr *h = CMSG_FIRSTHDR(&m);
        h->cmsg_level     = SOL_SOCKET;
        h->cmsg_type      = SCM_RIGHTS;
        h->cmsg_len       = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(h), &fd, sizeof(int));
        ok = sendmsg(cmd_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL) == 2;
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

/* B's SDR taken in place of one gone (b_gone_follow), kept as its choice (screen_main reads it at the start) */
static void b_choice_keep(void *arg) {
    param_i_set(cfg_rtl_dev, *(int *)arg);
}

/* B's own SDR in the pool now: the one asked for by its serial (by its place when none was known); true for a kind
 * other than an RTL that is there */
static bool b_want_there(const sdr_unit_t *u, int n) {
    rtl_dev_t k = (rtl_dev_t)atomic_load(&use_kind);
    char      want[24];

    pthread_mutex_lock(&use_lock);
    snprintf(want, sizeof(want), "%s", use_sn);
    pthread_mutex_unlock(&use_lock);
    for (int i = 0; i < n; i++)
        if (u[i].kind == k && (k != RTL_DEV_RTL || (want[0] ? !strcmp(u[i].sn, want) : u[i].rtl == atomic_load(&use_rtl))))
            return true;
    return false;
}

/* B's SDR gone from USB for good (the owner swapped a KrakenSDR for one stick, 2026-11-17). B was pinned to its stick by
 * serial, and two things went wrong: with nothing open, nothing asked again, so B waited on a stick that was not coming
 * back till the GUI restarted; and a GUI starting with a place that no longer exists ("k RTL4" of one stick) had the
 * daemon fall back to RTL0, which the re-ask above took for the wrong stick and asked RTL4 again every 3 s. Here, on
 * every screen: B's own not on USB for 3 s (a replug or a port reset comes back sooner), B takes the one the daemon has
 * open if it may, else another free or B's own, an RTL first, as if chosen. Never one page I, AIRSPACE or a watch
 * channel holds, nor one stopped in the pool's panel; not while B has none on purpose (page I lent it: use_kind NONE) */
static void b_gone_follow(void) {
    static uint32_t gone_ms;
    struct timespec ts;
    sdr_unit_t      u[16];
    rtl_dev_t       k = (rtl_dev_t)atomic_load(&use_kind), dk = (rtl_dev_t)atomic_load(&dev_kind);
    int             n, pick = -1;

    n = sdr_pool_units(u, 16);
    if (k == RTL_DEV_NONE || b_want_there(u, n)) {     /* none wanted, or it is there */
        gone_ms = 0;
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    if (!gone_ms) {
        gone_ms = now | 1;
        return;
    }
    if (now - gone_ms < 3000) return;
    for (int i = 0; i < n; i++) {
        bool free_ = !u[i].stopped && (u[i].owner == SDR_OWN_NONE || u[i].owner == SDR_OWN_B);
        bool open_ = u[i].kind == dk && (dk != RTL_DEV_RTL || u[i].rtl == atomic_load(&dev_rtl));

        if (!free_) continue;
        if (open_) {                    /* the one the daemon has: kept, nothing reopened */
            pick = i;
            break;
        }
        if (pick < 0 || (u[i].kind == RTL_DEV_RTL && u[pick].kind != RTL_DEV_RTL)) pick = i;
    }
    if (pick < 0) return;               /* nothing free: asked again in 3 s */
    gone_ms = 0;
    fprintf(stderr, "rtl: B's SDR gone from USB: %s (%s) in its place\n", u[pick].name, u[pick].sn[0] ? u[pick].sn : "-");
    if (u[pick].kind == RTL_DEV_RTL) rtl_spectrum_use_rtl(u[pick].rtl);
    else rtl_spectrum_use(u[pick].kind);
    int v = (int)u[pick].kind | (u[pick].kind == RTL_DEV_RTL ? u[pick].rtl << 16 : 0);   /* app_sentry.c's cfg_rtl_dev */

    scheduler_put(b_choice_keep, &v, sizeof(v));      /* kept as B's choice, on the GUI thread: the next start opens it */
}

void rtl_spectrum_use(rtl_dev_t d) {
    char cmd[80];

    atomic_store(&use_kind, d);
    if (d == RTL_DEV_RTL) k_rtl_cmd(cmd, sizeof(cmd));
    else snprintf(cmd, sizeof(cmd), "k %s\n", d == RTL_DEV_HACKRF ? "HACKRF" : d == RTL_DEV_BLADERF ? "BLADERF" : "-");
    say(cmd);
}

void rtl_spectrum_use_rtl(int n) {
    sdr_unit_t u[16];
    int        k = sdr_pool_units(u, 16);

    atomic_store(&use_rtl, n < 0 ? 0 : n);
    pthread_mutex_lock(&use_lock);
    use_sn[0] = use_loc[0] = 0;
    for (int i = 0; i < k; i++)         /* that place's stick as the pool sees it now */
        if (u[i].kind == RTL_DEV_RTL && u[i].rtl == (n < 0 ? 0 : n) && u[i].sn[0] && !strchr(u[i].sn, ' ')) {
            snprintf(use_sn, sizeof(use_sn), "%s", u[i].sn);
            snprintf(use_loc, sizeof(use_loc), "%s", u[i].loc);
            break;
        }
    pthread_mutex_unlock(&use_lock);
    rtl_spectrum_use(RTL_DEV_RTL);
}

int rtl_spectrum_used_rtl(void) {
    return atomic_load(&use_rtl);
}

int rtl_spectrum_dev_rtl(void) {
    return rtl_spectrum_dev() == RTL_DEV_RTL ? (atomic_load(&dev_rtl) < 0 ? 0 : atomic_load(&dev_rtl)) : -1;
}

const char *rtl_spectrum_rtl_tag(int n) {
    static const char *const t[] = {"RTL0", "RTL1", "RTL2", "RTL3", "RTL4", "RTL5", "RTL6", "RTL7"};

    return n >= 0 && n < 8 ? t[n] : "RTL";
}

rtl_dev_t rtl_spectrum_used(void) {
    return (rtl_dev_t)atomic_load(&use_kind);
}

void rtl_spectrum_kind_range(rtl_dev_t d, int64_t *lo, int64_t *hi) {
    *lo = d == RTL_DEV_HACKRF ? 1000000LL : d == RTL_DEV_BLADERF ? 70000000LL : RTL_SPEC_MIN_HZ;     /* the bladeRF 2.0's
                                                                                                * AD9361, as the daemon says */
    *hi = d == RTL_DEV_HACKRF || d == RTL_DEV_BLADERF ? 6000000000LL : RTL_SPEC_MAX_HZ;
}

void rtl_spectrum_tune(int64_t hz) {
    char cmd[32];

    if (hz < RTL_SPEC_ANY_MIN_HZ) hz = RTL_SPEC_ANY_MIN_HZ;
    if (hz > RTL_SPEC_ANY_MAX_HZ) hz = RTL_SPEC_ANY_MAX_HZ;
    atomic_store(&want_hz, hz);
    snprintf(cmd, sizeof(cmd), "f %lld\n", (long long)hz);
    say(cmd);
}

void rtl_spectrum_gain(int tenths) {
    char cmd[24];

    atomic_store(&want_gain, tenths);
    snprintf(cmd, sizeof(cmd), "g %d\n", tenths);
    say(cmd);
}

rtl_spec_state_t rtl_spectrum_state(void) {
    return (rtl_spec_state_t)atomic_load(&state);
}

void rtl_spectrum_tuner(char *s, size_t n) {
    pthread_mutex_lock(&lock);
    snprintf(s, n, "%s", tuner);
    pthread_mutex_unlock(&lock);
}

bool rtl_spectrum_frame(float *db, uint16_t cap, uint16_t *n, uint32_t *rate, int64_t *centre, uint32_t *seq,
                        bool *fresh) {
    bool got = false;

    pthread_mutex_lock(&lock);
    if (row_n && row_seq != *seq) {
        uint16_t k = row_n < cap ? row_n : cap;

        memcpy(db, row, (size_t)k * sizeof(float));
        *n      = k;
        *rate   = row_rate;
        *centre = row_centre;
        *seq    = row_seq;
        *fresh  = row_fresh;
        got     = true;
    }
    pthread_mutex_unlock(&lock);
    return got;
}

/* The stick's audio for the player, n samples into out; how many (fewer only when it ran dry: the rest is silence).
 * Its 50 ms messages come in clumps: taken as they came, it ran dry between them and each gap went out as silence
 * (A with BOTH choppy, 2026-11-14; B alone too, six gaps of 8 to 24 ms in 20 s, 2026-11-15). So it fills to ST_FILL
 * first and is taken from then, filled again if it runs dry. Its clock is the stick's, a little off the codec's:
 * past ST_SLACK from ST_FILL a sample is dropped, or said twice, at a call. Said in the log every 10 s when it ran
 * dry or low */
static bool     st_run;
static int      st_dry, st_low = INT32_MAX;
static uint32_t st_said;

static int stick_take(int16_t *out, int n) {
    int m = 0;

    pthread_mutex_lock(&ring_lock);
    if (!st_run && ring_len >= ST_FILL) st_run = true;
    if (st_run && ring_len > ST_FILL * 4) {         /* far behind: caught up */
        ring_head = (ring_head + ring_len - ST_FILL) % RING;
        ring_len  = ST_FILL;
    }
    if (st_run) {
        int take = n;

        if (ring_len > ST_FILL + ST_SLACK) {
            ring_head = (ring_head + 1) % RING;
            ring_len--;
        } else if (ring_len < ST_FILL - ST_SLACK && n > 1) {
            take = n - 1;
        }
        for (; m < take && ring_len > 0; m++) {
            out[m]    = ring[ring_head];
            ring_head = (ring_head + 1) % RING;
            ring_len--;
        }
        if (m && m == take && take < n) out[m] = out[m - 1], m++;
        if (m < n) st_run = false, st_dry++;
        if (ring_len < st_low) st_low = ring_len;
    }
    pthread_mutex_unlock(&ring_lock);
    if (lv_tick_or_ms() - st_said >= 10000) {
        if (st_dry || st_low < 44100 / 20 || atomic_load(&tones))
            fprintf(stderr, "rtl stick buffer: its low %d ms, ran dry %d times in 10 s\n",
                    st_low == INT32_MAX ? -1 : st_low * 10 / 441, st_dry);
        st_said = lv_tick_or_ms(), st_dry = 0, st_low = INT32_MAX;
    }
    return m;
}

/* The audio to the speaker while listening: one thread for good, that never has to be waited for. It takes the
 * speaker when listening starts (the base in play mode, as the recorder plays; or the Bluetooth headphones when they
 * stand in for it) and gives it back when it ends. audio_play() waits for room in CHUNK at most, so the codec paces
 * it; kept under half a second behind, as the stick's clock and the codec's differ a little.
 *
 * BOTH mixes the X6200's receiver in only on the headphones. On the speaker the base is in play mode, and then it
 * sends none of its receiver's audio, only its own copy of what is played (measured 2026-11-15, base 1.0.8: the
 * capture 8 rms with silence played, 2953 with play mode off): mixed in, that copy was BOTH's echo, played again and
 * again, and the canceller that followed it only made it fainter. So on the speaker BOTH plays the stick alone, as
 * RTL does; the Sentry says why (hear_how) */
static void *player_thread(void *arg) {
    static int16_t chunk[CHUNK], rx[CHUNK];
    bool           bt = false, tapping = false, moni = false;
    float          g  = 1;          /* the squelch's level, 0 shut: eased over 10 ms, no click */
    uint32_t       moni_look = 0;
    bool           moni_file = false;
    (void)arg;
    pthread_setname_np(pthread_self(), "rtl_player");   /* by name in top -H */

    for (;;) {
        /* a watch channel open (rtl_watch.c): the speaker taken for it too, the X6200 off it meanwhile (OFF
         * keeps it silent) */
        bool watch     = !atomic_load(&mute) && rtl_watch_active();
        bool want_play = atomic_load(&listen_mode) != 0 || atomic_load(&mute) || watch;

        if (want_play && !atomic_load(&playing)) {
            bt   = audio_play_bt_begin();
            moni = false;
            if (!bt && !atomic_load(&tx_mic)) radio_set_play_mode(AUDIO_PLAY_ON);   /* transmitting: the mic's mode
                                                                                     * stays, put back after it */
            atomic_store(&playing, true);
        } else if (!want_play && atomic_load(&playing)) {
            audio_play_flush();
            if (bt) audio_play_bt_end();
            else if (!moni && !atomic_load(&tx_mic)) radio_set_play_mode(AUDIO_PLAY_OFF);
            bt = moni = false;
            atomic_store(&playing, false);
        }
        /* BOTH's monitor test (while /tmp/both_moni exists, looked for each second; 2026-11-15, for the user's ear):
         * BOTH on the speaker with the base left in normal mode, so it plays its own receiver as ever, and the stick
         * sent to its line in as always. Whether the base puts that on the speaker too, at the monitor's level
         * (/tmp/moni_test, radio.c), only an ear can tell: the processor hears the receiver before the speaker. If it
         * does, this is BOTH on the speaker */
        if (want_play && !bt) {
            if (lv_tick_or_ms() - moni_look >= 1000) {
                moni_look = lv_tick_or_ms();
                moni_file = access("/tmp/both_moni", F_OK) == 0;
            }
            bool want_moni = moni_file && atomic_load(&mux) && atomic_load(&listen_mode);

            if (want_moni != moni && !atomic_load(&tx_mic)) {
                moni = want_moni;
                radio_set_play_mode(moni ? AUDIO_PLAY_OFF : AUDIO_PLAY_ON);
                fprintf(stderr, "rtl BOTH monitor test: %s\n", moni ? "on, the base in normal mode, the stick to its "
                                                                     "line in (its monitor: /tmp/moni_test)" : "off");
            }
        }
        /* the speaker taken back for us: another of the base's play mode's users (the recorder, a voice message,
         * FreeDV) set it OFF as it ended while we still play (only from OFF: a recording's mode is left alone) */
        if (atomic_load(&playing) && !bt && !moni && !atomic_load(&tx_mic) && radio_play_mode() == AUDIO_PLAY_OFF) {
            static uint32_t again;

            if (lv_tick_or_ms() - again >= 500) {
                again = lv_tick_or_ms();
                radio_set_play_mode(AUDIO_PLAY_ON);
            }
        }
        atomic_store(&on_bt, want_play && bt);
        atomic_store(&player_moni, moni);
        atomic_store(&on_moni, want_play && moni);
        if (want_play && (atomic_load(&mux) && bt) != tapping) {   /* BOTH on the headphones: the X6200 mixed in */
            tapping = !tapping;
            audio_rx_tap(tapping);
            st_run = false;             /* filled again for the other way */
        } else if (!want_play && tapping) {
            tapping = false;
            audio_rx_tap(false);
        }
        if (want_play && !atomic_load(&listen_mode) && watch) {   /* the watch channels alone: they pace it */
            memset(chunk, 0, sizeof(chunk));
            rtl_watch_mix(chunk, CHUNK);
            audio_play(chunk, CHUNK);
            continue;
        }
        if (!want_play || !atomic_load(&listen_mode)) {     /* nothing, or OFF: the speaker silent */
            usleep(20000);
            continue;
        }
        if (tapping) {                  /* BOTH: the X6200's samples pace it, the stick's added from its buffer */
            int n = (int)audio_rx_tap_read(rx, CHUNK);

            if (!n) {
                usleep(5000);
                continue;
            }
            static uint32_t tone_look;
            if (lv_tick_or_ms() - tone_look >= 1000) {
                tone_look = lv_tick_or_ms();
                atomic_store(&tones, access("/tmp/both_tones", F_OK) == 0);
            }
            if (atomic_load(&tones)) {          /* the test's tone on the X6200's line */
                for (int k = 0; k < n; k++) {
                    long v = rx[k] + lrint(TONE_X62_A * sin(tone_ph_x62));

                    tone_ph_x62 = fmod(tone_ph_x62 + 2 * M_PI * TONE_X62_HZ / 44100.0, 2 * M_PI);
                    rx[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            int m = stick_take(chunk, n);    /* the stick's, buffered; the X6200's samples pace it */

            float to = atomic_load(&tones) || !atomic_load(&sql_db) || atomic_load(&sql_open) ? 1.0f : 0.0f;
            float ks = atomic_load(&g_stick) / 1000.0f, kx = atomic_load(&g_x62) / 1000.0f;

            /* Each as it is, at its own page's volume (hear_vol_refresh: the stick as on B, the X6200 as on A): the
             * two added, nothing raised or levelled; a soft knee from 3/4 of full scale only where the sum would
             * clip */
            for (int k = 0; k < n; k++) {
                if (g < to) g = g + 1.0f / 441 > to ? to : g + 1.0f / 441;
                else if (g > to) g = g - 1.0f / 441 < to ? to : g - 1.0f / 441;
                float v = (k < m ? chunk[k] * g * ks : 0) + rx[k] * kx, a = fabsf(v);

                if (a > 24000) v = (v > 0 ? 1 : -1) * (24000 + (a - 24000) * 8767 / (a - 24000 + 8767));
                chunk[k] = (int16_t)v;
            }
            rtl_watch_mix(chunk, n);            /* the watch channels open, added */
            audio_play(chunk, n);
            continue;
        }

        int n = stick_take(chunk, CHUNK);     /* the stick alone, buffered as BOTH's: audio_play paces it */

        if (n) {
            for (int k = n; k < CHUNK; k++) chunk[k] = 0;     /* it ran dry: the rest silent, then filled again */
            n = CHUNK;
            float to = !atomic_load(&sql_db) || atomic_load(&sql_open) ? 1.0f : 0.0f;

            for (int k = 0; k < n; k++) {
                if (g < to) g = g + 1.0f / 441 > to ? to : g + 1.0f / 441;
                else if (g > to) g = g - 1.0f / 441 < to ? to : g - 1.0f / 441;
                chunk[k] = (int16_t)(chunk[k] * g);
            }
            rtl_watch_mix(chunk, n);            /* the watch channels open, added */
            if (atomic_load(&rscope_on)) {
                pthread_mutex_lock(&rscope_lock);
                for (int k = 0; k < n; k++) {
                    rscope[(rscope_head + rscope_len) % RSCOPE] = chunk[k];
                    if (rscope_len < RSCOPE) rscope_len++;
                    else rscope_head = (rscope_head + 1) % RSCOPE;
                }
                pthread_mutex_unlock(&rscope_lock);
            }
            audio_play(chunk, n);
        } else {
            usleep(5000);
        }
    }
    return NULL;
}

void rtl_spectrum_listen(int64_t hz, int mode) {
    char cmd[48];

    if (!hz) mode = 0;
    bool was = atomic_load(&listen_mode) != 0;

    atomic_store(&listen_hz, hz);
    atomic_store(&listen_mode, mode);
    if (mode && !was) {
        pthread_mutex_lock(&ring_lock);
        ring_head = ring_len = 0;
        st_run    = false;              /* filled to ST_FILL again before it plays */
        pthread_mutex_unlock(&ring_lock);
    }
    snprintf(cmd, sizeof(cmd), "l %lld %d\n", (long long)(mode ? hz : 0), mode);
    say(cmd);
    if (mode) rtl_spectrum_player_start();
}

/* The player running: at the first listen, or for the watch channels from the start (rtl_watch.c), so one restored
 * at boot is heard when it opens though nothing listened yet. The LVGL thread's */
void rtl_spectrum_player_start(void) {
    if (player_started) return;
    player_started = pthread_create(&player, NULL, player_thread, NULL) == 0;
    if (player_started) pthread_detach(player);
}

bool rtl_spectrum_listening(void) {
    return atomic_load(&listen_mode) != 0;
}

int64_t rtl_spectrum_listen_hz(void) {
    return atomic_load(&listen_hz);
}

bool rtl_spectrum_on_headphones(void) {
    return atomic_load(&on_bt);
}

bool rtl_spectrum_on_monitor(void) {
    return atomic_load(&on_moni);
}

void rtl_spectrum_scope(bool on) {
    pthread_mutex_lock(&rscope_lock);
    atomic_store(&rscope_on, on);
    rscope_head = rscope_len = 0;
    pthread_mutex_unlock(&rscope_lock);
}

size_t rtl_spectrum_scope_read(int16_t *buf, size_t n) {
    pthread_mutex_lock(&rscope_lock);
    if (n > (size_t)rscope_len) n = rscope_len;
    for (size_t i = 0; i < n; i++) buf[i] = rscope[(rscope_head + i) % RSCOPE];
    rscope_head = (rscope_head + (int)n) % RSCOPE;
    rscope_len -= (int)n;
    pthread_mutex_unlock(&rscope_lock);
    return n;
}

float rtl_spectrum_level(void) {
    return atomic_load(&chan_over) / 10.0f;
}

void rtl_spectrum_mux(bool on) {
    atomic_store(&mux, on);
}

void rtl_spectrum_levels(float stick, float x6200) {
    atomic_store(&g_stick, (int)(stick * 1000));
    atomic_store(&g_x62, (int)(x6200 * 1000));
}

void rtl_spectrum_mute(bool on) {
    atomic_store(&mute, on);
    if (on && !player_started) {
        player_started = pthread_create(&player, NULL, player_thread, NULL) == 0;
        if (player_started) pthread_detach(player);
    }
}

void rtl_spectrum_squelch(int db) {
    atomic_store(&sql_db, db < 0 ? 0 : db);
}

bool rtl_spectrum_squelch_open(void) {
    return !atomic_load(&sql_db) || atomic_load(&sql_open);
}

void rtl_spectrum_watch(int64_t hz) {
    char cmd[40];

    if (atomic_load(&watch_hz) == hz) return;
    atomic_store(&watch_hz, hz);
    snprintf(cmd, sizeof(cmd), "w %lld\n", (long long)hz);
    say(cmd);
}

bool rtl_spectrum_alert(char *text, size_t n, time_t *at, uint32_t *seq) {
    bool got = false;

    pthread_mutex_lock(&lock);
    if (alert_seq != *seq) {
        snprintf(text, n, "%s", alert);
        *at  = alert_at;
        *seq = alert_seq;
        got  = true;
    }
    pthread_mutex_unlock(&lock);
    return got;
}

void rtl_spectrum_rds_watch(int64_t hz) {
    char cmd[40];

    if (atomic_load(&rds_hz) == hz) return;
    atomic_store(&rds_hz, hz);
    pthread_mutex_lock(&lock);
    uint32_t seq = rds_now.seq + 1;

    memset(&rds_now, 0, sizeof(rds_now));
    rds_now.pty = -1;
    rds_now.seq = seq;
    pthread_mutex_unlock(&lock);
    snprintf(cmd, sizeof(cmd), "d %lld\n", (long long)hz);
    say(cmd);
}

bool rtl_spectrum_dmr(rtl_dmr_t *out) {
    bool got;

    pthread_mutex_lock(&lock);
    *out = dmr_now;
    got  = dmr_now.at_ms && dmr_now.hz == atomic_load(&listen_hz) && lv_tick_or_ms() - dmr_now.at_ms < 4000;
    pthread_mutex_unlock(&lock);
    return got;
}

bool rtl_spectrum_dsd(rtl_dsd_t *out) {
    bool got;

    pthread_mutex_lock(&lock);
    *out = dsd_now;
    got  = dsd_now.at_ms && dsd_now.proto[0] && dsd_now.hz == atomic_load(&listen_hz) &&
          lv_tick_or_ms() - dsd_now.at_ms < 3000;
    pthread_mutex_unlock(&lock);
    return got;
}

const char *rtl_dsd_name(const char *proto) {
    static const char *const names[][2] = {{"P25P1", "P25"}, {"P25P2", "P25 PHASE 2"}, {"DSTAR", "D-STAR"},
                                           {"X2TDMA", "X2-TDMA"}, {"EDACS/PV", "PROVOICE"}};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(proto, names[i][0])) return names[i][1];
    return proto;
}

void rtl_spectrum_meteor(int64_t hz) {
    char cmd[40];

    if (atomic_load(&met_hz) == hz) return;
    atomic_store(&met_hz, hz);
    snprintf(cmd, sizeof(cmd), "m %lld\n", (long long)hz);
    say(cmd);
}

bool rtl_spectrum_lrpt(rtl_lrpt_t *out) {
    bool got;

    pthread_mutex_lock(&lock);
    *out = lrpt_now;
    got  = lrpt_now.at_ms && lrpt_now.hz && lv_tick_or_ms() - lrpt_now.at_ms < 3000;
    pthread_mutex_unlock(&lock);
    return got;
}

bool rtl_spectrum_lrpt_picture(uint32_t *px, int w, int h, int stride, int view, uint32_t *ver) {
    pthread_mutex_lock(&lock);
    if (*ver == lp_ver) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    *ver = lp_ver;
    if (w > LP_LINE) w = LP_LINE;
    int top = (lp_top + 1) * 4;                         /* the line after the newest */
    for (int y = 0; y < h; y++) {
        uint32_t *o    = px + (size_t)y * stride;
        int       line = top - h + y, strip = line >= 0 ? line / 4 : -1, slot = strip >= 0 ? strip % LP_KEEP : 0;
        const uint8_t *a = NULL, *b = NULL;             /* colour: 65 (red, green) and 64 (blue); else the channel */

        if (strip >= 0) {
            int ca = view ? view - 1 : 1, cb = view ? -1 : 0;
            if (lp_px[ca] && lp_strip[ca][slot] == strip) a = lp_px[ca] + ((size_t)slot * 4 + line % 4) * LP_LINE;
            if (cb >= 0 && lp_px[cb] && lp_strip[cb][slot] == strip)
                b = lp_px[cb] + ((size_t)slot * 4 + line % 4) * LP_LINE;
        }
        for (int x = 0; x < w; x++) {
            uint32_t g = a ? a[x] : 0, bl = view ? g : b ? b[x] : 0;
            o[x] = 0xff000000u | g << 16 | g << 8 | bl;
        }
    }
    pthread_mutex_unlock(&lock);
    return true;
}

bool rtl_spectrum_rds(rtl_rds_t *out) {
    bool got;

    pthread_mutex_lock(&lock);
    *out = rds_now;
    got  = rds_now.pi && rds_now.hz && rds_now.hz == atomic_load(&rds_hz);
    pthread_mutex_unlock(&lock);
    return got;
}

void rtl_spectrum_acars(bool on) {
    if (atomic_exchange(&acars_on, on) == on) return;
    say(on ? "c 1\n" : "c 0\n");
}

bool rtl_spectrum_acars_on(void) { return atomic_load(&acars_on); }

bool rtl_spectrum_acars_msg(int i, rtl_acars_t *out) {
    bool got = false;

    pthread_mutex_lock(&lock);
    if (i >= 0 && (uint32_t)i < acars_n && i < RTL_ACARS_KEEP) {
        *out = acars_ring[(acars_n - 1 - (uint32_t)i) % RTL_ACARS_KEEP];
        got  = true;
    }
    pthread_mutex_unlock(&lock);
    return got;
}

uint32_t rtl_spectrum_acars_seq(void) {
    pthread_mutex_lock(&lock);
    uint32_t n = acars_n;
    pthread_mutex_unlock(&lock);
    return n;
}

/* ---- The device ---- */

rtl_dev_t rtl_spectrum_dev(void) {
    int k = atomic_load(&dev_kind);

    if (k != RTL_DEV_NONE) return (rtl_dev_t)k;
    const char *u = adsb_rx_sdr_kind();         /* on USB, before the daemon says (or with none) */

    return !strcmp(u, "HACKRF") ? RTL_DEV_HACKRF : !strcmp(u, "BLADERF") ? RTL_DEV_BLADERF : u[0] ? RTL_DEV_RTL
                                                                                                : RTL_DEV_NONE;
}

const char *rtl_spectrum_dev_tag(void) {
    rtl_dev_t d = rtl_spectrum_dev();

    return d == RTL_DEV_HACKRF ? "HACKRF" : d == RTL_DEV_BLADERF ? "BLADERF"
         : d == RTL_DEV_RTL    ? rtl_spectrum_rtl_tag(rtl_spectrum_dev_rtl()) : "SDR";
}

const char *rtl_spectrum_dev_name(void) {
    rtl_dev_t d = rtl_spectrum_dev();

    return d == RTL_DEV_HACKRF ? "HACKRF" : d == RTL_DEV_BLADERF ? "BLADERF"
         : d == RTL_DEV_RTL    ? rtl_spectrum_rtl_tag(rtl_spectrum_dev_rtl()) : "SDR";     /* RTL0, RTL1...: the user,
                                                                         * 2026-11-16, "all single stick RTL should be
                                                                         * called RTL0" */
}

int64_t rtl_spectrum_min_hz(void) {
    return atomic_load(&dev_lo);
}

int64_t rtl_spectrum_max_hz(void) {
    return atomic_load(&dev_hi);
}

bool rtl_spectrum_can_tx(void) {
    return atomic_load(&dev_tx) && rtl_spectrum_daemon();
}

/* ---- Transmitting ---- */

/* The voice to the daemon while transmitting: the capture (the hand mic, the base in record mode) as it comes, in
 * messages of up to 50 ms; its loudest of each 0.1 s for the screen. The base's audio put back as the player wants
 * it when it ends */
static void *tx_mic_thread(void *arg) {
    static uint8_t msg[8 + 2205 * 2];
    int16_t        buf[2205];
    int            peak = 0, got = 0;
    (void)arg;
    pthread_setname_np(pthread_self(), "sdr_mic");

    radio_set_play_mode(AUDIO_PLAY_VOICE_REC);
    audio_mic_tap(true);
    while (atomic_load(&tx_mic)) {
        size_t n = audio_mic_tap_read(buf, 2205);

        if (!n) {
            usleep(10000);
            continue;
        }
        uint32_t cnt = (uint32_t)n;

        memcpy(msg, "TAUD", 4);
        memcpy(msg + 4, &cnt, 4);
        memcpy(msg + 8, buf, n * 2);
        pthread_mutex_lock(&lock);
        if (cmd_fd >= 0) (void)send(cmd_fd, msg, 8 + n * 2, MSG_DONTWAIT | MSG_NOSIGNAL);
        pthread_mutex_unlock(&lock);
        for (size_t i = 0; i < n; i++) {
            int a = abs(buf[i]);

            if (a > peak) peak = a;
        }
        if ((got += (int)n) >= 4410) {
            atomic_store(&tx_mic_cdb, peak ? (int)lrintf(2000 * log10f(peak / 32768.0f)) : -9900);
            peak = got = 0;
        }
    }
    audio_mic_tap(false);
    /* the base's audio as the player has it: playing the stick on the speaker (play mode), or not */
    radio_set_play_mode(atomic_load(&playing) && !atomic_load(&on_bt) && !atomic_load(&player_moni) ? AUDIO_PLAY_ON
                                                                                                      : AUDIO_PLAY_OFF);
    atomic_store(&tx_mic_cdb, -9900);
    return NULL;
}

void rtl_spectrum_tx(int64_t hz, int gain_db, bool amp) {
    char cmd[64];

    if (hz > 0) {
        if (atomic_load(&tx_mic)) return;           /* on already */
        if (tx_thr_on) pthread_join(tx_thr, NULL);  /* the last one's thread, done */
        tx_thr_on = false;
        atomic_store(&tx_state, RTL_TX_ASKED);
        atomic_store(&tx_mic, true);
        tx_thr_on = pthread_create(&tx_thr, NULL, tx_mic_thread, NULL) == 0;
        if (!tx_thr_on) atomic_store(&tx_mic, false);
        snprintf(cmd, sizeof(cmd), "t %lld %d %d\n", (long long)hz, gain_db, amp ? 1 : 0);
        say(cmd);
        fprintf(stderr, "sdr transmitter: asked for %lld Hz, TX gain %d dB, amplifier %s\n", (long long)hz, gain_db,
                amp ? "on" : "off");
    } else {
        say("t 0\n");
        atomic_store(&tx_mic, false);               /* the mic thread ends, the base's audio back */
        if (atomic_load(&tx_state) == RTL_TX_ASKED) atomic_store(&tx_state, RTL_TX_OFF);
    }
}

rtl_tx_state_t rtl_spectrum_tx_state(void) {
    return (rtl_tx_state_t)atomic_load(&tx_state);
}

uint32_t rtl_spectrum_tx_said(char *s, size_t n) {
    pthread_mutex_lock(&lock);
    snprintf(s, n, "%s", tx_said);
    uint32_t k = tx_said_n;
    pthread_mutex_unlock(&lock);
    return k;
}

float rtl_spectrum_tx_mic_db(void) {
    return atomic_load(&tx_mic_cdb) / 100.0f;
}
