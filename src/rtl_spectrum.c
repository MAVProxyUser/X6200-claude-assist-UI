/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An RTL-SDR stick's spectrum (rtl_spectrum.h). A thread of its own starts /usr/bin/x6x00-rtlspec while
 *  the spectrum is wanted, with a pipe each way: its rows come in on one (a text line, then binary frames, see
 *  tools/rtlspec/rtlspec.c), retunes go out on the other. The stick is AIRSPACE's otherwise: it is lent first, and
 *  a helper that finds it still busy (dump1090 not yet gone) is started again a moment later. No stick, or one that
 *  goes away: looked for again every two seconds.
 */

#define _GNU_SOURCE    /* pipe2 */
#include "rtl_spectrum.h"

#include "adsb_rx.h"
#include "audio.h"
#include "radio.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HELPER    "/usr/bin/x6x00-rtlspec"
#define LOG       "/tmp/rtlspec.log"
#define MAX_BINS  8192
#define BINS      2048
#define ROWS      20            /* a second */
#define RATE        2116800     /* 48 x 44.1 kHz: the helper's audio comes out at the codec's rate */
#define RING        (44100 * 2) /* audio waiting to play: two seconds at most */
#define CHUNK       1024        /* audio_play() needs room for the whole write: a bigger one may never find it */

static pthread_t       thread;
static bool            started;
static atomic_bool     want;
static atomic_bool     yielded;          /* AIRSPACE's screen open: its dump1090 has the stick */
static atomic_uint     want_ms;          /* when want or yielded last changed (now_ms) */

/* The stick's stream started and stopped as seldom as can be: the radio's USB host (MUSB) has been left broken for
 * the stick ("THE RTL-SDR SENDS NOTHING") after stops and starts, and once froze the radio. So the Sentry wants it
 * from its first start for as long as the GUI runs, whatever screen or app is shown (F and G only tune it and listen),
 * and it stops only while AIRSPACE's screen is open; it starts START_MS after that closes */
#define START_MS    500

static uint32_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Wanted and not yielded: the helper running stays */
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

/* A write to the helper that cannot end the GUI: one that has just gone gives EPIPE, and its SIGPIPE is taken here,
 * not left to end the process (the GUI keeps the default action) */
static void quiet_write(int fd, const char *s, size_t n) {
    sigset_t        pipe_set, old;
    struct timespec now = {0, 0};

    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    if (write(fd, s, n) < 0 && errno == EPIPE) {
        sigtimedwait(&pipe_set, NULL, &now);
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void say(const char *cmd) {
    pthread_mutex_lock(&lock);
    if (cmd_fd >= 0) quiet_write(cmd_fd, cmd, strlen(cmd));
    pthread_mutex_unlock(&lock);
}

static int64_t spawned_hz;      /* the frequency the helper was started on */

static pid_t spawn(int *in_fd, int *out_fd) {
    int   to[2], from[2];
    char  f[24], g[16], n[16], r[16];
    pid_t p;

    if (pipe2(to, O_CLOEXEC) != 0) return -1;
    if (pipe2(from, O_CLOEXEC) != 0) {
        close(to[0]), close(to[1]);
        return -1;
    }
    spawned_hz = atomic_load(&want_hz);
    snprintf(f, sizeof(f), "%lld", (long long)spawned_hz);
    snprintf(g, sizeof(g), "%d", atomic_load(&want_gain));
    snprintf(n, sizeof(n), "%d", BINS);
    snprintf(r, sizeof(r), "%d", ROWS);
    char s[16];
    snprintf(s, sizeof(s), "%d", RATE);
    if ((p = fork()) < 0) {
        close(to[0]), close(to[1]), close(from[0]), close(from[1]);
        return -1;
    }
    if (p == 0) {                       /* the child: its stdin and stdout the pipes, stderr the log */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int log = open(LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(to[0], 0);
        dup2(from[1], 1);
        if (log >= 0) dup2(log, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl(HELPER, HELPER, "-f", f, "-g", g, "-n", n, "-r", r, "-s", s, (char *)NULL);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    *in_fd  = to[1];
    *out_fd = from[0];
    return p;
}

static void end_child(pid_t p, int in_fd, int out_fd) {
    pthread_mutex_lock(&lock);
    cmd_fd = -1;
    pthread_mutex_unlock(&lock);
    if (in_fd >= 0) {
        quiet_write(in_fd, "q\n", 2);
        close(in_fd);
    }
    if (out_fd >= 0) close(out_fd);
    /* Time to close the stick: a helper killed with transfers in flight leaves the radio's USB host (MUSB) broken
     * for the stick until it is plugged in again, and once froze the radio. SIGTERM after 2 s (it cancels them
     * too), SIGKILL only after 6 */
    if (p > 0) {
        for (int i = 0; i < 40 && waitpid(p, NULL, WNOHANG) == 0; i++) usleep(50000);
        if (waitpid(p, NULL, WNOHANG) == 0) kill(p, SIGTERM);
        for (int i = 0; i < 80 && waitpid(p, NULL, WNOHANG) == 0; i++) usleep(50000);
        if (waitpid(p, NULL, WNOHANG) == 0) {
            kill(p, SIGKILL);
            waitpid(p, NULL, 0);
        }
    }
}

/* n bytes, with up to ms in all for them: false at the end of the pipe, the time, or when no longer wanted */
static bool read_all(int fd, void *buf, size_t n, int ms) {
    uint8_t *b = buf;

    while (n > 0) {
        fd_set         rd;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};

        if (!kept() || ms <= 0) return false;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        int r = select(fd + 1, &rd, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return false;
        if (r == 0) {
            ms -= 200;
            continue;
        }
        ssize_t k = read(fd, b, n);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return false;
        b += k;
        n -= (size_t)k;
    }
    return true;
}

static bool read_line(int fd, char *s, size_t n, int ms) {
    size_t i = 0;

    while (i + 1 < n) {
        if (!read_all(fd, s + i, 1, ms)) {
            s[i] = 0;
            return false;
        }
        if (s[i] == '\n') break;
        i++;
    }
    s[i] = 0;
    return true;
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

/* A helper still running: the GUI before this one, restarted (a hot install, an update), its helper closing the stick */
static bool helper_running(void) {
    DIR           *d = opendir("/proc");
    struct dirent *e;
    bool           found = false;

    while (d && !found && (e = readdir(d))) {
        char p[64], comm[32] = "";
        FILE *f;

        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(p, sizeof(p), "/proc/%s/comm", e->d_name);
        if ((f = fopen(p, "r"))) {
            if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
            fclose(f);
        }
        found = !strcmp(comm, "x6x00-rtlspec");
    }
    if (d) closedir(d);
    return found;
}

static void *spec_thread(void *arg) {
    static uint8_t buf[24 + MAX_BINS * 4];
    (void)arg;

    for (;;) {
        if (!kept()) {
            adsb_rx_lend(false);
            state = RTL_SPEC_OFF;
            usleep(200000);
            continue;
        }
        if (now_ms() - atomic_load(&want_ms) < START_MS) {     /* AIRSPACE only just closed */
            usleep(100000);
            continue;
        }
        if (!adsb_rx_sdr_present()) {   /* no stick: looked for each second, no helper started for nothing */
            adsb_rx_lend(true);         /* and dump1090 kept off it, should one come */
            state = RTL_SPEC_NO_STICK;
            usleep(1000000);
            continue;
        }
        if (access(HELPER, X_OK) != 0) {
            state = RTL_SPEC_NO_HELPER;
            sleep(2);
            continue;
        }
        adsb_rx_lend(true);             /* AIRSPACE's dump1090 lets the stick go */
        /* and is gone, its transfers cancelled and the stick closed (up to 8 s, adsb_rx.c d1090_stop), a moment before
         * the helper opens it: opened while dump1090 still had transfers in flight, the stick sent nothing */
        bool waited = false;
        for (int i = 0; i < 100; i++) {
            adsb_rx_state_t a = adsb_rx_state(NULL, 0);
            if (a != ADSB_RX_ON && a != ADSB_RX_OPENING) break;
            waited = true;
            usleep(100000);
        }
        if (waited) usleep(1000000);

        /* none of ours runs here: one still running is the GUI's before this one, closing the stick; opened over it,
         * the stick is left sending nothing. It goes first (it ends with its GUI: PR_SET_PDEATHSIG) */
        for (int i = 0; i < 80 && helper_running(); i++) usleep(100000);

        int   in_fd = -1, out_fd = -1;
        pid_t p = spawn(&in_fd, &out_fd);
        char  line[96] = "", tn[16] = "";
        int   ver = 0, nb = 0;
        unsigned rate = 0;

        if (p < 0) {
            sleep(2);
            continue;
        }
        if (state != RTL_SPEC_STALLED) state = RTL_SPEC_OPENING;
        if (!read_line(out_fd, line, sizeof(line), 8000) ||
            sscanf(line, "RTLSPEC %d %15s %u %d", &ver, tn, &rate, &nb) != 4 || ver != 1 || nb < 64 || nb > MAX_BINS) {
            end_child(p, in_fd, out_fd);
            if (!kept()) continue;
            state = strstr(line, "BUSY") ? RTL_SPEC_BUSY : RTL_SPEC_NO_STICK;
            sleep(state == RTL_SPEC_BUSY ? 1 : 2);
            continue;
        }
        pthread_mutex_lock(&lock);
        snprintf(tuner, sizeof(tuner), "%s", tn);
        cmd_fd = in_fd;
        pthread_mutex_unlock(&lock);
        if (atomic_load(&watch_hz)) {           /* a new helper: the channel watched, again */
            char cmd[40];
            snprintf(cmd, sizeof(cmd), "w %lld\n", (long long)atomic_load(&watch_hz));
            say(cmd);
        }
        if (atomic_load(&listen_mode)) {        /* a new helper: what was listened to, again */
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "l %lld %d\n", (long long)atomic_load(&listen_hz), atomic_load(&listen_mode));
            say(cmd);
        }

        /* What it was started on: a retune asked for while it started (before it took commands) is sent below */
        int64_t asked = spawned_hz;
        for (;;) {
            size_t len = 24 + (size_t)nb * 4;

            if (!read_all(out_fd, buf, 4, 3000)) break;
            if (memcmp(buf, "RAUD", 4) == 0) {      /* audio: into the ring, the oldest out when it is full */
                uint32_t cnt;
                static int16_t a[44100];

                if (!read_all(out_fd, &cnt, 4, 3000) || cnt > 44100 || !read_all(out_fd, a, cnt * 2, 3000)) break;
                pthread_mutex_lock(&ring_lock);
                for (uint32_t k = 0; k < cnt; k++) {
                    ring[(ring_head + ring_len) % RING] = a[k];
                    if (ring_len < RING) ring_len++;
                    else ring_head = (ring_head + 1) % RING;
                }
                pthread_mutex_unlock(&ring_lock);
                continue;
            }
            if (memcmp(buf, "RSAM", 4) == 0) {      /* a SAME header decided: kept, and in the log */
                uint32_t n;
                char     t[300];

                if (!read_all(out_fd, &n, 4, 3000) || n >= sizeof(t) || !read_all(out_fd, t, n, 3000)) break;
                t[n] = 0;
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
                continue;
            }
            if (memcmp(buf, "RSPC", 4) != 0 || !read_all(out_fd, buf + 4, len - 4, 3000)) break;
            uint32_t seq, r;
            uint64_t c;
            uint16_t n, flags;

            memcpy(&seq, buf + 4, 4);
            memcpy(&c, buf + 8, 8);
            memcpy(&r, buf + 16, 4);
            memcpy(&n, buf + 20, 2);
            memcpy(&flags, buf + 22, 2);
            if (n != nb) break;
            pthread_mutex_lock(&lock);
            memcpy(row, buf + 24, (size_t)n * 4);
            row_n      = n;
            row_rate   = r;
            row_centre = (int64_t)c;
            row_seq++;
            row_fresh  = flags & 1;
            pthread_mutex_unlock(&lock);
            squelch_row(row, n, r, (int64_t)c);     /* row is only written here: read without the lock */
            state = RTL_SPEC_ON;
            if (asked != atomic_load(&want_hz) && (int64_t)c != atomic_load(&want_hz)) {
                char cmd[32];
                asked = atomic_load(&want_hz);
                snprintf(cmd, sizeof(cmd), "f %lld\n", (long long)asked);
                say(cmd);
            }
        }
        bool got_rows = state == RTL_SPEC_ON;
        end_child(p, in_fd, out_fd);
        if (kept()) {                   /* it went: unplugged, or the stream stalled */
            static int stalls;

            stalls = got_rows ? 0 : stalls + 1;
            if (stalls < 2) {
                state = RTL_SPEC_NO_STICK;
                sleep(2);
                continue;
            }
            /* Opened twice and nothing sent: the USB host's endpoint is broken, retries would not mend it, and the
             * stick pulled out while a helper had transfers queued on it has hung the radio. Nor may the radio unplug
             * it in software: de-authorizing the stick crashed the MUSB driver (2026-11-14). So nothing touches it
             * until it is another device: unplugged and plugged in again (or another stick) */
            int id = adsb_rx_sdr_id();

            state = RTL_SPEC_STALLED;
            while (kept() && id && adsb_rx_sdr_id() == id) sleep(1);
            stalls = 0;
        }
    }
    return NULL;
}

void rtl_spectrum_want(bool on, int64_t hz, int gain) {
    if (hz < RTL_SPEC_MIN_HZ) hz = RTL_SPEC_MIN_HZ;
    if (hz > RTL_SPEC_MAX_HZ) hz = RTL_SPEC_MAX_HZ;
    atomic_store(&want_hz, hz);
    atomic_store(&want_gain, gain);
    if (on != atomic_load(&want)) atomic_store(&want_ms, now_ms());
    atomic_store(&want, on);
    if (on && !started) {
        started = pthread_create(&thread, NULL, spec_thread, NULL) == 0;
        if (started) pthread_detach(thread);
    }
}

void rtl_spectrum_yield(bool on) {
    if (on != atomic_load(&yielded)) atomic_store(&want_ms, now_ms());
    atomic_store(&yielded, on);
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
