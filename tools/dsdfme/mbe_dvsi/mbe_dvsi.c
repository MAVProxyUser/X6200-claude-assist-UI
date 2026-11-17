/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - libmbe-dvsi (mbelib.h): dsd-fme's vocoder calls answered by an AMBE-3000.
 *
 *  The chip's packets: 0x61, a length (big endian, what follows the type), a type (0 control, 1 channel data, 2
 *  speech), fields, a parity field (0x2F and the XOR of every byte after the 0x61 up to it). Each 20 ms of AMBE+2
 *  (49 voice bits, dsd-fme's ambe_d: u0 12, u1 12, u2 11, u3 14) goes as a channel packet at rate 34 (2450 bit/s, no
 *  FEC) in the chip's own order, measured on a USB-3000 (AMBE3000P25F) on 2026-11-16 against its rate-33 frames
 *  (DMR's air order): u0 to 0,3,..,33; u1 to 36,39,41,43,45,47,1,4,7,10,13,16; u2 to 19,22,..,46; u3 to 48,2,5,..,38.
 *  Its answer: 160 samples at 8 kHz, the floats dsd-fme takes (16-bit scale).
 *
 *  The error correction: Golay (23,12), generator 0xC75 (x^11+x^10+x^6+x^5+x^4+x^2+1), data in the top 12 bits of
 *  each codeword (bit i the frame's bit i), corrected by a syndrome table (up to three errors); the second codeword
 *  scrambled by a sequence from the first's data (p0 = 16 x data, p(n) = 173 p(n-1) + 13849 mod 65536, its top bit),
 *  as the chip's own frames bear out.
 *
 *  One channel: dsd-fme's second DMR slot (its own mbe_parms) is silent while the first talks (one stream holds the
 *  chip till 300 ms after its last frame).
 */

#define _GNU_SOURCE
#include "mbelib.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef B460800                         /* macOS: cfsetspeed takes the rate itself */
#define B460800 460800
#endif
#ifndef SOCK_CLOEXEC                    /* macOS */
#define SOCK_CLOEXEC 0
#endif

static const int u_to_chip[49] = {0,  3,  6,  9,  12, 15, 18, 21, 24, 27, 30, 33, 36, 39, 41, 43, 45,
                                  47, 1,  4,  7,  10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 42, 44,
                                  46, 48, 2,  5,  8,  11, 14, 17, 20, 23, 26, 29, 32, 35, 38};

/* ---- The chip ---- */

static int  fd = -1, serial, rate = -1;
static long last_ms, said_ms, rate_ms;

static long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int pkt(uint8_t *b, int type, const uint8_t *p, int n) {
    int     len = n + 2;
    uint8_t par = 0;

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

static int read_by(uint8_t *b, int n, long end) {
    while (n > 0) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        long          left = end - now_ms();

        if (left <= 0 || poll(&p, 1, (int)left) <= 0) return -1;
        ssize_t k = read(fd, b, (size_t)n);

        if (k < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (k <= 0) return -1;
        b += k;
        n -= (int)k;
    }
    return 0;
}

/* A packet out and its answer back (type and fields, the parity field off): their length, -1 none */
static int xfer(int type, const uint8_t *p, int n, int *rtype, uint8_t *r, int max) {
    uint8_t out[400], in[400];
    int     m = pkt(out, type, p, n), len;
    long    end = now_ms() + 300;

    if (fd < 0) return -1;
    if (!serial) {
        if (send(fd, out, (size_t)m, 0) != m) return -1;
        for (;;) {
            struct pollfd pf = {.fd = fd, .events = POLLIN};
            long          left = end - now_ms();
            ssize_t       k;

            if (left <= 0 || poll(&pf, 1, (int)left) <= 0) return -1;
            if ((k = recv(fd, in, sizeof(in), 0)) >= 5 && in[0] == 0x61) {
                len = in[1] << 8 | in[2];
                if (len + 4 > k) continue;
                break;
            }
        }
    } else {
        if (write(fd, out, (size_t)m) != m) return -1;
        do {
            if (read_by(in, 1, end)) return -1;
        } while (in[0] != 0x61);
        if (read_by(in + 1, 3, end)) return -1;
        len = in[1] << 8 | in[2];
        if (len + 4 > (int)sizeof(in) || read_by(in + 4, len, end)) return -1;
    }
    if (len >= 2 && in[4 + len - 2] == 0x2F) len -= 2;
    if (len > max) len = max;
    *rtype = in[3];
    memcpy(r, in + 4, (size_t)len);
    return len;
}

static int ctl(const uint8_t *q, int n) {
    uint8_t r[64];
    int     t, k = xfer(0, q, n, &t, r, sizeof(r));

    return k >= 2 && t == 0 && r[0] == q[0] && r[1] == 0 ? 0 : -1;
}

static void open_chip(void) {
    const char *w = getenv("DSD_DVSI");
    char        where[128];

    if (fd >= 0 || now_ms() - said_ms < 2000) return;           /* not there: asked again each 2 s */
    snprintf(where, sizeof(where), "%s", w && *w ? w : "127.0.0.1:2460");
    if (where[0] == '/') {                                       /* a serial port: its own chip */
        char          *c = strchr(where, ':');
        long           baud = c ? atol(c + 1) : 460800;
        struct termios t;

        if (c) *c = 0;
        if ((fd = open(where, O_RDWR | O_NOCTTY | O_NONBLOCK)) >= 0 && tcgetattr(fd, &t) == 0) {
            cfmakeraw(&t);
            cfsetspeed(&t, baud == 230400 ? B230400 : B460800);
            t.c_cflag |= CLOCAL | CREAD;
            t.c_cflag &= ~(tcflag_t)CRTSCTS;
            tcsetattr(fd, TCSANOW, &t);
            tcflush(fd, TCIOFLUSH);
            serial = 1;
        }
    } else {                                                     /* the SDR daemon's port, or another AMBEserver */
        char            *c = strrchr(where, ':');
        struct addrinfo  h = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *ai = NULL;

        if (c) *c++ = 0;
        if (getaddrinfo(where, c ? c : "2460", &h, &ai) == 0) {
            if ((fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) >= 0 && connect(fd, ai->ai_addr, ai->ai_addrlen))
                close(fd), fd = -1;
            freeaddrinfo(ai);
        }
        serial = 0;
    }
    uint8_t r[64];
    int     t, k;

    if (fd >= 0 && (k = xfer(0, (const uint8_t[]){0x30}, 1, &t, r, sizeof(r))) > 1 && memmem(r, (size_t)k, "AMBE", 4)) {
        fprintf(stderr, "libmbe-dvsi: %.*s at %s\n", k - 1, (const char *)r + 1, w && *w ? w : "127.0.0.1:2460");
        rate = -1;
        return;
    }
    if (fd >= 0) close(fd);
    fd      = -1;
    said_ms = now_ms();
    fprintf(stderr, "libmbe-dvsi: no AMBE-3000 at %s (DSD_DVSI): no voice\n", w && *w ? w : "127.0.0.1:2460");
}

/* One stream holds the chip; another's frames are silence till 300 ms after its last */
static const void *holder;

static int may(const void *who) {
    long t = now_ms();

    if (holder && holder != who && t - last_ms < 300) return 0;
    if (holder != who) {
        holder = who;
        if (fd >= 0) ctl((const uint8_t[]){0x0B, 0x02}, 2);     /* the decoder from fresh for it */
    }
    last_ms = t;
    return 1;
}

/* 49 voice bits (u0 u1 u2 u3) at rate 34: 160 samples, 0 none */
static int decode49(const char d[49], float *out) {
    uint8_t q[9] = {0x01, 49}, r[340];
    int     t, n;

    open_chip();
    if (fd < 0) return 0;
    /* rate 34 when it was not, and again after a pause (the SDR daemon sets 33 for its own DMR in between) */
    if ((rate != 34 || now_ms() - rate_ms > 100) && ctl((const uint8_t[]){0x09, 34}, 2) == 0) rate = 34;
    rate_ms = now_ms();
    for (int i = 0; i < 49; i++)
        if (d[i] & 1) q[2 + u_to_chip[i] / 8] |= (uint8_t)(0x80 >> (u_to_chip[i] % 8));
    n = xfer(1, q, sizeof(q), &t, r, sizeof(r));
    if (n < 2 || t != 2 || r[0] != 0x00) {
        if (n < 0) {                    /* gone: looked for again */
            close(fd);
            fd = -1;
        }
        return 0;
    }
    int ns = r[1] > 160 ? 160 : r[1];

    for (int i = 0; i < ns && 3 + 2 * i < n; i++) out[i] = (float)(int16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);
    return ns;
}

/* ---- The error correction ---- */

#define GOLAY_G 0xC75

static uint32_t golay_err[2048];        /* by syndrome: the error pattern of three bits or fewer */
static int      golay_made;

static uint32_t golay_syn(uint32_t w) {
    for (int i = 22; i >= 11; i--)
        if (w >> i & 1) w ^= (uint32_t)GOLAY_G << (i - 11);
    return w & 0x7FF;
}

static void golay_init(void) {
    if (golay_made) return;
    for (int a = 0; a < 23; a++)
        for (int b = a; b < 23; b++)
            for (int c = b; c < 23; c++) {
                uint32_t e = 1u << a | 1u << b | 1u << c;   /* a == b == c: one bit; two equal: one; the rest */

                if (!golay_err[golay_syn(e)]) golay_err[golay_syn(e)] = e;
            }
    golay_made = 1;
}

/* A 23-bit codeword (bit i = c[i]) put right in place: the bits changed */
static int golay_fix(char *c) {
    uint32_t w = 0, e;
    int      n = 0;

    golay_init();
    for (int i = 0; i < 23; i++) w |= (uint32_t)(c[i] & 1) << i;
    e = golay_err[golay_syn(w)];
    for (int i = 0; i < 23; i++)
        if (e >> i & 1) c[i] ^= 1, n++;
    return n;
}

int mbe_eccAmbe3600x2450C0(char ambe_fr[4][24]) {
    return golay_fix(ambe_fr[0] + 1);
}

void mbe_demodulateAmbe3600x2450Data(char ambe_fr[4][24]) {
    unsigned p = 0;

    for (int i = 23; i >= 12; i--) p = p << 1 | (unsigned)(ambe_fr[0][i] & 1);
    p *= 16;
    for (int j = 22; j >= 0; j--) {
        p = (173 * p + 13849) & 0xFFFF;
        ambe_fr[1][j] ^= (char)(p >> 15);
    }
}

int mbe_eccAmbe3600x2450Data(char ambe_fr[4][24], char *ambe_d) {
    char c1[23];
    int  n, k = 0;

    memcpy(c1, ambe_fr[1], 23);
    n = golay_fix(c1);
    for (int j = 23; j >= 12; j--) ambe_d[k++] = ambe_fr[0][j];
    for (int j = 22; j >= 11; j--) ambe_d[k++] = c1[j];
    for (int j = 10; j >= 0; j--) ambe_d[k++] = ambe_fr[2][j];
    for (int j = 13; j >= 0; j--) ambe_d[k++] = ambe_fr[3][j];
    return n;
}

/* ---- AMBE+2 ---- */

static char last_d[49];
static int  repeats = 99;

void mbe_processAmbe2450Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_d[49],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality) {
    (void)errs, (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
    if (!may(cur_mp)) return;
    /* past what the codes put right (errs2 is both codewords' errors, three each at most: more is a frame they may
     * have "corrected" into another; no FEC at rate 34): the last good frame again, three times, then silence */
    if ((errs2 ? *errs2 : 0) > 6) {
        if (++repeats > 3) return;
        if (err_str) strcpy(err_str, "R");
        decode49(last_d, aout_buf);
        return;
    }
    repeats = 0;
    memcpy(last_d, ambe_d, 49);
    decode49(ambe_d, aout_buf);
}

void mbe_processAmbe3600x2450Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_fr[4][24],
                                    char ambe_d[49], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality) {
    *errs = mbe_eccAmbe3600x2450C0(ambe_fr);
    mbe_demodulateAmbe3600x2450Data(ambe_fr);
    *errs2 = *errs + mbe_eccAmbe3600x2450Data(ambe_fr, ambe_d);
    mbe_processAmbe2450Dataf(aout_buf, errs, errs2, err_str, ambe_d, cur_mp, prev_mp, prev_mp_enhanced, uvquality);
}

/* ---- IMBE 7200x4400 (P25 phase 1): the codes; the voice once the chip's P25 mode is measured ---- */

int mbe_eccImbe7200x4400C0(char imbe_fr[8][23]) {
    return golay_fix(imbe_fr[0]);
}

void mbe_demodulateImbe7200x4400Data(char imbe_fr[8][23]) {
    unsigned p = 0;

    for (int i = 22; i >= 11; i--) p = p << 1 | (unsigned)(imbe_fr[0][i] & 1);
    p *= 16;
    for (int i = 1; i < 4; i++)
        for (int j = 22; j >= 0; j--) {
            p = (173 * p + 13849) & 0xFFFF;
            imbe_fr[i][j] ^= (char)(p >> 15);
        }
    for (int i = 4; i < 7; i++)
        for (int j = 14; j >= 0; j--) {
            p = (173 * p + 13849) & 0xFFFF;
            imbe_fr[i][j] ^= (char)(p >> 15);
        }
}

int mbe_eccImbe7200x4400Data(char imbe_fr[8][23], char *imbe_d) {
    int n = 0, k = 0;

    for (int i = 0; i < 4; i++) {
        char c[23];

        memcpy(c, imbe_fr[i], 23);
        if (i > 0) n += golay_fix(c);
        for (int j = 22; j >= 11; j--) imbe_d[k++] = c[j];
    }
    for (int i = 4; i < 7; i++)                 /* Hamming (15,11): its data as it came, for now */
        for (int j = 14; j >= 4; j--) imbe_d[k++] = imbe_fr[i][j];
    for (int j = 6; j >= 0; j--) imbe_d[k++] = imbe_fr[7][j];
    return n;
}

void mbe_processImbe4400Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char imbe_d[88],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality) {
    (void)errs, (void)errs2, (void)imbe_d, (void)cur_mp, (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
}

void mbe_processImbe7200x4400Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char imbe_fr[8][23],
                                    char imbe_d[88], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality) {
    *errs = mbe_eccImbe7200x4400C0(imbe_fr);
    mbe_demodulateImbe7200x4400Data(imbe_fr);
    *errs2 = *errs + mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
    mbe_processImbe4400Dataf(aout_buf, errs, errs2, err_str, imbe_d, cur_mp, prev_mp, prev_mp_enhanced, uvquality);
}

/* ---- IMBE 7100x4400 (ProVoice) and AMBE 3600x2400 (D-STAR): silent for now ---- */

int  mbe_eccImbe7100x4400C0(char imbe_fr[7][24]) { (void)imbe_fr; return 0; }
void mbe_demodulateImbe7100x4400Data(char imbe_fr[7][24]) { (void)imbe_fr; }
int  mbe_eccImbe7100x4400Data(char imbe_fr[7][24], char *imbe_d) { (void)imbe_fr; memset(imbe_d, 0, 88); return 0; }
void mbe_convertImbe7100to7200(char *imbe_d) { (void)imbe_d; }

void mbe_processAmbe2400Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_d[49],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality) {
    (void)errs, (void)errs2, (void)ambe_d, (void)cur_mp, (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
}

void mbe_processAmbe3600x2400Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_fr[4][24],
                                    char ambe_d[49], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality) {
    (void)ambe_fr;
    mbe_processAmbe2400Dataf(aout_buf, errs, errs2, err_str, ambe_d, cur_mp, prev_mp, prev_mp_enhanced, uvquality);
}

/* ---- The rest ---- */

void mbe_printVersion(char *str) { strcpy(str, "DVSI-1.0"); }

void mbe_initMbeParms(mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced) {
    (void)prev_mp, (void)prev_mp_enhanced;
    if (cur_mp) memset(cur_mp, 0, sizeof(*cur_mp));
    if (holder == cur_mp) holder = NULL;
    repeats = 99;                       /* a new start: nothing to repeat */
}

void mbe_floattoshort(float *float_buf, short *aout_buf) {
    for (int i = 0; i < 160; i++) {
        float a = float_buf[i];

        aout_buf[i] = (short)(a > 32760 ? 32760 : a < -32760 ? -32760 : a);
    }
}
