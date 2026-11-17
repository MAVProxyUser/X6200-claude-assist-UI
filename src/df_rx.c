/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Page I's link to the direction finder x6x00-df (df_rx.h). Modelled on rtl_spectrum.c's link to
 *  x6x00-rtlspec: an abstract SOCK_SEQPACKET socket, the daemon started when none answers (it goes into the
 *  background itself), commands never waited for. Unlike B's daemon this one runs only while page I is open: the
 *  KrakenSDR is everyone else's the rest of the time.
 */

#define _GNU_SOURCE
#include "df_rx.h"

#include "gui_state.h"
#include "rtl_spectrum.h"
#include "sdr_pool.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
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

#define HELPER     "/usr/bin/x6x00-df"
#define LOG        "/tmp/x6x00-df.log"
#define SYNTH_FILE "/mnt/df_synth"
#define SYNTH_ARG  "40,3"       /* the test source: a transmitter at 40 degrees, turning 3 degrees a second */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       thread;
static bool            started;
static atomic_bool     want;
static int             cmd_fd = -1;
static df_snap_t       snap;
static uint8_t         hist[DF_RX_HIST][360];
static uint8_t         hist_gate[DF_RX_HIST];
static char            kept[128][160];      /* the last command of each letter, said again to a new daemon */
static int             b_lent;              /* 0, 1 moved, 2 lent */
static rtl_dev_t       b_prev = RTL_DEV_NONE;
static int             b_prev_rtl;

static uint32_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool df_rx_synth(void) {
    return gui_state_get("df.synth", 0) != 0 || access(SYNTH_FILE, F_OK) == 0;
}

void df_rx_set_synth(bool on) {
    gui_state_set("df.synth", on);
}

bool df_rx_present(void) {
    return sdr_pool_array_count() >= 3 || df_rx_synth();
}

/* ---- The daemon ---- */

static void say_locked(const char *cmd) {
    if (cmd_fd >= 0) (void)send(cmd_fd, cmd, strlen(cmd), MSG_DONTWAIT | MSG_NOSIGNAL);
}

void df_rx_cmd(const char *fmt, ...) {
    char    c[160];                 /* a custom array of five is about 80 */
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(c, sizeof(c), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lock);
    unsigned char k = (unsigned char)c[0];
    if (k < 128 && k != 'c' && k != 'Q') snprintf(kept[k], sizeof(kept[k]), "%s", c);
    say_locked(c);
    pthread_mutex_unlock(&lock);
}

static int daemon_connect(void) {
    struct sockaddr_un a   = {.sun_family = AF_UNIX};
    socklen_t          len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(DF_SOCK));
    int                fd  = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (fd < 0) return -1;
    memcpy(a.sun_path + 1, DF_SOCK, strlen(DF_SOCK));
    if (connect(fd, (struct sockaddr *)&a, len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Started: it goes into the background at once (the child waited for here is gone in a moment) */
static void daemon_start(void) {
    pid_t p;
    bool  synth = df_rx_synth();

    if ((p = fork()) < 0) return;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR), log = open(LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
        if (log >= 0) dup2(log, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        if (synth) execl(HELPER, HELPER, "-T", SYNTH_ARG, (char *)NULL);
        else execl(HELPER, HELPER, (char *)NULL);
        _exit(127);
    }
    waitpid(p, NULL, 0);
}

static void take(const uint8_t *b, ssize_t n) {
    pthread_mutex_lock(&lock);
    if (n >= (ssize_t)sizeof(df_st_msg_t) && !memcmp(b, "DFST", 4)) {
        memcpy(&snap.st, b, sizeof(snap.st));
        snap.have_st = true;
        snap.st_ms   = now_ms() | 1;
    } else if (n >= (ssize_t)sizeof(df_do_msg_t) && !memcmp(b, "DFDO", 4)) {
        memcpy(&snap.dd, b, sizeof(snap.dd));
        snap.have_do = true;
        memcpy(hist[snap.do_n % DF_RX_HIST], snap.dd.p, 360);
        hist_gate[snap.do_n % DF_RX_HIST] = snap.dd.gate;
        snap.do_n++;
    } else if (n >= (ssize_t)offsetof(df_sp_msg_t, db) && !memcmp(b, "DFSP", 4)) {
        memset(&snap.sp, 0, sizeof(snap.sp));
        memcpy(&snap.sp, b, (size_t)n < sizeof(snap.sp) ? (size_t)n : sizeof(snap.sp));
        if (snap.sp.n > DF_PROTO_SPEC) snap.sp.n = DF_PROTO_SPEC;
        snap.have_sp = true;
    } else if (n >= (ssize_t)sizeof(df_tg_msg_t) && !memcmp(b, "DFTG", 4)) {
        memcpy(&snap.tg, b, sizeof(snap.tg));
        snap.tg_ms = now_ms() | 1;
    } else if (n >= (ssize_t)sizeof(df_gp_msg_t) && !memcmp(b, "DFGP", 4)) {
        memcpy(&snap.gp, b, sizeof(snap.gp));
        snap.have_gp = true;
    }
    pthread_mutex_unlock(&lock);
}

static void *df_thread(void *arg) {
    static uint8_t buf[4096];
    int            fd = -1, failed = 0;
    (void)arg;

    for (;;) {
        if (!atomic_load(&want)) {      /* page I closed: the daemon told to end, nothing started */
            if (fd >= 0) {
                (void)send(fd, "Q", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
                pthread_mutex_lock(&lock);
                cmd_fd = -1;
                snap.connected = false;
                pthread_mutex_unlock(&lock);
                close(fd);
                fd = -1;
            }
            failed = 0;
            usleep(200000);
            continue;
        }
        if (fd < 0) {
            if ((fd = daemon_connect()) < 0 && access(HELPER, X_OK) == 0) {
                daemon_start();
                for (int i = 0; i < 30 && atomic_load(&want) && (fd = daemon_connect()) < 0; i++) usleep(100000);
            }
            if (fd < 0) {
                sleep(failed < 4 ? 1 << failed : 10);
                failed++;
                continue;
            }
            failed = 0;
            pthread_mutex_lock(&lock);
            cmd_fd         = fd;
            snap.connected = true;
            /* the settings again, in an order that keeps the daemon from tuning twice; every letter page I sends
             * (a fresh daemon had only the first nine: sources, FB, smoothing, the log, the heading and the other
             * targets never reached it) */
            static const char order[] = "sgftamqixkwolh123";
            for (const char *o = order; *o; o++)
                if (kept[(unsigned char)*o][0]) say_locked(kept[(unsigned char)*o]);
            pthread_mutex_unlock(&lock);
        }
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 200);
        ssize_t       n;

        if (r == 0 || (r < 0 && errno == EINTR)) continue;
        n = r > 0 ? recv(fd, buf, sizeof(buf), 0) : -1;
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) {                   /* the daemon went: connected again, or started again */
            pthread_mutex_lock(&lock);
            cmd_fd         = -1;
            snap.connected = false;
            pthread_mutex_unlock(&lock);
            close(fd);
            fd = -1;
            sleep(1);
            continue;
        }
        take(buf, n);
    }
    return NULL;
}

/* ---- B and the pool ---- */

/* Another SDR B could go to (not a stick of the Kraken, nobody else's, not stopped): its place in u, -1 none */
static int other_sdr(const sdr_unit_t *u, int n) {
    for (int i = 0; i < n; i++)
        if (sdr_pool_array_ch(&u[i]) < 0 && !u[i].stopped && u[i].owner == SDR_OWN_NONE) return i;
    return -1;
}

void df_rx_open(void) {
    sdr_unit_t b, u[SDR_POOL_UNITS];
    int        n = sdr_pool_units(u, SDR_POOL_UNITS);

    b_lent = 0;
    if (!df_rx_synth() && sdr_pool_held(SDR_OWN_B, &b) >= 0 && sdr_pool_array_ch(&b) >= 0) {
        int o = other_sdr(u, n);

        b_prev     = rtl_spectrum_used();
        b_prev_rtl = rtl_spectrum_used_rtl();
        if (o >= 0) {                   /* B to the other SDR while page I is open, back after */
            if (u[o].kind == RTL_DEV_RTL) rtl_spectrum_use_rtl(u[o].rtl);
            else rtl_spectrum_use(u[o].kind);
            b_lent = 1;
        } else {                        /* nothing else: B's daemon ended till page I closes */
            rtl_spectrum_lend_df(true);
            b_lent = 2;
        }
    }
    if (!df_rx_synth()) sdr_pool_want(SDR_OWN_DF, true);
    pthread_mutex_lock(&lock);
    memset(&snap, 0, sizeof(snap));
    pthread_mutex_unlock(&lock);
    atomic_store(&want, true);
    if (!started) started = pthread_create(&thread, NULL, df_thread, NULL) == 0;
}

void df_rx_close(void) {
    atomic_store(&want, false);
    sdr_pool_want(SDR_OWN_DF, false);
    if (b_lent == 2) rtl_spectrum_lend_df(false);
    if (b_lent) {                       /* B's own choice again */
        if (b_prev == RTL_DEV_RTL) rtl_spectrum_use_rtl(b_prev_rtl);
        else rtl_spectrum_use(b_prev);
    }
    b_lent = 0;
}

int df_rx_b_lent(void) {
    return b_lent;
}

bool df_rx_snapshot(df_snap_t *s) {
    pthread_mutex_lock(&lock);
    *s = snap;
    pthread_mutex_unlock(&lock);
    return s->connected;
}

bool df_rx_hist(uint32_t n, uint8_t *p, uint8_t *gate) {
    bool ok;

    pthread_mutex_lock(&lock);
    ok = n < snap.do_n && snap.do_n - n <= DF_RX_HIST;
    if (ok) {
        memcpy(p, hist[n % DF_RX_HIST], 360);
        if (gate) *gate = hist_gate[n % DF_RX_HIST];
    }
    pthread_mutex_unlock(&lock);
    return ok;
}
