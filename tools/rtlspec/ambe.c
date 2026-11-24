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
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FRAMES 64               /* frames waiting to be decoded */
#define PCM    8192             /* 8 kHz speech kept: 1 s */
#define START  480              /* 60 ms of it in before a call is heard: DMR's frames come three at a time */
#define GAIN   1.5f             /* the vocoder's speech about as loud as narrow FM's audio here */
#define PORT   2460             /* the chip for other programs (dsd-fme through its DVSI libmbe, tests), as
                                 * AMBEserver gives it: a datagram an AMBE-3000 packet, its answer the one back.
                                 * 127.0.0.1 only */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  more = PTHREAD_COND_INITIALIZER;
static uint8_t         fq[FRAMES][9];
static int             fq_n, fq_r;
static int16_t         pcm[PCM];
static unsigned        pcm_w, pcm_r;
static bool            playing;
static atomic_bool     ready, started;
static int             fd = -1;
static int             udp = -1;
static int             chip_rate = -1;  /* the rate it was set to last (33 DMR's), -1 another or not known */
static uint8_t         chip_cfg[16];    /* the rate fields it was last set with by anyone (0x09 n, or 0x0A and six
                                         * words), chip_cfg_n bytes: 0 not known */
static int             chip_cfg_n;
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

/* A packet in b (16 + 340 bytes): its length, -1 too long */
static int build_pkt(uint8_t *b, int type, const uint8_t *p, int n) {
    uint8_t par = 0;
    int     len = n + 2;

    if (n > 330) return -1;
    b[0] = 0x61;
    b[1] = (uint8_t)(len >> 8);
    b[2] = (uint8_t)len;
    b[3] = (uint8_t)type;
    memcpy(b + 4, p, (size_t)n);
    b[4 + n] = 0x2F;
    for (int i = 1; i < 5 + n; i++) par ^= b[i];
    b[5 + n] = par;
    return 6 + n;
}

static bool send_pkt(int type, const uint8_t *p, int n) {
    uint8_t b[16 + 340];
    int     len = build_pkt(b, type, p, n);

    return len > 0 && put_all(b, (size_t)len);
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

/* A packet within ms, whole as it came (its 0x61 and header too): its length; -1 none */
static int recv_raw(uint8_t *b, int max, int ms) {
    long end = now_ms() + ms;

    do {                                /* to its start */
        if (!get_all(b, 1, end)) return -1;
    } while (b[0] != 0x61);
    if (!get_all(b + 1, 3, end)) return -1;
    int len = b[1] << 8 | b[2];

    if (len < 0 || len + 4 > max || !get_all(b + 4, len, end)) return -1;
    return len + 4;
}

/* The whole packet b (its 0x61 first, n bytes) ends in a parity field: 0x2F and the XOR of every byte after the 0x61
 * before it. A chip may send none: the DVMega's AMBE3000F leaves it off until asked (PKT_PARITYMODE), and a field
 * whose next to last byte is 0x2F lost its last two bytes to a check of that byte alone (2026-11-24: 9 of 525 DMR
 * frames from its encoder cut short) */
static bool has_parity(const uint8_t *b, int n) {
    uint8_t x = 0;

    if (n < 6 || b[n - 2] != 0x2F) return false;
    for (int i = 1; i < n; i++) x ^= b[i];
    return x == 0;
}

/* A packet within ms: its type and fields (its parity field off), their length; -1 none */
static int recv_pkt(int *type, uint8_t *p, int max, int ms) {
    uint8_t b[400];
    int     n = recv_raw(b, sizeof(b), ms), len;

    if (n < 0 || n - 4 > max) return -1;
    len   = n - 4;
    *type = b[3];
    memcpy(p, b + 4, (size_t)len);
    if (has_parity(b, n)) len -= 2;
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

/* Every packet from the chip with its parity field, as the USB-3000 sends them from power-on and the DVMega's stick
 * only when asked: one layout for every client of the port. Set up and after each restart (power-on's again); a chip
 * that refuses it is read all the same (has_parity) */
static void parity_on(void) {
    uint8_t r[16];

    ask((const uint8_t[]){0x3F, 1}, 2, r, sizeof(r), 300);
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

/* The chip tried as it is set up: 200 ms of a 1 kHz tone encoded at rate 33 (a speech packet: field 0x00, 160 samples)
 * and each frame decoded again, the last 100 ms of what comes back measured (its zero crossings, its level). Said in
 * the log; the codec's state set back after (PKT_INIT, 0x0B: encoder and decoder) */
static void self_test(void) {
    uint8_t r[340], q[2 + 320], c[11];
    int     type, n, zc = 0, frames = 0;
    double  energy = 0;
    int16_t last = 0;

    for (int f = 0; f < 10; f++) {
        q[0] = 0x00;
        q[1] = 160;
        for (int i = 0; i < 160; i++) {
            int16_t v = (int16_t)(8000 * sin(2 * M_PI * 1000 * (f * 160 + i) / 8000.0));

            q[2 + 2 * i] = (uint8_t)(v >> 8);
            q[3 + 2 * i] = (uint8_t)v;
        }
        if (!send_pkt(2, q, sizeof(q)) || (n = recv_pkt(&type, r, sizeof(r), 300)) < 11 || type != 1 || r[0] != 0x01)
            break;
        memcpy(c, r, 11);               /* field 0x01, 72 bits, the frame: as the decoder takes it */
        if (!send_pkt(1, c, sizeof(c)) || (n = recv_pkt(&type, r, sizeof(r), 300)) < 2 || type != 2 || r[0] != 0x00)
            break;
        frames++;
        for (int i = 0; f >= 5 && i < r[1] && 3 + 2 * i < n; i++) {
            int16_t v = (int16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);

            if ((v >= 0) != (last >= 0)) zc++;
            last = v;
            energy += (double)v * v;
        }
    }
    if (frames < 10)
        fprintf(stderr, "rtlspec: AMBE self-test: %d of 10 frames through\n", frames);
    else
        fprintf(stderr, "rtlspec: AMBE self-test: a 1 kHz tone (8000) encoded and decoded came back at %d Hz, level %.0f\n",
                zc * 5, sqrt(energy / 800) * M_SQRT2);
    ask((const uint8_t[]){0x0B, 0x03}, 2, r, sizeof(r), 300);
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
    parity_on();
    if ((n = ask((const uint8_t[]){0x09, 33}, 2, r, sizeof(r), 500)) < 2 || r[1] != 0) {   /* rate 33: DMR's */
        fprintf(stderr, "rtlspec: %s on %s refused rate 33\n", prod, path);
        close(fd);
        fd = -1;
        return false;
    }
    snprintf(dev, sizeof(dev), "%s", path);
    self_test();
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

/* The UDP port: one request at a time, the chip's answer back to its sender. A rate set through it is noted: DMR's
 * own frames set 33 again first */
static int udp_err;     /* why the port could not be had: EADDRINUSE, another daemon serves the chip */

static void udp_open(void) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT)};

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0) return;
    if (bind(udp, (struct sockaddr *)&a, sizeof(a)) < 0) {
        udp_err = errno;
        fprintf(stderr, "rtlspec: the AMBE port 127.0.0.1:%d: %s\n", PORT, strerror(udp_err));
        close(udp);
        udp = -1;
    }
}

/* Another daemon's port (a test daemon's, beside B's: ambe_start): DMR's frames go to the chip through it, as the
 * dsd-fme modes' do, the chip's serial port being that daemon's alone */
static int cli = -1;

static void cli_open(void) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(PORT)};

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if ((cli = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) return;
    if (connect(cli, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(cli);
        cli = -1;
    }
}

/* One packet through that port and its answer within ms: its type and fields (its parity field off), their length;
 * -1 none. Answers late from an earlier one are passed over first */
static int port_pkt(int type, const uint8_t *p, int n, int *rtype, uint8_t *r, int max, int ms) {
    uint8_t b[16 + 340], a[400];
    int     len = build_pkt(b, type, p, n);
    long    end = now_ms() + ms;

    if (len < 0 || cli < 0) return -1;
    while (recv(cli, a, sizeof(a), MSG_DONTWAIT) > 0) {}
    if (send(cli, b, (size_t)len, 0) != len) return -1;
    while (now_ms() < end) {
        struct pollfd pf = {.fd = cli, .events = POLLIN};
        ssize_t       k;

        if (poll(&pf, 1, (int)(end - now_ms())) <= 0) return -1;
        k = recv(cli, a, sizeof(a), 0);
        if (k < 6 || a[0] != 0x61 || (a[1] << 8 | a[2]) + 4 != k || k - 4 > max) continue;
        int l = (int)k - 4;

        *rtype = a[3];
        memcpy(r, a + 4, (size_t)l);
        if (has_parity(a, (int)k)) l -= 2;
        return l;
    }
    return -1;
}

static void lost(void);

/* The chip set to the rate fields f (n bytes): said for the port's clients and DMR's own frames */
static void chip_is(const uint8_t *f, int n) {
    if (n <= 0 || n > (int)sizeof(chip_cfg)) {
        chip_cfg_n = 0;
        chip_rate  = -1;
        return;
    }
    memcpy(chip_cfg, f, (size_t)n);
    chip_cfg_n = n;
    chip_rate  = f[0] == 0x09 && n >= 2 ? f[1] : -1;
}

/* The port's clients (B's three dsd-fme, the GUI's DMR app, a test daemon's DMR), each one's rate as it last set it.
 * The chip has one rate: with two decoding at once, each frame went out at whichever rate was set last (an NXDN
 * dsd-fme's false syncs in a P25 call sent its frames at NXDN's rate between the P25 one's: the call's 1 kHz came out
 * at 400 Hz, and D-STAR's voice dropped out, 2026-11-24). A client's frame now goes with its own rate whenever
 * another's is on the chip */
#define CLIENTS 8
static struct {
    struct sockaddr_in a;
    uint8_t            rate[16];
    int                rn;          /* its rate fields' length, 0 none set yet */
    long               used;
} clients[CLIENTS];

static int client_of(const struct sockaddr_in *a) {
    int old = 0;

    for (int i = 0; i < CLIENTS; i++) {
        if (clients[i].used && clients[i].a.sin_addr.s_addr == a->sin_addr.s_addr &&
            clients[i].a.sin_port == a->sin_port) {
            clients[i].used = now_ms();
            return i;
        }
        if (clients[i].used < clients[old].used) old = i;
    }
    memset(&clients[old], 0, sizeof(clients[old]));     /* new, or the longest unheard replaced */
    clients[old].a    = *a;
    clients[old].used = now_ms();
    return old;
}

/* Client i's rate on the chip again before its frame, when another's is there: its answer taken here. False: the chip
 * gone */
static bool rate_for(int i) {
    uint8_t q[16 + 340], r[64];
    int     len, t, k;

    if (!clients[i].rn || (chip_cfg_n == clients[i].rn && !memcmp(chip_cfg, clients[i].rate, (size_t)chip_cfg_n)))
        return true;
    if ((len = build_pkt(q, 0, clients[i].rate, clients[i].rn)) < 0) return true;
    if (!put_all(q, (size_t)len)) return false;
    k = recv_pkt(&t, r, sizeof(r), 400);
    if (k >= 2 && t == 0 && r[0] == clients[i].rate[0] && r[1] == 0) chip_is(clients[i].rate, clients[i].rn);
    else chip_is(NULL, 0);
    return true;
}

static void serve_udp(void) {
    uint8_t            b[400], r[400];
    struct sockaddr_in from;
    socklen_t          fl = sizeof(from);
    ssize_t            n;

    while (udp >= 0 && (n = recvfrom(udp, b, sizeof(b), 0, (struct sockaddr *)&from, &fl)) > 0) {
        if (n < 5 || b[0] != 0x61 || (b[1] << 8 | b[2]) + 4 != n || fd < 0) continue;     /* no chip: no answer */
        int ci = client_of(&from), fn = (int)n - 4 - (has_parity(b, (int)n) ? 2 : 0);
        bool rate_pkt = b[3] == 0 && fn >= 2 && fn <= 16 && (b[4] == 0x09 || b[4] == 0x0A);

        if (rate_pkt) {                 /* its rate, kept for its frames */
            memcpy(clients[ci].rate, b + 4, (size_t)fn);
            clients[ci].rn = fn;
        } else if ((b[3] == 1 || b[3] == 2) && !rate_for(ci)) {
            lost();
            return;
        }
        if (b[3] == 0 && (b[4] == 0x33 || rate_pkt)) chip_is(NULL, 0);     /* till its answer says */
        if (!put_all(b, (size_t)n)) {
            lost();
            return;
        }
        int rn = recv_raw(r, sizeof(r), b[3] == 0 && b[4] == 0x33 ? 1500 : 400);

        if (rate_pkt && rn >= 6 && r[3] == 0 && r[4] == b[4] && r[5] == 0) chip_is(b + 4, fn);

        if (rn >= 5 && r[3] == 0 && r[4] == 0x39) {     /* READY: it rebooted (P25's full rate can make it), its
                                                         * power-on rate again: DMR's set afresh before its frames */
            static long said;
            static int  since;

            chip_is(NULL, 0);
            parity_on();                                /* power-on's layout again: the parity field asked for */
            since++;
            if (now_ms() - said > 60000) {              /* said once a minute at most, with how many */
                said = now_ms();
                fprintf(stderr, "rtlspec: the AMBE-3000 restarted itself %d time%s (a client's request at P25 full "
                                "rate); taken up again\n", since, since == 1 ? "" : "s");
                since = 0;
            }
        }
        if (rn > 0) sendto(udp, r, (size_t)rn, 0, (struct sockaddr *)&from, fl);
        fl = sizeof(from);
    }
}

static void lost(void) {
    fprintf(stderr, "rtlspec: the AMBE vocoder on %s gone\n", dev);
    close(fd);
    fd = -1;
    atomic_store(&ready, false);
}

/* A speech packet's samples (field 0x00, their count, 16-bit big endian) into the speech heard */
static void pcm_put(const uint8_t *r, int n) {
    int ns = r[1];

    if (2 + 2 * ns > n) ns = (n - 2) / 2;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < ns; i++) {
        if (pcm_w - pcm_r >= PCM) pcm_r++;     /* behind: the oldest goes */
        pcm[pcm_w++ % PCM] = (int16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);
    }
    pthread_mutex_unlock(&lock);
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
            fprintf(stderr, "rtlspec: AMBE vocoder %s on %s, rate 33 (DMR); 127.0.0.1:%d for others\n", prod, dev,
                    PORT);
            atomic_store(&ready, true);
            chip_is(NULL, 0);
            misses = 0;
        }
        serve_udp();
        pthread_mutex_lock(&lock);
        bool have = fq_n > 0;

        if (have) {
            memcpy(f, fq[fq_r], 9);
            fq_r = (fq_r + 1) % FRAMES;
            fq_n--;
        }
        pthread_mutex_unlock(&lock);
        if (!have) {                    /* idle: a request on the port, or 20 ms; still there? */
            static long looked;
            struct pollfd pf = {.fd = udp, .events = POLLIN};

            if (udp < 0 || poll(&pf, 1, 20) <= 0) {
                if (now_ms() - looked > 2000) {
                    looked = now_ms();
                    if (access(dev, F_OK)) lost();
                }
            }
            continue;
        }
        if (chip_rate != 33) {          /* another rate set through the port, or the chip restarted: DMR's again */
            parity_on();
            if (ask((const uint8_t[]){0x09, 33}, 2, r, sizeof(r), 300) < 2 || r[1] != 0) {
                if (++misses >= 5) lost();
                continue;
            }
            chip_is((const uint8_t[]){0x09, 33}, 2);
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
        if (type == 0 && n >= 1 && r[0] == 0x39) {     /* READY: it rebooted, its rate the power-on one */
            chip_is(NULL, 0);
            continue;
        }
        if (type != 2 || n < 2 || r[0] != 0x00) continue;
        pcm_put(r, n);
    }
    return NULL;
}

/* A test daemon's (another daemon has the chip): each DMR frame through that daemon's port. Rate 33 set at the start,
 * after the chip restarted, and after an answer missed (another of its clients may have set another rate between
 * calls); said in the log when that daemon stops answering and when it answers again */
static void *port_thread(void *arg) {
    int misses = 0;

    (void)arg;
    {
        uint8_t r[340];
        int     type, n = port_pkt(0, (const uint8_t[]){0x30}, 1, &type, r, sizeof(r), 1000);

        if (n >= 2 && type == 0 && memmem(r + 1, (size_t)(n - 1), "AMBE", 4))
            snprintf(prod, sizeof(prod), "%.*s", n - 1, (const char *)r + 1);
        fprintf(stderr, "rtlspec: DMR's frames through the other daemon's AMBE port 127.0.0.1:%d (%s)\n", PORT,
                prod[0] ? prod : "no answer yet");
        atomic_store(&ready, true);     /* frames queued for it from now on, answered or not (said in the log) */
    }
    chip_rate = -1;
    for (;;) {
        uint8_t f[9], q[11] = {0x01, 72}, r[340];
        int     type, n;

        pthread_mutex_lock(&lock);
        while (fq_n == 0) pthread_cond_wait(&more, &lock);
        memcpy(f, fq[fq_r], 9);
        fq_r = (fq_r + 1) % FRAMES;
        fq_n--;
        pthread_mutex_unlock(&lock);
        if (chip_rate != 33) {
            if ((n = port_pkt(0, (const uint8_t[]){0x09, 33}, 2, &type, r, sizeof(r), 500)) < 2 || type != 0 ||
                r[0] != 0x09 || r[1] != 0) {
                if (++misses == 5) fprintf(stderr, "rtlspec: the other daemon's AMBE port does not answer\n");
                continue;
            }
            chip_rate = 33;
        }
        memcpy(q + 2, f, 9);
        n = port_pkt(1, q, sizeof(q), &type, r, sizeof(r), 500);
        if (n < 0) {
            chip_rate = -1;
            if (++misses == 5) fprintf(stderr, "rtlspec: the other daemon's AMBE port does not answer\n");
            continue;
        }
        if (misses >= 5) fprintf(stderr, "rtlspec: the other daemon's AMBE port answers again\n");
        misses = 0;
        if (type == 0 && n >= 1 && r[0] == 0x39) {     /* READY: the chip restarted, its power-on rate */
            chip_rate = -1;
            continue;
        }
        if (type != 2 || n < 2 || r[0] != 0x00) continue;
        pcm_put(r, n);
    }
    return NULL;
}

void ambe_start(void) {
    bool      no = false;
    pthread_t t;

    if (!atomic_compare_exchange_strong(&started, &no, true)) return;
    udp_open();
    if (udp < 0 && udp_err == EADDRINUSE) {
        /* Another daemon serves the chip already (B's, and this a test daemon beside it: -S, -i, RTLSPEC_DMR): its
         * serial port left alone, as two writers on it wedge the chip (a 0x61 half read). DMR's frames go through
         * that daemon's port (port_thread), as the dsd-fme modes' do; up to 2026-11-18 a test daemon's DMR was
         * silent */
        fprintf(stderr, "rtlspec: the AMBE vocoder is another daemon's (127.0.0.1:%d in use): not opened here\n", PORT);
        cli_open();
        if (cli >= 0 && pthread_create(&t, NULL, port_thread, NULL) == 0) pthread_detach(t);
        return;
    }
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
