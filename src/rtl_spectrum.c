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

#include <errno.h>
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
#include <unistd.h>

#define HELPER    "/usr/bin/x6x00-rtlspec"
#define LOG       "/tmp/rtlspec.log"
#define MAX_BINS  8192
#define BINS      2048
#define ROWS      20            /* a second */

static pthread_t       thread;
static bool            started;
static atomic_bool     want;
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

static pid_t spawn(int *in_fd, int *out_fd) {
    int   to[2], from[2];
    char  f[24], g[16], n[16], r[16];
    pid_t p;

    if (pipe2(to, O_CLOEXEC) != 0) return -1;
    if (pipe2(from, O_CLOEXEC) != 0) {
        close(to[0]), close(to[1]);
        return -1;
    }
    snprintf(f, sizeof(f), "%lld", (long long)atomic_load(&want_hz));
    snprintf(g, sizeof(g), "%d", atomic_load(&want_gain));
    snprintf(n, sizeof(n), "%d", BINS);
    snprintf(r, sizeof(r), "%d", ROWS);
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
        execl(HELPER, HELPER, "-f", f, "-g", g, "-n", n, "-r", r, (char *)NULL);
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
    if (p > 0) {
        for (int i = 0; i < 20 && waitpid(p, NULL, WNOHANG) == 0; i++) usleep(50000);
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

        if (!atomic_load(&want) || ms <= 0) return false;
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

static void *spec_thread(void *arg) {
    static uint8_t buf[24 + MAX_BINS * 4];
    (void)arg;

    for (;;) {
        if (!atomic_load(&want)) {
            adsb_rx_lend(false);
            state = RTL_SPEC_OFF;
            usleep(200000);
            continue;
        }
        if (access(HELPER, X_OK) != 0) {
            state = RTL_SPEC_NO_HELPER;
            sleep(2);
            continue;
        }
        adsb_rx_lend(true);             /* AIRSPACE's dump1090 lets the stick go */

        int   in_fd = -1, out_fd = -1;
        pid_t p = spawn(&in_fd, &out_fd);
        char  line[96] = "", tn[16] = "";
        int   ver = 0, nb = 0;
        unsigned rate = 0;

        if (p < 0) {
            sleep(2);
            continue;
        }
        if (state != RTL_SPEC_ON) state = RTL_SPEC_OPENING;
        if (!read_line(out_fd, line, sizeof(line), 8000) ||
            sscanf(line, "RTLSPEC %d %15s %u %d", &ver, tn, &rate, &nb) != 4 || ver != 1 || nb < 64 || nb > MAX_BINS) {
            end_child(p, in_fd, out_fd);
            if (!atomic_load(&want)) continue;
            state = strstr(line, "BUSY") ? RTL_SPEC_BUSY : RTL_SPEC_NO_STICK;
            sleep(state == RTL_SPEC_BUSY ? 1 : 2);
            continue;
        }
        pthread_mutex_lock(&lock);
        snprintf(tuner, sizeof(tuner), "%s", tn);
        cmd_fd = in_fd;
        pthread_mutex_unlock(&lock);

        /* A retune asked for while it started */
        int64_t asked = atomic_load(&want_hz);
        for (;;) {
            size_t len = 24 + (size_t)nb * 4;

            if (!read_all(out_fd, buf, len, 3000) || memcmp(buf, "RSPC", 4) != 0) break;
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
            state = RTL_SPEC_ON;
            if (asked != atomic_load(&want_hz) && (int64_t)c != atomic_load(&want_hz)) {
                char cmd[32];
                asked = atomic_load(&want_hz);
                snprintf(cmd, sizeof(cmd), "f %lld\n", (long long)asked);
                say(cmd);
            }
        }
        end_child(p, in_fd, out_fd);
        if (atomic_load(&want)) {       /* it went: the stick unplugged, most likely */
            state = RTL_SPEC_NO_STICK;
            sleep(2);
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
