/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the USB-3000's AMBE-3000R (ambe.h). Its packets: 0x61, a length (big endian, what follows the type),
 *  a type (0 control, 1 channel data, 2 speech), its fields, and a parity field (0x2F and the XOR of every byte after
 *  the 0x61 up to it). Set up: reset (0x33) and its READY (0x39), the product ID (0x30: "AMBE3000..."), rate 33
 *  (0x09). Each frame: a channel packet (field 0x01: 72 bits and the frame's 9 bytes) answered by a speech packet
 *  (field 0x00: 160 samples, 16-bit big endian, 20 ms at 8 kHz). Its port: an FTDI one (USB 0403), 460800 baud, 8N1
 *  (230400 tried after); ftdi_sio is loaded when an FTDI device is on USB and no driver has it (the image has it as a
 *  module, and nothing else loads it).
 */

#define _GNU_SOURCE
#include "ambe.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FRAMES 64               /* frames waiting to be decoded */
#define PCM    8192             /* 8 kHz speech kept: 1 s */
#define START  480              /* 60 ms of it in before a call is heard: DMR's frames come three at a time */
#define GAIN   1.5f             /* the vocoder's speech about as loud as narrow FM's audio here */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  more = PTHREAD_COND_INITIALIZER;
static uint8_t         fq[FRAMES][9];
static int             fq_n, fq_r;
static int16_t         pcm[PCM];
static unsigned        pcm_w, pcm_r;
static bool            playing;
static atomic_bool     ready, started;
static int             fd = -1;
static char            dev[32], prod[48];

static long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static bool put_all(const uint8_t *b, size_t n) {
    while (n) {
        ssize_t k = write(fd, b, n);

        if (k < 0 && errno == EAGAIN) {
            struct pollfd p = {.fd = fd, .events = POLLOUT};

            if (poll(&p, 1, 200) <= 0) return false;
            continue;
        }
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        b += k;
        n -= (size_t)k;
    }
    return true;
}

static bool send_pkt(int type, const uint8_t *p, int n) {
    uint8_t b[16 + 340], par = 0;
    int     len = n + 2;

    if (n > 330) return false;
    b[0] = 0x61;
    b[1] = (uint8_t)(len >> 8);
    b[2] = (uint8_t)len;
    b[3] = (uint8_t)type;
    memcpy(b + 4, p, (size_t)n);
    b[4 + n] = 0x2F;
    for (int i = 1; i < 5 + n; i++) par ^= b[i];
    b[5 + n] = par;
    return put_all(b, (size_t)(6 + n));
}

/* n bytes by the deadline */
static bool get_all(uint8_t *b, int n, long end) {
    while (n > 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        long          left = end - now_ms();

        if (left <= 0 || poll(&p, 1, (int)left) <= 0) return false;
        ssize_t k = read(fd, b, (size_t)n);

        if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (k <= 0) return false;
        b += k;
        n -= (int)k;
    }
    return true;
}

/* A packet within ms: its type and fields (its parity field off), their length; -1 none */
static int recv_pkt(int *type, uint8_t *p, int max, int ms) {
    long    end = now_ms() + ms;
    uint8_t h[4];

    do {                                /* to its start */
        if (!get_all(h, 1, end)) return -1;
    } while (h[0] != 0x61);
    if (!get_all(h + 1, 3, end)) return -1;
    int len = h[1] << 8 | h[2];

    if (len < 0 || len > max) return -1;
    if (!get_all(p, len, end)) return -1;
    *type = h[3];
    if (len >= 2 && p[len - 2] == 0x2F) len -= 2;
    return len;
}

/* A control packet sent and its answer to the same field taken: its length, -1 none */
static int ask(const uint8_t *q, int n, uint8_t *r, int max, int ms) {
    long end = now_ms() + ms;
    int  type, got;

    if (!send_pkt(0, q, n)) return -1;
    while (now_ms() < end)
        if ((got = recv_pkt(&type, r, max, (int)(end - now_ms()))) > 0 && type == 0 && r[0] == q[0]) return got;
    return -1;
}

/* Another program on the port: gpsd (an image whose udev took the USB-3000 for a GPS, before 26-usb3000.rules) */
static void free_port(const char *path) {
    DIR           *d = opendir("/proc");
    struct dirent *e;
    bool           held = false;

    while (d && !held && (e = readdir(d))) {
        char           fds[64], l[300], to[64];
        DIR           *f;
        struct dirent *g;

        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        snprintf(fds, sizeof(fds), "/proc/%s/fd", e->d_name);
        if (!(f = opendir(fds))) continue;
        while (!held && (g = readdir(f))) {
            ssize_t k;

            snprintf(l, sizeof(l), "%s/%s", fds, g->d_name);
            if ((k = readlink(l, to, sizeof(to) - 1)) > 0) {
                to[k] = 0;
                held  = !strcmp(to, path) && (pid_t)atoi(e->d_name) != getpid();
            }
        }
        closedir(f);
    }
    if (d) closedir(d);
    if (!held) return;
    fprintf(stderr, "rtlspec: %s held by another program (gpsd): asked to let it go\n", path);
    {
        char cmd[96];

        snprintf(cmd, sizeof(cmd), "GPSD_SOCKET=/var/run/gpsd.sock gpsdctl remove %s >/dev/null 2>&1", path);
        if (system(cmd)) { /* no gpsd: nothing to ask */ }
    }
}

/* The FTDI's latency timer to 1 ms: at its 16 ms each answer waited that long, and three 20 ms frames come every
 * 60 ms */
static void low_latency(const char *path) {
    char  p[96];
    FILE *f;

    snprintf(p, sizeof(p), "/sys/bus/usb-serial/devices/%s/latency_timer", path + 5);
    if ((f = fopen(p, "w"))) {
        fputs("1", f);
        fclose(f);
    }
}

static bool set_up(const char *path, speed_t speed) {
    struct termios t;
    uint8_t        r[340];
    int            n, type, ld = N_TTY;
    long           end;

    free_port(path);
    low_latency(path);
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;
    ioctl(fd, TIOCSETD, &ld);           /* gpsd's PPS line discipline off it */
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, speed);
        cfsetospeed(&t, speed);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= ~(tcflag_t)CRTSCTS;
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &t);
    }
    tcflush(fd, TCIOFLUSH);
    {                                   /* a packet it may be half way through (garbage from a probe, gpsd's) filled
                                         * up: zeros between packets are passed over */
        static const uint8_t zero[1024];

        for (int i = 0; i < 70; i++)
            if (!put_all(zero, sizeof(zero))) break;
        tcdrain(fd);
    }
    send_pkt(0, (const uint8_t[]){0x33}, 1);            /* reset: READY when it is up again */
    bool rdy = false;

    for (end = now_ms() + 1500; now_ms() < end && !rdy;)
        rdy = (n = recv_pkt(&type, r, sizeof(r), (int)(end - now_ms()))) > 0 && type == 0 && r[0] == 0x39;
    if ((n = ask((const uint8_t[]){0x30}, 1, r, sizeof(r), 500)) < 2 || !memmem(r + 1, (size_t)(n - 1), "AMBE", 4)) {
        static int said;                /* why, once for each speed */

        if (said < 2) {
            said++;
            fprintf(stderr, "rtlspec: %s at %s baud: %s, no product ID (%d): again each 30 s\n", path,
                    speed == B460800 ? "460800" : "another", rdy ? "READY after reset" : "no READY", n);
        }
        close(fd);
        fd = -1;
        return false;
    }
    snprintf(prod, sizeof(prod), "%.*s", n - 1, (const char *)r + 1);
    if ((n = ask((const uint8_t[]){0x09, 33}, 2, r, sizeof(r), 500)) < 2 || r[1] != 0) {   /* rate 33: DMR's */
        fprintf(stderr, "rtlspec: %s on %s refused rate 33\n", prod, path);
        close(fd);
        fd = -1;
        return false;
    }
    snprintf(dev, sizeof(dev), "%s", path);
    return true;
}

/* An FTDI device on USB with no driver: ftdi_sio loaded, once */
static void ftdi_driver(void) {
    static bool tried;
    DIR        *d;
    struct dirent *e;
    struct stat st;

    if (tried || stat("/sys/bus/usb/drivers/ftdi_sio", &st) == 0 || !(d = opendir("/sys/bus/usb/devices"))) return;
    while ((e = readdir(d))) {
        char  p[300], v[8] = "";
        FILE *f;

        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        if ((f = fopen(p, "r"))) {
            if (!fgets(v, sizeof(v), f)) v[0] = 0;
            fclose(f);
        }
        if (!strncmp(v, "0403", 4)) {
            tried = true;
            fprintf(stderr, "rtlspec: an FTDI device on USB: ftdi_sio loaded for it\n");
            if (system("modprobe ftdi_sio 2>/dev/null")) { /* none: no USB-3000 here */ }
            break;
        }
    }
    closedir(d);
}

/* Each FTDI serial port asked for an AMBE-3000 */
static bool find(void) {
    DIR           *d;
    struct dirent *e;
    bool           ok = false;

    ftdi_driver();
    if (!(d = opendir("/sys/bus/usb-serial/devices"))) return false;
    while (!ok && (e = readdir(d))) {
        char  p[300], v[8] = "", path[32];
        FILE *f;

        if (strncmp(e->d_name, "ttyUSB", 6)) continue;
        snprintf(p, sizeof(p), "/sys/bus/usb-serial/devices/%s/../../idVendor", e->d_name);
        if ((f = fopen(p, "r"))) {
            if (!fgets(v, sizeof(v), f)) v[0] = 0;
            fclose(f);
        }
        if (strncmp(v, "0403", 4)) continue;
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        ok = set_up(path, B460800);     /* the USB-3000's only: at another speed it reads garbage, and a 0x61 in it
                                         * leaves it waiting for a packet's length (2026-11-16: it then answered
                                         * nothing) */
    }
    closedir(d);
    return ok;
}

static void lost(void) {
    fprintf(stderr, "rtlspec: the AMBE vocoder on %s gone\n", dev);
    close(fd);
    fd = -1;
    atomic_store(&ready, false);
}

static void *thread(void *arg) {
    int misses = 0;

    (void)arg;
    for (;;) {
        uint8_t f[9], q[11] = {0x01, 72}, r[340];
        int     type, n;

        if (fd < 0) {
            static long last_try;
            static bool any;

            if (now_ms() - last_try < (any ? 30000 : 2000)) {      /* a port that did not answer: each 30 s */
                sleep(2);
                continue;
            }
            last_try = now_ms();
            if (!find()) {
                any = access("/sys/bus/usb-serial/devices/ttyUSB0", F_OK) == 0;
                continue;
            }
            fprintf(stderr, "rtlspec: AMBE vocoder %s on %s, rate 33 (DMR)\n", prod, dev);
            atomic_store(&ready, true);
            misses = 0;
        }
        pthread_mutex_lock(&lock);
        while (!fq_n) {
            struct timespec ts;

            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            if (pthread_cond_timedwait(&more, &lock, &ts) == ETIMEDOUT) break;
        }
        bool have = fq_n > 0;

        if (have) {
            memcpy(f, fq[fq_r], 9);
            fq_r = (fq_r + 1) % FRAMES;
            fq_n--;
        }
        pthread_mutex_unlock(&lock);
        if (!have) {                    /* idle: still there? */
            if (access(dev, F_OK)) lost();
            continue;
        }
        memcpy(q + 2, f, 9);
        if (!send_pkt(1, q, sizeof(q))) {
            lost();
            continue;
        }
        n = recv_pkt(&type, r, sizeof(r), 300);
        if (n < 0) {
            if (++misses >= 5) lost();
            continue;
        }
        misses = 0;
        if (type != 2 || n < 2 || r[0] != 0x00) continue;
        int ns = r[1];

        if (2 + 2 * ns > n) ns = (n - 2) / 2;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < ns; i++) {
            if (pcm_w - pcm_r >= PCM) pcm_r++;     /* behind: the oldest goes */
            pcm[pcm_w++ % PCM] = (int16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

void ambe_start(void) {
    bool      no = false;
    pthread_t t;

    if (!atomic_compare_exchange_strong(&started, &no, true)) return;
    if (pthread_create(&t, NULL, thread, NULL) == 0) pthread_detach(t);
}

bool ambe_ready(void) { return atomic_load(&ready); }

void ambe_frame(const uint8_t frame[9]) {
    if (!atomic_load(&ready)) return;
    pthread_mutex_lock(&lock);
    if (fq_n < FRAMES) {
        memcpy(fq[(fq_r + fq_n) % FRAMES], frame, 9);
        fq_n++;
        pthread_cond_signal(&more);
    }
    pthread_mutex_unlock(&lock);
}

void ambe_flush(void) {
    pthread_mutex_lock(&lock);
    pcm_r   = pcm_w;
    fq_n    = 0;
    playing = false;
    pthread_mutex_unlock(&lock);
}

float ambe_audio(void) {
    static double ph;
    static float  a, b;
    float         v;

    pthread_mutex_lock(&lock);
    if (!playing && pcm_w - pcm_r >= START) playing = true;     /* a little in hand: frames come in threes */
    if (!playing) {
        pthread_mutex_unlock(&lock);
        return 0;
    }
    ph += 8000.0 / 44100.0;
    while (ph >= 1) {
        ph -= 1;
        a = b;
        if (pcm_w != pcm_r) {
            b = pcm[pcm_r++ % PCM] / 32768.0f;
        } else {                        /* out: silent till the next 60 ms */
            b       = 0;
            playing = false;
        }
    }
    v = (a + (b - a) * (float)ph) * GAIN;
    pthread_mutex_unlock(&lock);
    return v;
}
