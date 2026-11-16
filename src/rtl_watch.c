/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Watch channels (rtl_watch.h). The user, 2026-11-16, after five RTLs streamed at once on the radio
 *  through a KrakenSDR: "Tuners at once", then "Yes, build them". Each channel has an RTL of its own: one on a hub of
 *  its own power (the Kraken's), that B neither holds nor asked for, taken from the last (RTL4, RTL3...) so
 *  AIRSPACE's first spare keeps the first free one; AIRSPACE leaves them (adsb_rx_rtl_claim).
 *
 *  Each is an x6x00-rtlspec of its own (-W: that RTL alone, under the socket "x6x00-rtlspec-w<i>"), at 1.0584 MS/s
 *  (4 x 264.6 kHz, what its audio chain takes), tuned W_OFF over the channel so the tuner's own spike at its middle
 *  stays out of it, small rows (W_BINS, W_ROWS a second) for the squelch, decided here as B's is (rtl_spectrum.c:
 *  the channel's peak over the row's median), and its audio ("RAUD", 44.1 kHz) into a ring of its own. The player
 *  (rtl_spectrum.c) adds the open ones into what it plays and takes the speaker while one is open: then the base
 *  sends none of the X6200's receiver, as for B (scanner priority), and gives it back a moment after they shut.
 *  One thread per channel, never the GUI's.
 */

#include "rtl_watch.h"

#include "adsb_rx.h"
#include "board.h"
#include "rtl_spectrum.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
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

#define WATCH_FILE BOARD_DATA "/rtl_watch"
#define HELPER     "/usr/bin/x6x00-rtlspec"
#define PROTO      2
#define W_RATE     1058400
#define W_BINS     256
#define W_ROWS     10
#define W_OFF      250000                   /* the device this far over the channel: off its middle's spike */
#define W_RING     (44100 / 2)              /* audio waiting: half a second at most */
#define W_KEEP     (44100 / 5)              /* trimmed to 0.2 s when it runs over: the RTL's clock and the codec's */
#define W_HANG     10                       /* rows (a second) under the squelch before it shuts */
#define W_ACTIVE   1500                     /* ms the speaker stays taken after the last one shut */
#define MAX_ROW    1024

typedef struct {
    int64_t         hz;
    int             mode, sql, gain;
    atomic_int      place;                  /* RTL<n> it has, -1 none */
    atomic_bool     up, open;
    atomic_int      over10;
    atomic_uint     gen;                    /* its settings changed: said again to its daemon */
    pthread_mutex_t lock;                   /* the ring */
    int16_t         ring[W_RING];
    int             head, len;
    float           g;                      /* the fade, the player's */
} watch_t;

static watch_t         w[RTL_WATCH_N];
static int             n_w;
static pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint     last_open_ms;

static uint32_t now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void save(void) {
    FILE *f = fopen(WATCH_FILE ".new", "w");

    if (!f) return;
    for (int i = 0; i < n_w; i++) fprintf(f, "%lld %d %d %d\n", (long long)w[i].hz, w[i].mode, w[i].sql, w[i].gain);
    fclose(f);
    rename(WATCH_FILE ".new", WATCH_FILE);
}

/* Which RTL each channel has, said to AIRSPACE: from the last place down, one of its own power, not B's (held or
 * asked for). Under cfg_lock */
static void alloc(void) {
    int      n = adsb_rx_rtl_count(), held = rtl_spectrum_dev_rtl();
    int      want = rtl_spectrum_used() == RTL_DEV_RTL ? rtl_spectrum_used_rtl() : -1, next = n - 1;
    unsigned claim = 0;

    for (int i = 0; i < RTL_WATCH_N; i++) {
        int p = -1;

        if (i < n_w) {
            while (next >= 0 && (next == held || next == want || !adsb_rx_rtl_self(next))) next--;
            if (next >= 0) p = next--;
        }
        if (p >= 0) claim |= 1u << p;
        atomic_store(&w[i].place, p);
    }
    adsb_rx_rtl_claim(claim);
}

int rtl_watch_free(void) {
    int n = adsb_rx_rtl_count(), held = rtl_spectrum_dev_rtl(), free = 0;
    int want = rtl_spectrum_used() == RTL_DEV_RTL ? rtl_spectrum_used_rtl() : -1;

    for (int p = 0; p < n; p++)
        if (p != held && p != want && adsb_rx_rtl_self(p)) free++;
    pthread_mutex_lock(&cfg_lock);
    free -= n_w;
    pthread_mutex_unlock(&cfg_lock);
    return free < 0 ? 0 : free;
}

/* ---- One channel's daemon ---- */

static void sock_name(int i, char *s, size_t n) { snprintf(s, n, "x6x00-rtlspec-w%d", i); }

static int w_connect(int i) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    char               name[32];

    sock_name(i, name, sizeof(name));
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name));
    int       fd  = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (fd < 0) return -1;
    memcpy(a.sun_path + 1, name, strlen(name));
    if (connect(fd, (struct sockaddr *)&a, len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Started in the background (it leaves at once, nobody's child), on RTL place, tuned for channel i */
static void w_start(int i, int place, int64_t hz, int gain) {
    char  name[32], d[8], f[24], g[16], nb[8], r[8], s[16], log[40];
    pid_t p;

    sock_name(i, name, sizeof(name));
    snprintf(d, sizeof(d), "%d", place);
    snprintf(f, sizeof(f), "%lld", (long long)(hz + W_OFF));
    snprintf(g, sizeof(g), "%d", gain);
    snprintf(nb, sizeof(nb), "%d", W_BINS);
    snprintf(r, sizeof(r), "%d", W_ROWS);
    snprintf(s, sizeof(s), "%d", W_RATE);
    snprintf(log, sizeof(log), "/tmp/rtlspec-w%d.log", i);
    if ((p = fork()) < 0) return;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR), lg = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if (nul >= 0) dup2(nul, 0), dup2(nul, 1);
        if (lg >= 0) dup2(lg, 2);
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl(HELPER, HELPER, "-W", "-S", name, "-d", d, "-f", f, "-g", g, "-n", nb, "-r", r, "-s", s, (char *)NULL);
        _exit(127);
    }
    waitpid(p, NULL, 0);
}

static void say(int fd, const char *s) {
    if (fd >= 0) (void)send(fd, s, strlen(s), MSG_DONTWAIT | MSG_NOSIGNAL);
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* A row: the channel's peak over the row's median, as B's squelch (rtl_spectrum.c squelch_row) */
static void w_row(watch_t *c, int64_t hz, int mode, int sql, const float *db, uint16_t n, uint32_t rate, int64_t centre,
                  int *quiet) {
    static __thread float sorted[MAX_ROW];
    int64_t half = mode == 3 ? 90000 : mode == 2 ? 4000 : 6000, low = centre - (int64_t)rate / 2;
    double  per  = (double)rate / n;
    int     lo   = (int)((hz - half - low) / per), hi = (int)((hz + half - low) / per);

    if (hi < 0 || lo >= n) return;
    if (lo < 0) lo = 0;
    if (hi >= n) hi = n - 1;
    float peak = -1e9f;
    for (int k = lo; k <= hi; k++)
        if (db[k] > peak) peak = db[k];
    memcpy(sorted, db, (size_t)n * sizeof(float));
    qsort(sorted, n, sizeof(float), cmp_float);
    float over = peak - sorted[n / 2];
    bool  open = atomic_load(&c->open);

    atomic_store(&c->over10, (int)(over * 10));
    if (over >= sql) {
        open   = true;
        *quiet = 0;
    } else if (over >= sql - 3) {
        *quiet = 0;
    } else if (open && ++*quiet >= W_HANG) {
        open = false;
    }
    atomic_store(&c->open, open);
    if (open) atomic_store(&last_open_ms, now_ms());
}

static void *w_thread(void *arg) {
    int          i  = (int)(intptr_t)arg;
    watch_t     *c  = &w[i];
    int          fd = -1, ran = -1, quiet = 0;      /* ran: the place its daemon was started on */
    unsigned     said_gen = ~0u;
    int64_t      hz = 0;                            /* its channel as last said: the thread's own copy */
    int          mode = 0, sql = 10, gain = 0;
    uint32_t     looked = 0;
    static float row[RTL_WATCH_N][MAX_ROW];
    char         buf[16384];

    pthread_setname_np(pthread_self(), "rtl_watch");
    for (;;) {
        if (now_ms() - looked >= 1000) {        /* who has which RTL, each second */
            looked = now_ms();
            pthread_mutex_lock(&cfg_lock);
            alloc();
            pthread_mutex_unlock(&cfg_lock);
        }
        int place = atomic_load(&c->place);

        if (place != ran) {                     /* none now, or another RTL: its daemon ends (it lets the RTL go) */
            if (fd < 0) fd = w_connect(i);
            say(fd, "Q\n");
            if (fd >= 0) close(fd);
            fd  = -1;
            ran = -1;
            atomic_store(&c->up, false);
            atomic_store(&c->open, false);
            if (place < 0) {
                sleep(1);
                continue;
            }
            usleep(500000);                     /* the old one gone (its socket name free) */
            pthread_mutex_lock(&cfg_lock);
            hz   = c->hz;
            gain = c->gain;
            pthread_mutex_unlock(&cfg_lock);
            w_start(i, place, hz, gain);
            ran      = place;
            said_gen = ~0u;
            continue;
        }
        if (place < 0) {
            sleep(1);
            continue;
        }
        if (fd < 0) {
            if ((fd = w_connect(i)) < 0) {
                usleep(300000);
                continue;
            }
            said_gen = ~0u;
        }
        unsigned g = atomic_load(&c->gen);

        if (g != said_gen) {                    /* its channel: tuned W_OFF under the device's middle, listened */
            char cmd[64];

            pthread_mutex_lock(&cfg_lock);
            hz   = c->hz;
            mode = c->mode;
            sql  = c->sql;
            gain = c->gain;
            pthread_mutex_unlock(&cfg_lock);
            quiet = 0;
            atomic_store(&c->open, false);
            snprintf(cmd, sizeof(cmd), "f %lld\n", (long long)(hz + W_OFF));
            say(fd, cmd);
            snprintf(cmd, sizeof(cmd), "s %d\n", W_RATE);
            say(fd, cmd);
            snprintf(cmd, sizeof(cmd), "g %d\n", gain);
            say(fd, cmd);
            say(fd, "r 1\n");
            snprintf(cmd, sizeof(cmd), "l %lld %d\n", (long long)hz, mode);
            say(fd, cmd);
            said_gen = g;
        }

        struct pollfd p = {.fd = fd, .events = POLLIN};
        int           r = poll(&p, 1, 200);

        if (r <= 0) continue;
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

        if (n <= 0) {                           /* it went: started again on its RTL */
            close(fd);
            fd  = -1;
            ran = -1;
            atomic_store(&c->up, false);
            continue;
        }
        if (n > 8 && !memcmp(buf, "RTLSPEC ", 8)) {
            char st[16] = "", tn[16] = "", kd[16] = "";
            int  ver = 0, nb = 0, tx = 0, rtl = -1;
            unsigned           rate = 0;
            unsigned long long lo = 0, hi = 0;

            buf[n] = 0;
            int got = sscanf(buf, "RTLSPEC %d %15s %15s %u %d %15s %llu %llu %d %d", &ver, st, tn, &rate, &nb, kd, &lo,
                             &hi, &tx, &rtl);

            if (got >= 1 && ver != PROTO) {     /* another version: it ends, this one's started */
                say(fd, "Q\n");
                close(fd);
                fd  = -1;
                ran = -1;
                continue;
            }
            if (got >= 10 && rtl >= 0 && rtl != ran && !strcmp(kd, "RTL")) {   /* one left from before on another */
                say(fd, "Q\n");
                close(fd);
                fd  = -1;
                ran = -1;
                continue;
            }
            atomic_store(&c->up, !strcmp(st, "ON"));
            continue;
        }
        if (n >= 24 && !memcmp(buf, "RSPC", 4)) {
            uint64_t ce;
            uint32_t rt;
            uint16_t nb;

            memcpy(&ce, buf + 8, 8);
            memcpy(&rt, buf + 16, 4);
            memcpy(&nb, buf + 20, 2);
            if (!nb || nb > MAX_ROW || (size_t)n < 24 + (size_t)nb * 4) continue;
            memcpy(row[i], buf + 24, (size_t)nb * 4);
            atomic_store(&c->up, true);
            w_row(c, hz, mode, sql, row[i], nb, rt, (int64_t)ce, &quiet);
            continue;
        }
        if (n >= 8 && !memcmp(buf, "RAUD", 4)) {
            uint32_t cnt;

            memcpy(&cnt, buf + 4, 4);
            if (cnt > (uint32_t)(n - 8) / 2) continue;
            pthread_mutex_lock(&c->lock);
            for (uint32_t k = 0; k < cnt; k++) {
                int16_t s;

                memcpy(&s, buf + 8 + k * 2, 2);
                c->ring[(c->head + c->len) % W_RING] = s;
                if (c->len < W_RING) c->len++;
                else c->head = (c->head + 1) % W_RING;
            }
            if (c->len > W_RING * 9 / 10) {     /* running over (the clocks differ): back to W_KEEP */
                c->head = (c->head + c->len - W_KEEP) % W_RING;
                c->len  = W_KEEP;
            }
            pthread_mutex_unlock(&c->lock);
        }
    }
    return NULL;
}

/* ---- The user's side ---- */

void rtl_watch_init(void) {
    static atomic_bool started;
    bool               no = false;
    FILE              *f;

    if (!atomic_compare_exchange_strong(&started, &no, true)) return;
    for (int i = 0; i < RTL_WATCH_N; i++) {
        pthread_mutex_init(&w[i].lock, NULL);
        atomic_store(&w[i].place, -1);
    }
    if ((f = fopen(WATCH_FILE, "r"))) {
        long long hz;
        int       m, q, g;

        while (n_w < RTL_WATCH_N && fscanf(f, "%lld %d %d %d", &hz, &m, &q, &g) == 4) {
            w[n_w].hz   = hz;
            w[n_w].mode = m;
            w[n_w].sql  = q;
            w[n_w].gain = g;
            n_w++;
        }
        fclose(f);
    }
    for (int i = 0; i < RTL_WATCH_N; i++) {
        pthread_t t;

        if (pthread_create(&t, NULL, w_thread, (void *)(intptr_t)i) == 0) pthread_detach(t);
    }
}

bool rtl_watch_has(int64_t hz) {
    bool has = false;

    pthread_mutex_lock(&cfg_lock);
    for (int i = 0; i < n_w && !has; i++) has = llabs(w[i].hz - hz) < 2000;
    pthread_mutex_unlock(&cfg_lock);
    return has;
}

int rtl_watch_toggle(int64_t hz, int mode, int sql_db, int gain) {
    int r = -1;

    if (!hz) return -1;
    bool free = rtl_watch_free() > 0;

    pthread_mutex_lock(&cfg_lock);
    for (int i = 0; i < n_w; i++)
        if (llabs(w[i].hz - hz) < 2000) {       /* watched: no longer, the rest moved up (their daemons follow) */
            for (int k = i; k + 1 < n_w; k++) {
                w[k].hz   = w[k + 1].hz;
                w[k].mode = w[k + 1].mode;
                w[k].sql  = w[k + 1].sql;
                w[k].gain = w[k + 1].gain;
                atomic_fetch_add(&w[k].gen, 1);
            }
            n_w--;
            r = 0;
            break;
        }
    if (r < 0 && n_w < RTL_WATCH_N && free) {
        w[n_w].hz   = hz;
        w[n_w].mode = mode;
        w[n_w].sql  = sql_db > 0 ? sql_db : 10;     /* a watch channel is always squelched */
        w[n_w].gain = gain;
        atomic_fetch_add(&w[n_w].gen, 1);
        n_w++;
        r = 1;
    }
    if (r >= 0) {
        alloc();
        save();
    }
    pthread_mutex_unlock(&cfg_lock);
    return r;
}

int rtl_watch_count(void) {
    pthread_mutex_lock(&cfg_lock);
    int n = n_w;
    pthread_mutex_unlock(&cfg_lock);
    return n;
}

bool rtl_watch_get(int i, rtl_watch_info_t *out) {
    bool ok = false;

    pthread_mutex_lock(&cfg_lock);
    if (i >= 0 && i < n_w) {
        out->hz      = w[i].hz;
        out->mode    = w[i].mode;
        out->place   = atomic_load(&w[i].place);
        out->up      = atomic_load(&w[i].up);
        out->open    = atomic_load(&w[i].open);
        out->over_db = atomic_load(&w[i].over10) / 10;
        ok           = true;
    }
    pthread_mutex_unlock(&cfg_lock);
    return ok;
}

bool rtl_watch_active(void) {
    uint32_t t = atomic_load(&last_open_ms);

    return t && now_ms() - t < W_ACTIVE;
}

void rtl_watch_mix(int16_t *buf, int n) {
    static float acc[2048];

    if (n > 2048) n = 2048;
    for (int k = 0; k < n; k++) acc[k] = buf[k];
    for (int i = 0; i < RTL_WATCH_N; i++) {
        watch_t *c  = &w[i];
        float    to = atomic_load(&c->open) && atomic_load(&c->place) >= 0 ? 1.0f : 0.0f;

        pthread_mutex_lock(&c->lock);
        if (to == 0 && c->g == 0) {             /* shut: what came meanwhile is old by the time it opens (50 ms kept) */
            if (c->len > 2205) {
                c->head = (c->head + c->len - 2205) % W_RING;
                c->len  = 2205;
            }
            pthread_mutex_unlock(&c->lock);
            continue;
        }
        for (int k = 0; k < n; k++) {
            int16_t s = 0;

            if (c->len) {
                s       = c->ring[c->head];
                c->head = (c->head + 1) % W_RING;
                c->len--;
            }
            if (c->g < to) c->g = c->g + 1.0f / 441 > to ? to : c->g + 1.0f / 441;      /* 10 ms, no click */
            else if (c->g > to) c->g = c->g - 1.0f / 441 < to ? to : c->g - 1.0f / 441;
            acc[k] += s * c->g;
        }
        pthread_mutex_unlock(&c->lock);
    }
    for (int k = 0; k < n; k++) {               /* a soft knee from 3/4 of full scale where the sum would clip */
        float v = acc[k], a = fabsf(v);

        if (a > 24000) v = (v > 0 ? 1 : -1) * (24000 + (a - 24000) * 8767 / (a - 24000 + 8767));
        buf[k] = (int16_t)v;
    }
}
