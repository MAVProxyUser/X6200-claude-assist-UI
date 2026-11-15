/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An RTL-SDR stick's spectrum (rtl_spectrum.h). The stick is the daemon's, /usr/bin/x6x00-rtlspec
 *  (tools/rtlspec/rtlspec.c): it opens it once and keeps it streaming for as long as the radio runs. A thread of its
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
#include "audio.h"
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
#define RING        (44100 * 2) /* audio waiting to play: two seconds at most */
#define CHUNK       1024        /* audio_play() needs room for the whole write: a bigger one may never find it */
#define ST_FILL     13230       /* BOTH: the stick's audio buffered, 300 ms: its 50 ms messages come in clumps and the
                                 * X6200's 100 ms fragments take it in bursts; at 150 ms it ran dry, a gap each time
                                 * (up to 130 ms, two of three 10 s tone tests, 2026-11-15) */
#define ST_SLACK    4410        /* and let drift 100 ms either way before a sample is dropped or said twice */

static pthread_t       thread;
static bool            started;
static atomic_bool     want;
static atomic_bool     yielded;          /* AIRSPACE's screen open: its dump1090 has the stick's samples */

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

/* SAME alerts from the watched channel (NOAA Weather Radio): the last one, also kept in ALERT_LOG */
#define ALERT_LOG "/mnt/wx_alerts.log"
static atomic_llong    watch_hz;
static char            alert[300];
static time_t          alert_at;
static uint32_t        alert_seq;

/* Listening: the helper's audio, played on the speaker in the recorder's way (the base in play mode) */
static atomic_llong    listen_hz;
static atomic_int      listen_mode;      /* 0 not listening */
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t         ring[RING];
static int             ring_head, ring_len;
static pthread_t       player;
static bool            player_started;
static atomic_bool     playing;          /* the player has the speaker */
static atomic_bool     mux;              /* BOTH: the X6200's receiver mixed in (audio_rx_tap) */
static atomic_bool     mute;             /* OFF: the speaker taken and given nothing, neither radio heard */
static atomic_int      g_stick = 1000, g_x62 = 1000;   /* each receiver's share in the mix, thousandths (BOTH) */
/* BOTH's test (on while /tmp/both_tones exists, looked for each second): a steady tone on each receiver's line, as
 * close to where its audio comes in as can be, to hear the mix: 700 Hz in place of the stick's audio as the daemon
 * sends it (through the buffer and its timing, as a station would be), 1300 Hz added to what the base sends of the
 * X6200's (its return of what is played left in, as the canceller meets it); the stick's squelch held open */
#define TONE_STICK_HZ 700
#define TONE_X62_HZ   1300
#define TONE_STICK_A  8000
#define TONE_X62_A    3000
static atomic_int      tones;            /* 0 off, 1 the tones (/tmp/both_tones), 2 noise (/tmp/both_noise) */
static uint32_t        noise_st = 0x12345678, noise_x6 = 0x9e3779b9;
static float           nlp_st, nhp_st, nx_st, nlp_x6, nhp_x6, nx_x6;
/* Noise as voice spans it (about 300 Hz to 3 kHz), its own on each line: never like itself a delay later, as a
 * tone is, and spread over the band, so the canceller learns the base's return all over it */
static float noise_next(uint32_t *st, float *lp, float *hp, float *xp) {
    *st ^= *st << 13, *st ^= *st >> 17, *st ^= *st << 5;
    float w = (float)(int32_t)*st / 2147483648.0f, y;
    *lp = 0.652f * *lp + 0.348f * w;            /* one pole at 3 kHz */
    y   = *lp - *xp + 0.958f * *hp;             /* DC out, the low end from about 300 Hz */
    *xp = *lp, *hp = y;
    return y;
}
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
    char  f[24], g[16], n[16], r[16], s[16];
    pid_t p;

    snprintf(f, sizeof(f), "%lld", (long long)atomic_load(&want_hz));
    snprintf(g, sizeof(g), "%d", atomic_load(&want_gain));
    snprintf(n, sizeof(n), "%d", BINS);
    snprintf(r, sizeof(r), "%d", ROWS);
    snprintf(s, sizeof(s), "%d", RATE);
    if ((p = fork()) < 0) return;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR), log = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
        if (log >= 0) dup2(log, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl(HELPER, HELPER, "-f", f, "-g", g, "-n", n, "-r", r, "-s", s, (char *)NULL);
        _exit(127);
    }
    waitpid(p, NULL, 0);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
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
    qsort(sorted, n, sizeof(float), cmp_float);
    float over = peak - sorted[n / 2];
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

static void *spec_thread(void *arg) {
    static uint8_t buf[24 + MAX_BINS * 4 + 64];
    int            fd = -1, asked = -1, failed = 0;
    char           last_st[16] = "";    /* the daemon's last word on the stick: it says it again only when it changes */
    (void)arg;

    for (;;) {
        bool k = kept();

        /* AIRSPACE's dump1090 runs only while its screen is open, on the daemon's samples (adsb_rx.c) */
        adsb_rx_lend(!atomic_load(&yielded));
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
            snprintf(cmd, sizeof(cmd), "s %d\n", RATE);
            say(cmd);
            snprintf(cmd, sizeof(cmd), "g %d\n", atomic_load(&want_gain));
            say(cmd);
            snprintf(cmd, sizeof(cmd), "f %lld\n", (long long)atomic_load(&want_hz));
            say(cmd);
            if (atomic_load(&watch_hz)) {
                snprintf(cmd, sizeof(cmd), "w %lld\n", (long long)atomic_load(&watch_hz));
                say(cmd);
            }
            if (atomic_load(&listen_mode)) {
                snprintf(cmd, sizeof(cmd), "l %lld %d\n", (long long)atomic_load(&listen_hz),
                         atomic_load(&listen_mode));
                say(cmd);
            }
        }
        if ((int)k != asked) {          /* rows asked for while wanted, and only then */
            say(k ? "r 1\n" : "r 0\n");
            asked = k;
        }
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
            pthread_mutex_lock(&lock);
            cmd_fd = -1;
            pthread_mutex_unlock(&lock);
            close(fd);
            fd = -1;
            sleep(1);
            continue;
        }

        if (n > 8 && memcmp(buf, "RTLSPEC ", 8) == 0) {
            char     st[16] = "", tn[16] = "";
            int      ver = 0, nb = 0;
            unsigned rate = 0;

            buf[n] = 0;
            int got = sscanf((char *)buf, "RTLSPEC %d %15s %15s %u %d", &ver, st, tn, &rate, &nb);

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
            if (got != 5) continue;
            snprintf(last_st, sizeof(last_st), "%s", st);
            pthread_mutex_lock(&lock);
            snprintf(tuner, sizeof(tuner), "%s", strcmp(tn, "-") ? tn : "");
            pthread_mutex_unlock(&lock);
            if (k && !(state == RTL_SPEC_ON && !strcmp(st, "ON"))) state = daemon_state(st);
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
                if (atomic_load(&tones) == 1) {     /* the test's tone in place of the stick's audio */
                    s = (int16_t)lrint(TONE_STICK_A * sin(tone_ph_stick));
                    tone_ph_stick = fmod(tone_ph_stick + 2 * M_PI * TONE_STICK_HZ / 44100.0, 2 * M_PI);
                } else if (atomic_load(&tones) == 2) {  /* or its noise */
                    float v = 12000 * noise_next(&noise_st, &nlp_st, &nhp_st, &nx_st);
                    s = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
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

void rtl_spectrum_want(bool on, int64_t hz, int gain) {
    if (hz < RTL_SPEC_MIN_HZ) hz = RTL_SPEC_MIN_HZ;
    if (hz > RTL_SPEC_MAX_HZ) hz = RTL_SPEC_MAX_HZ;
    atomic_store(&want_hz, hz);
    atomic_store(&want_gain, gain);
    atomic_store(&want, on);
    if (on && !started) {
        started = pthread_create(&thread, NULL, spec_thread, NULL) == 0;
        if (started) pthread_detach(thread);
    }
}

void rtl_spectrum_yield(bool on) {
    atomic_store(&yielded, on);
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

void rtl_spectrum_tune(int64_t hz) {
    char cmd[32];

    if (hz < RTL_SPEC_MIN_HZ) hz = RTL_SPEC_MIN_HZ;
    if (hz > RTL_SPEC_MAX_HZ) hz = RTL_SPEC_MAX_HZ;
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

/* ---- BOTH's echo. The base sends the speaker's audio back with its receiver's while the GUI plays (base 1.0.8,
 * measured 2026-11-14: what was played came back 406 ms later at 0.6 of its level), so the X6200's samples mixed in
 * came back and were played again, over and over till they faded. Cancelled here: the played samples are kept, the
 * delay of their return is found every 2 s (the captured against the played: their loudness in 10 ms steps, then
 * sample by sample, decimated, then exact), and an adaptive filter (NLMS, EC_TAPS around that delay) takes their
 * return out of what the receiver sends before it is mixed. The delay is the found one, not the queues' fill read
 * each pass: that swings by 150 ms while the delay holds to the sample (521.1 ms, four searches, 2026-11-14) ---- */

#define EC_HIST   131072                /* played and captured samples kept: 3 s (a power of two; the search wants 2.2) */
#define EC_MASK   (EC_HIST - 1)
#define EC_TAPS   320                   /* -1.5 to +5.8 ms around the delay: the return is mostly one tap (96% of
                                         * its power), a tail to 6 ms after it holds most of the rest; longer took
                                         * no more (least squares on 1.5 s dumped, 2026-11-14). A multiple of 16 */
#define EC_PRE    64                    /* of them before the delay */
#define EC_FH     65536                 /* the played, as floats for the filter: 1.5 s, past EC_MAXLAG and its taps */
#define EC_FMASK  (EC_FH - 1)
#define EC_PRIOR  22977                 /* the delay till one is found: 521 ms, as the GUI sees it (2026-11-14) */
#define EC_MINLAG 11025                 /* the delay looked for between 250 ms (the speech's own slow swells, nearer, */
                                        /* are not it) and EC_MAXLAG, short of the echo's echo at twice the delay */
#define EC_WIN    44100                 /* the delay looked for over the last second */
#define EC_MAXLAG 33075                 /* 750 ms */
static int16_t  ec_play[EC_HIST], ec_cap[EC_HIST], ec_res[EC_HIST];   /* played, captured, captured less its echo */
static float    ec_pf[EC_FH * 2];       /* the played again, each sample at i and i + EC_FH: any EC_TAPS of them in
                                         * a row, whatever the wrap, so the filter's loops run straight (NEON) */
static float    ec_sf[EC_FH * 2];       /* the stick's part of it alone, kept the same way: what the filter learns
                                         * from (ec_cancel) */
static uint32_t ec_n;                   /* samples captured (as many played) since BOTH started */
static int      ec_d = EC_PRIOR;        /* the return's delay in samples: played sample p comes back as captured p+d
                                         * (both streams on the codec's clock: it holds till a queue drops or skips
                                         * samples). Kept from one BOTH to the next */
static int      ec_cand = -1;           /* a new delay found once, moved to when found again */
static bool     ec_locked;              /* a delay found since BOTH started */
static float    ec_w[EC_TAPS];          /* tap j: the played lag ec_d - EC_PRE + EC_TAPS - 1 - j samples back */
static float    ec_pin, ec_pout;        /* the captured's power and what is left of it, this second (ec_cancel) */
static float    ec_guard = 1;           /* the X6200's share let through: 1 but while the played rings (ec_ring) */
static double   ec_rxy, ec_rxx, ec_ryy; /* what the filter left of the X6200's against the played a delay before,
                                         * this quarter second: the return not taken out */
#define EC_FILE   "/mnt/both_echo.bin"  /* the filter learnt (its delay and weights), for the next start: a GUI that
                                         * starts empty while the stick is quiet has nothing to learn from, and the
                                         * X6200 alone rang through the base (2026-11-14) */
static uint32_t ec_next;                /* the next look for the delay */
static float    ec_found;               /* the last search's correlation (for the log) */

static void ec_load(void) {
    static bool done;
    FILE       *f;
    int         d;
    float       w[EC_TAPS];

    if (done) return;
    done = true;
    if (!(f = fopen(EC_FILE, "rb"))) return;
    if (fread(&d, sizeof(d), 1, f) == 1 && fread(w, sizeof(w), 1, f) == 1 && d > EC_TAPS && d < EC_MAXLAG) {
        bool ok = true;
        for (int j = 0; j < EC_TAPS; j++) ok = ok && isfinite(w[j]) && fabsf(w[j]) <= 2;
        if (ok) {
            ec_d = d;
            memcpy(ec_w, w, sizeof(w));
            fprintf(stderr, "rtl BOTH echo: the filter learnt before, at %.1f ms\n", d / 44.1);
        }
    }
    fclose(f);
}

static void ec_save(void) {
    FILE *f = fopen(EC_FILE ".new", "wb");

    if (!f) return;
    bool ok = fwrite(&ec_d, sizeof(ec_d), 1, f) == 1 && fwrite(ec_w, sizeof(ec_w), 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    if (ok) rename(EC_FILE ".new", EC_FILE);
}

static void ec_reset(void) {
    ec_load();
    memset(ec_play, 0, sizeof(ec_play));
    memset(ec_cap, 0, sizeof(ec_cap));
    memset(ec_res, 0, sizeof(ec_res));
    memset(ec_pf, 0, sizeof(ec_pf));
    memset(ec_sf, 0, sizeof(ec_sf));    /* the weights kept: the base's return is the same one BOTH to the next */
    ec_n      = 0;
    ec_cand   = -1;
    ec_locked = false;
    ec_guard  = 0;                      /* the X6200 joins when the return is found (2 s at most), not before: till
                                         * then the filter would take it out where the last start had it */
    ec_rxy = ec_rxx = ec_ryy = 0;
    ec_next   = EC_WIN + EC_MAXLAG;     /* once there is a window and its lags kept */
}

/* corr of the captured (or what is left of it, res) from sample a (len n, step st) with the played lag samples
 * earlier */
static float ec_corr(const int16_t *cap, uint32_t a, int n, int lag, int st) {
    double sxy = 0, sxx = 0, syy = 0;

    for (int i = 0; i < n; i += st) {
        double y = cap[(a + i) & EC_MASK], x = ec_play[(a + i - lag) & EC_MASK];
        sxy += x * y, sxx += x * x, syy += y * y;
    }
    return (float)(sxy / (sqrt(sxx * syy) + 1e-9));
}

/* The return's delay, from the last EC_WIN samples: their loudness in 10 ms blocks against the played's over 0 to
 * EC_MAXLAG, then every 10th sample around the best block, then each sample around that. 0: none clear */
static int ec_find(void) {
    static float ec[EC_WIN / 441], ep[(EC_WIN + EC_MAXLAG) / 441];
    int          nb = EC_WIN / 441, np = (EC_WIN + EC_MAXLAG) / 441, bl = 0, best = -1;
    uint32_t     a = ec_n - EC_WIN, ap = ec_n - EC_WIN - EC_MAXLAG;
    float        bc = 0.2f, mc = 0, mp = 0;

    for (int b = 0; b < nb; b++) {
        double s = 0;
        for (int i = 0; i < 441; i++) {
            double v = ec_cap[(a + b * 441 + i) & EC_MASK];
            s += v * v;
        }
        mc += (ec[b] = (float)sqrt(s / 441)) / nb;
    }
    for (int b = 0; b < np; b++) {
        double s = 0;
        for (int i = 0; i < 441; i++) {
            double v = ec_play[(ap + b * 441 + i) & EC_MASK];
            s += v * v;
        }
        mp += (ep[b] = (float)sqrt(s / 441)) / np;
    }
    for (int L = EC_MINLAG / 441; L < EC_MAXLAG / 441; L++) {   /* the captured block b against the played EC_MAXLAG/441-L+b */
        double sxy = 0, sxx = 0, syy = 0;
        for (int b = 0; b < nb; b++) {
            double y = ec[b] - mc, x = ep[EC_MAXLAG / 441 - L + b] - mp;
            sxy += x * y, sxx += x * x, syy += y * y;
        }
        float r = (float)(sxy / (sqrt(sxx * syy) + 1e-9));
        if (r > bc) bc = r, bl = L;
    }
    ec_found = bc;
    if (!bl) return 0;
    bc = 0.15f;                         /* either sign: the base sends it back inverted (its tap at -0.75) */
    for (int L = bl * 441 - 441; L <= bl * 441 + 441; L += 10) {
        float r = fabsf(ec_corr(ec_cap, a, EC_WIN, L, 10));
        if (r > bc) bc = r, best = L;
    }
    if (best < 0) return 0;
    ec_found = bc;
    int coarse = best;
    for (int L = coarse - 10; L <= coarse + 10; L++) {
        float r = fabsf(ec_corr(ec_cap, a, EC_WIN / 2, L, 1));
        if (r > bc) bc = r, best = L;
    }
    ec_found = bc;
    return best;
}

/* The filter's two loops: the echo's estimate (a dot product) and the weights' step */
#if defined(__ARM_NEON)
#include <arm_neon.h>
/* 16 a turn, four sums apart: the A7 runs in order and a multiply-add's result is a while coming */
static float ec_dot(const float *w, const float *x) {
    float32x4_t s0 = vdupq_n_f32(0), s1 = s0, s2 = s0, s3 = s0;

    for (int j = 0; j < EC_TAPS; j += 16) {
        float32x4_t w0 = vld1q_f32(w + j), w1 = vld1q_f32(w + j + 4), w2 = vld1q_f32(w + j + 8),
                    w3 = vld1q_f32(w + j + 12), x0 = vld1q_f32(x + j), x1 = vld1q_f32(x + j + 4),
                    x2 = vld1q_f32(x + j + 8), x3 = vld1q_f32(x + j + 12);
        s0 = vmlaq_f32(s0, w0, x0);
        s1 = vmlaq_f32(s1, w1, x1);
        s2 = vmlaq_f32(s2, w2, x2);
        s3 = vmlaq_f32(s3, w3, x3);
    }
    s0            = vaddq_f32(vaddq_f32(s0, s1), vaddq_f32(s2, s3));
    float32x2_t r = vadd_f32(vget_low_f32(s0), vget_high_f32(s0));
    return vget_lane_f32(vpadd_f32(r, r), 0);
}
static void ec_step(float *w, const float *x, float mu) {
    float32x4_t m = vdupq_n_f32(mu);

    for (int j = 0; j < EC_TAPS; j += 16) {
        float32x4_t w0 = vld1q_f32(w + j), w1 = vld1q_f32(w + j + 4), w2 = vld1q_f32(w + j + 8),
                    w3 = vld1q_f32(w + j + 12), x0 = vld1q_f32(x + j), x1 = vld1q_f32(x + j + 4),
                    x2 = vld1q_f32(x + j + 8), x3 = vld1q_f32(x + j + 12);
        w0 = vmlaq_f32(w0, m, x0);
        w1 = vmlaq_f32(w1, m, x1);
        w2 = vmlaq_f32(w2, m, x2);
        w3 = vmlaq_f32(w3, m, x3);
        vst1q_f32(w + j, w0);
        vst1q_f32(w + j + 4, w1);
        vst1q_f32(w + j + 8, w2);
        vst1q_f32(w + j + 12, w3);
    }
}
#else
static float ec_dot(const float *w, const float *x) {
    float s = 0;
    for (int j = 0; j < EC_TAPS; j++) s += w[j] * x[j];
    return s;
}
static void ec_step(float *w, const float *x, float mu) {
    for (int j = 0; j < EC_TAPS; j++) w[j] += mu * x[j];
}
#endif

/* The receiver's n samples (rx) with the played audio's return taken out, into out: a filter over EC_TAPS played
 * samples around the delay. It learns from the stick's part of the played only (an instrument): the played holds the
 * X6200's own audio too, what the filter leaves of it, so learnt from the played the filter's error fed its own input
 * and it settled at 2.6 times the return (-1.5 for -0.58: the echo put back louder, inverted, ringing at its delay,
 * dumped and measured 2026-11-14). The stick's audio has none of the X6200's in it: learnt from that it settles on the
 * return itself. While the stick is silent (its squelch shut) the filter holds and still takes the return out. The
 * samples played (written by the caller after) are kept with ec_played */
static void ec_cancel(const int16_t *rx, float *out, int n) {
    double pz = 0, px = 0;

    for (int k = 0; k < n; k++) {
        uint32_t     i  = ec_n + k, o = (i - ec_d + EC_PRE - EC_TAPS + 1) & EC_FMASK;
        const float *x  = ec_pf + o, *z = ec_sf + o;   /* x[EC_TAPS-1]: lag ec_d - EC_PRE */

        if (k == 0) {
            for (int j = 0; j < EC_TAPS; j++) pz += (double)z[j] * z[j], px += (double)x[j] * x[j];
        } else {
            int    b  = -1 + (o == 0 ? EC_FH : 0);
            double zi = z[EC_TAPS - 1], zo = z[b], xi = x[EC_TAPS - 1], xo = x[b];
            pz += zi * zi - zo * zo;
            px += xi * xi - xo * xo;
            if (pz < 0) pz = 0;
            if (px < 0) px = 0;
        }
        float e = rx[k] - ec_dot(ec_w, x);

        if (!isfinite(e)) {             /* never: but a filter gone wrong is started over, never left to mute */
            memset(ec_w, 0, sizeof(ec_w));
            e = rx[k];
        }
        /* the stick heard: learnt from, by a step that stays under 0.01 of the error however loud the played is
         * against the stick's part (the step's gain is (z.x)/|z||x| <= 1, Cauchy-Schwarz) */
        if (pz > EC_TAPS * 250.0 * 250) ec_step(ec_w, z, (float)(0.01 * e / sqrt(pz * px + 1e12)));
        out[k]              = e;
        ec_cap[i & EC_MASK] = rx[k];
        ec_res[i & EC_MASK] = (int16_t)(e > 32767 ? 32767 : e < -32768 ? -32768 : e);
        ec_pin  += (float)rx[k] * rx[k];
        ec_pout += e * e;
    }
    for (int j = 0; j < EC_TAPS; j++)   /* no return is over twice what left */
        ec_w[j] = ec_w[j] > 2 ? 2 : ec_w[j] < -2 ? -2 : ec_w[j];
}

/* The filter moved to delay d: its weights slide with it (tap j means lag d - EC_PRE + EC_TAPS - 1 - j) */
static void ec_move(int d) {
    int s = d - ec_d;                   /* lag L was tap j = ec_d - PRE + T - 1 - L, now j + s */

    if (abs(s) >= EC_TAPS) {
        memset(ec_w, 0, sizeof(ec_w));
    } else if (s > 0) {
        memmove(ec_w + s, ec_w, (EC_TAPS - s) * sizeof(float));
        memset(ec_w, 0, s * sizeof(float));
    } else if (s < 0) {
        memmove(ec_w, ec_w - s, (EC_TAPS + s) * sizeof(float));
        memset(ec_w + EC_TAPS + s, 0, -s * sizeof(float));
    }
    ec_d = d;
}

/* The filter's strongest tap: its index, and its weight in *w */
static int ec_peak(float *w) {
    int pk = 0;

    for (int j = 1; j < EC_TAPS; j++)
        if (fabsf(ec_w[j]) > fabsf(ec_w[pk])) pk = j;
    *w = ec_w[pk];
    return pk;
}

/* The n samples played, kept. The filter's own strongest tap says where the return is once it has one (|w| over
 * 0.2): the filter is kept centred on it as the delay creeps (a queue's few ms now and then). Till then, and when it
 * loses it, the delay is looked for every 2 s: a clear one moves the filter there, at once the first time, else only
 * when found twice (a stray match is not followed). Every 10 s the log tells what is left of the return: the
 * captured's correlation with the played at the strongest tap, before the filter and after */
static void ec_played(const int16_t *play, const float *stick, int n) {
    for (int k = 0; k < n; k++) {
        uint32_t i = (ec_n + k) & EC_FMASK;

        ec_play[(ec_n + k) & EC_MASK] = play[k];
        ec_pf[i] = ec_pf[i + EC_FH] = play[k];
        ec_sf[i] = ec_sf[i + EC_FH] = stick[k];
    }
    for (int k = 0; k < n; k += 2) {   /* the X6200's as left now against what was played a delay before (the stick's
                                         * own music, its beat at the delay, is not in the X6200's: never taken for it) */
        uint32_t i = ec_n + k;
        double   a = ec_res[i & EC_MASK], b = ec_play[(i - ec_d) & EC_MASK];
        ec_rxy += a * b, ec_rxx += a * a, ec_ryy += b * b;
    }
    ec_n += n;
    static uint32_t qtr;
    if (ec_n - qtr >= 44100 / 4) {      /* the return left in the X6200's (it rings, played again and again): the
                                         * X6200 held back till it is gone, let back in a step at a time after */
        float r = (float)(fabs(ec_rxy) / (sqrt(ec_rxx * ec_ryy) + 1e-9));

        qtr = ec_n;
        if (r > 0.3f && ec_ryy > 22050 / 2 * 300.0 * 300) {
            if (ec_guard > 0.06f && ec_guard * 0.6f <= 0.06f)
                fprintf(stderr, "rtl BOTH echo: its return not taken out (%.2f): the X6200 held back\n", r);
            ec_guard = ec_guard * 0.6f < 0.05f ? 0.05f : ec_guard * 0.6f;
        } else if (r < 0.15f && ec_guard < 1 && (ec_locked || ec_n > 44100 * 4)) {   /* 4 s: the stick quiet,
                                         * nothing played to find the return by; the guard watches */
            ec_guard = ec_guard + 0.1f > 1 ? 1 : ec_guard + 0.1f;
        }
        ec_rxy = ec_rxx = ec_ryy = 0;
    }
    /* A quarter second that left more than came in: the return moved (a queue dropped or added a little, the
     * delay jumped 1 ms) and the filter takes it out where it was. Looked for at once, and the filter, as learnt,
     * put there (its window and weights moved together: the return's shape is the same); started over only when
     * it is not found for 2 s */
    static uint32_t qsec;
    static int      bad;
    if (ec_n - qsec >= 44100 / 4) {
        qsec = ec_n;
        if ((ec_pout > 1.5f * ec_pin && ec_pin > 0) || !isfinite(ec_pout)) {
            float wp;
            int   d = ec_find(), at = ec_d - EC_PRE + EC_TAPS - 1 - ec_peak(&wp);

            /* any distance: the delay differs from one start to the next (476, 521 ms), the saved filter with it */
            if (isfinite(ec_pout) && d > EC_TAPS && ec_found > 0.2f && abs(d - at) >= 2) {
                fprintf(stderr, "rtl BOTH echo: the return moved %+.2f ms (to %.1f): the filter put there\n",
                        (d - at) / 44.1, d / 44.1);
                ec_d += d - at;
                ec_locked = true;
                bad = 0;
            } else if (++bad >= 8 || !isfinite(ec_pout)) {
                fprintf(stderr, "rtl BOTH echo: the filter added more than it took (%.1f dB): started over\n",
                        10 * log10f(ec_pout / ec_pin));
                memset(ec_w, 0, sizeof(ec_w));
                bad = 0;
            }
        } else {
            bad = 0;
        }
        ec_pin = ec_pout = 0;
    }

    float w;
    int   pk = ec_peak(&w), lag = ec_d - EC_PRE + EC_TAPS - 1 - pk;
    bool  held = fabsf(w) > 0.2f;

    if (held && abs(lag - ec_d) > EC_PRE / 4) ec_move(lag);    /* put back on it */
    if (ec_n >= ec_next) {
        ec_next = ec_n + 44100 * 2;
        if (held) {                     /* a small jump, found twice: the filter put there as it is */
            int d = ec_find();

            if (d > EC_TAPS && ec_found > 0.2f && abs(d - lag) < 2) {
                ec_locked = true;       /* found where it is */
                ec_cand   = -1;
            } else if (d > EC_TAPS && ec_found > 0.3f && abs(d - lag) >= 2) {
                /* the first since BOTH started at once, at any distance (the saved filter is where the last start
                 * had it: 476 to 521 ms); after that only when found twice */
                if (!ec_locked || (ec_cand >= 0 && abs(d - ec_cand) <= 1)) {
                    fprintf(stderr, "rtl BOTH echo: the return moved %+.2f ms (to %.1f): the filter put there\n",
                            (d - lag) / 44.1, d / 44.1);
                    ec_d += d - lag;
                    ec_cand   = -1;
                    ec_locked = true;
                } else {
                    ec_cand = d;
                }
            } else {
                ec_cand = -1;
            }
        } else {
            int d = ec_find();

            if (d > EC_TAPS && ec_found > 0.2f) {
                if (!ec_locked || (ec_cand >= 0 && abs(d - ec_cand) <= 2)) {
                    fprintf(stderr, "rtl BOTH echo: its delay %.1f ms (was %.1f), corr %.2f\n", d / 44.1, ec_d / 44.1,
                            ec_found);
                    ec_move(d);
                    ec_locked = true;
                    ec_cand   = -1;
                } else {
                    ec_cand = d;
                }
            }
        }
    }
    static uint32_t said;
    if (ec_n - said >= 44100 * 10 && !unlink("/tmp/ec_dump")) {   /* asked for: the last 3 s as they were, to look
                                                                     * at off the radio (ec_play.raw, ec_cap.raw, ec_res.raw) */
        static const char *nm[] = {"/tmp/ec_play.raw", "/tmp/ec_cap.raw", "/tmp/ec_res.raw"};
        const int16_t     *bf[] = {ec_play, ec_cap, ec_res};

        for (int f = 0; f < 3; f++) {
            FILE *o = fopen(nm[f], "wb");
            if (!o) continue;
            uint32_t h = ec_n & EC_MASK;
            fwrite(bf[f] + h, 2, EC_HIST - h, o);
            fwrite(bf[f], 2, h, o);
            fclose(o);
        }
    }
    if (ec_n - said >= 44100 * 10) {
        uint32_t a  = ec_n - EC_WIN;
        static int saves;
        double   e0 = 0, e16 = 0;

        said = ec_n;
        pk   = ec_peak(&w);
        lag  = ec_d - EC_PRE + EC_TAPS - 1 - pk;
        for (int j = 0; j < EC_TAPS; j++) {
            e0 += ec_w[j] * ec_w[j];
            if (abs(j - pk) <= 16) e16 += ec_w[j] * ec_w[j];
        }
        float cr = ec_corr(ec_cap, a, EC_WIN / 2, lag, 1), cl = ec_corr(ec_res, a, EC_WIN / 2, lag, 1);
        fprintf(stderr, "rtl BOTH echo: at %.1f ms (w %.2f, %.0f%% of the filter within 16 taps) corr %.2f raw, %.2f "
                "left; the X6200 let through %.2f\n", lag / 44.1, w, 100 * e16 / (e0 + 1e-12), cr, cl, ec_guard);
        /* kept for the next start when it holds the return and takes it out (each 30 s at most) */
        if (++saves % 3 == 0 && fabsf(w) > 0.2f && fabsf(cl) < 0.15f && fabsf(cr) > 0.2f) ec_save();
    }
}

/* The audio to the speaker while listening: one thread for good, that never has to be waited for. It takes the
 * speaker when listening starts (the base in play mode, as the recorder plays; or the Bluetooth headphones when they
 * stand in for it) and gives it back when it ends. audio_play() waits for room in CHUNK at most, so the codec paces
 * it; kept under half a second behind, as the stick's clock and the codec's differ a little */
static void *player_thread(void *arg) {
    static int16_t chunk[CHUNK], rx[CHUNK];
    bool           bt = false, tapping = false;
    float          g  = 1;          /* the squelch's level, 0 shut: eased over 10 ms, no click */
    bool           st_run = false;  /* BOTH: the stick's buffer filled, taken from (ST_FILL) */
    int            st_dry = 0, st_low = INT32_MAX;     /* this 10 s: the times it ran dry, its lowest fill */
    uint32_t       st_said = 0;
    (void)arg;
    pthread_setname_np(pthread_self(), "rtl_player");   /* by name in top -H */

    for (;;) {
        bool want_play = atomic_load(&listen_mode) != 0 || atomic_load(&mute);

        if (want_play && !atomic_load(&playing)) {
            bt = audio_play_bt_begin();
            if (!bt) radio_set_play_mode(AUDIO_PLAY_ON);
            atomic_store(&playing, true);
        } else if (!want_play && atomic_load(&playing)) {
            audio_play_flush();
            if (bt) audio_play_bt_end();
            else radio_set_play_mode(AUDIO_PLAY_OFF);
            bt = false;
            atomic_store(&playing, false);
        }
        if (want_play && atomic_load(&mux) != tapping) {
            tapping = !tapping;
            audio_rx_tap(tapping);
            if (tapping) ec_reset();    /* BOTH again: its echo's canceller from the start */
            st_run = false;
        } else if (!want_play && tapping) {
            tapping = false;
            audio_rx_tap(false);
        }
        if (!want_play || !atomic_load(&listen_mode)) {     /* nothing, or OFF: the speaker silent */
            usleep(20000);
            continue;
        }
        if (tapping) {                  /* BOTH: the X6200's samples pace it, the stick's added from its buffer */
            int n = (int)audio_rx_tap_read(rx, CHUNK), m = 0;

            if (!n) {
                usleep(5000);
                continue;
            }
            static uint32_t tone_look;
            if (lv_tick_or_ms() - tone_look >= 1000) {
                tone_look = lv_tick_or_ms();
                atomic_store(&tones, access("/tmp/both_tones", F_OK) == 0 ? 1 : access("/tmp/both_noise", F_OK) == 0 ? 2 : 0);
            }
            if (atomic_load(&tones)) {          /* the test's tone, or noise, on the X6200's line */
                for (int k = 0; k < n; k++) {
                    long v = rx[k];

                    if (atomic_load(&tones) == 1) {
                        v += lrint(TONE_X62_A * sin(tone_ph_x62));
                        tone_ph_x62 = fmod(tone_ph_x62 + 2 * M_PI * TONE_X62_HZ / 44100.0, 2 * M_PI);
                    } else {
                        v += lrint(4000 * noise_next(&noise_x6, &nlp_x6, &nhp_x6, &nx_x6));
                    }
                    rx[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
            }
            /* The stick's audio comes 50 ms a message, in clumps; the X6200's as the codec sends it. Taken as the
             * X6200's came, the stick's ran dry between its clumps and each gap went out as silence (A with BOTH
             * choppy where B alone was clean, 2026-11-14): it fills to ST_FILL first and is taken from then, filled
             * again if it runs dry. Its clock is the stick's, a little off the codec's: past ST_SLACK from ST_FILL a
             * sample is dropped, or said twice, at a pass */
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
                    chunk[m]  = ring[ring_head];
                    ring_head = (ring_head + 1) % RING;
                    ring_len--;
                }
                if (m && m == take && take < n) chunk[m] = chunk[m - 1], m++;
                if (m < n) st_run = false, st_dry++;
                if (ring_len < st_low) st_low = ring_len;
            }
            pthread_mutex_unlock(&ring_lock);
            if (lv_tick_or_ms() - st_said >= 10000) {
                if (st_dry || st_low < 44100 / 20 || atomic_load(&tones))
                    fprintf(stderr, "rtl BOTH stick buffer: its low %d ms, ran dry %d times in 10 s\n",
                            st_low == INT32_MAX ? -1 : st_low * 10 / 441, st_dry);
                st_said = lv_tick_or_ms(), st_dry = 0, st_low = INT32_MAX;
            }
            static float rxc[CHUNK];    /* the X6200's, the base's copy of what was played taken out (ec_cancel) */
            ec_cancel(rx, rxc, n);
            float to = atomic_load(&tones) || !atomic_load(&sql_db) || atomic_load(&sql_open) ? 1.0f : 0.0f;
            float ks = atomic_load(&g_stick) / 1000.0f, kx = atomic_load(&g_x62) / 1000.0f;

            static float stk[CHUNK];    /* the stick's part, for the canceller to learn from */
            /* Each as it is, at its own page's volume (hear_vol_refresh: the speaker at the louder, the other's share
             * under 1): the stick as on B, the X6200 as the base sends it. Nothing raised or levelled; a soft knee
             * from 3/4 of full scale only where the sum would clip */
            for (int k = 0; k < n; k++) {
                if (g < to) g = g + 1.0f / 441 > to ? to : g + 1.0f / 441;
                else if (g > to) g = g - 1.0f / 441 < to ? to : g - 1.0f / 441;
                stk[k]  = k < m ? chunk[k] * g * ks : 0;
                float v = stk[k] + rxc[k] * kx * ec_guard, a = fabsf(v);

                if (a > 24000) v = (v > 0 ? 1 : -1) * (24000 + (a - 24000) * 8767 / (a - 24000 + 8767));
                chunk[k] = (int16_t)v;
            }
            ec_played(chunk, stk, n);
            audio_play(chunk, n);
            continue;
        }

        int n = 0;
        pthread_mutex_lock(&ring_lock);
        if (ring_len > 44100 / 2) {             /* behind: catch up */
            ring_head = (ring_head + ring_len - 44100 / 5) % RING;
            ring_len  = 44100 / 5;
        }
        if (ring_len >= CHUNK) {
            for (; n < CHUNK; n++) chunk[n] = ring[(ring_head + n) % RING];
            ring_head = (ring_head + CHUNK) % RING;
            ring_len -= CHUNK;
        }
        pthread_mutex_unlock(&ring_lock);
        if (n) {
            float to = !atomic_load(&sql_db) || atomic_load(&sql_open) ? 1.0f : 0.0f;

            for (int k = 0; k < n; k++) {
                if (g < to) g = g + 1.0f / 441 > to ? to : g + 1.0f / 441;
                else if (g > to) g = g - 1.0f / 441 < to ? to : g - 1.0f / 441;
                chunk[k] = (int16_t)(chunk[k] * g);
            }
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
        pthread_mutex_unlock(&ring_lock);
    }
    snprintf(cmd, sizeof(cmd), "l %lld %d\n", (long long)(mode ? hz : 0), mode);
    say(cmd);
    if (mode && !player_started) {
        player_started = pthread_create(&player, NULL, player_thread, NULL) == 0;
        if (player_started) pthread_detach(player);
    }
}

bool rtl_spectrum_listening(void) {
    return atomic_load(&listen_mode) != 0;
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
