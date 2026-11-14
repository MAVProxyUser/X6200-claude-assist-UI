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

/* The audio to the speaker while listening: one thread for good, that never has to be waited for. It takes the
 * speaker when listening starts (the base in play mode, as the recorder plays; or the Bluetooth headphones when they
 * stand in for it) and gives it back when it ends. audio_play() waits for room in CHUNK at most, so the codec paces
 * it; kept under half a second behind, as the stick's clock and the codec's differ a little */
static void *player_thread(void *arg) {
    static int16_t chunk[CHUNK], rx[CHUNK];
    bool           bt = false, tapping = false;
    float          g  = 1;          /* the squelch's level, 0 shut: eased over 10 ms, no click */
    float          rx_rms = 500;    /* the X6200's level in the mix, brought up to the stick's (its AGC'd audio) */
    (void)arg;

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
        } else if (!want_play && tapping) {
            tapping = false;
            audio_rx_tap(false);
        }
        if (!want_play || !atomic_load(&listen_mode)) {     /* nothing, or OFF: the speaker silent */
            usleep(20000);
            continue;
        }
        if (tapping) {                  /* BOTH: the X6200's samples pace it, the stick's added as they are there */
            int n = (int)audio_rx_tap_read(rx, CHUNK), m = 0;

            if (!n) {
                usleep(5000);
                continue;
            }
            pthread_mutex_lock(&ring_lock);
            if (ring_len > 44100 / 2) {
                ring_head = (ring_head + ring_len - 44100 / 5) % RING;
                ring_len  = 44100 / 5;
            }
            for (; m < n && ring_len > 0; m++) {
                chunk[m]  = ring[ring_head];
                ring_head = (ring_head + 1) % RING;
                ring_len--;
            }
            pthread_mutex_unlock(&ring_lock);
            double e = 0;
            for (int k = 0; k < n; k++) e += (double)rx[k] * rx[k];
            rx_rms     = 0.97f * rx_rms + 0.03f * (float)sqrt(e / n);
            float gx   = rx_rms > 1 ? 6000 / rx_rms : 1;
            float to   = !atomic_load(&sql_db) || atomic_load(&sql_open) ? 1.0f : 0.0f;

            gx = gx < 1 ? 1 : gx > 16 ? 16 : gx;
            float ks = atomic_load(&g_stick) / 1000.0f, kx = atomic_load(&g_x62) / 1000.0f;

            gx *= kx;
            for (int k = 0; k < n; k++) {
                if (g < to) g = g + 1.0f / 441 > to ? to : g + 1.0f / 441;
                else if (g > to) g = g - 1.0f / 441 < to ? to : g - 1.0f / 441;
                float v = 0.6f * (k < m ? chunk[k] * g * ks : 0) + 0.6f * rx[k] * gx;
                chunk[k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
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
