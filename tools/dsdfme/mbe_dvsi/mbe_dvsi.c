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

/* ---- IMBE 7200x4400 (P25 phase 1). The chip's P25 full-rate mode (rate words 0x0558 0x086B 0x1030 0 0 0x0190,
 * 7200 bit/s with FEC) takes the 144 bits in P25's own order on the air (each IMBE frame's 72 dibits, a dibit's bit 1
 * first): its frames all read as good codewords so (2026-11-17), and as nothing in plain codeword order. So the bits
 * dsd-fme sorted into imbe_fr (p25p1_ldu.c: imbe_fr[iW][iX] = bit 1, imbe_fr[iY][iZ] = bit 0; its p25p1_const.h's
 * tables, ISC) go back in that order, and the chip corrects them itself. Measured on this AMBE3000P25F: it reboots
 * (READY, its power-on rate again) when (a) a frame comes at full rate before it has done any frame since it last
 * started, (b) PKT_INIT asks for the encoder's reset at full rate, (c) its full-rate encoder is given quiet audio.
 * Only decoding here: (a) is met by a frame at rate 33 first whenever the chip may have restarted ---- */

static const int iW[72] = {0, 2, 4, 1, 3, 5, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6,
                       0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 6,
                       0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 4, 7, 0, 3, 5, 2, 4, 7};
static const int iX[72] = {22, 20, 10, 20, 18, 0, 20, 18, 8, 18, 16, 13, 18, 16, 6, 16, 14, 11, 16, 14, 4, 14, 12, 9,
                       14, 12, 2, 12, 10, 7, 12, 10, 0, 10, 8, 5, 10, 8, 13, 8, 6, 3, 8, 6, 11, 6, 4, 1,
                       6, 4, 9, 4, 2, 6, 4, 2, 7, 2, 0, 4, 2, 0, 5, 0, 13, 2, 0, 21, 3, 21, 11, 0};
static const int iY[72] = {1, 3, 5, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 4,
                       1, 3, 6, 0, 2, 4, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5, 1, 3, 6, 0, 2, 5,
                       1, 3, 6, 0, 2, 5, 1, 3, 7, 0, 2, 5, 1, 4, 7, 0, 3, 5, 2, 4, 7, 1, 3, 5};
static const int iZ[72] = {21, 19, 1, 21, 19, 9, 19, 17, 14, 19, 17, 7, 17, 15, 12, 17, 15, 5, 15, 13, 10, 15, 13, 3,
                       13, 11, 8, 13, 11, 1, 11, 9, 6, 11, 9, 14, 9, 7, 4, 9, 7, 12, 7, 5, 2, 7, 5, 10,
                       5, 3, 0, 5, 3, 8, 3, 1, 5, 3, 1, 6, 1, 14, 3, 1, 22, 4, 22, 12, 1, 22, 20, 2};
#define RATE_P25 1001                   /* rate's value in the P25 full-rate mode */

static int decode_p25(const uint8_t b[18], float *out) {
    static const uint8_t ratep[13] = {0x0A, 0x05, 0x58, 0x08, 0x6B, 0x10, 0x30, 0x00, 0x00, 0x00, 0x00, 0x01, 0x90};
    static int           warm;          /* a frame done since the chip (may have) started */
    uint8_t              q[20] = {0x01, 144}, r[340];
    int                  t, n;

    open_chip();
    if (fd < 0) return 0;
    if (rate == -1) warm = 0;           /* just found, or it rebooted */
    if (!warm) {                        /* a frame at rate 33 first: full rate as the first frame reboots it */
        uint8_t z[11] = {0x01, 72};

        if (ctl((const uint8_t[]){0x09, 33}, 2) == 0 && xfer(1, z, sizeof(z), &t, r, sizeof(r)) >= 2) warm = 1;
        rate = 33;
    }
    if ((rate != RATE_P25 || now_ms() - rate_ms > 100) && ctl(ratep, sizeof(ratep)) == 0) rate = RATE_P25;
    rate_ms = now_ms();
    memcpy(q + 2, b, 18);
    {                                   /* DSD_DVSI_DUMP=file: each frame as given to the chip (a test's check) */
        static FILE *dump;
        static int   tried;
        const char  *d = getenv("DSD_DVSI_DUMP");

        if (!tried++ && d && *d) dump = fopen(d, "ab");
        if (dump) fwrite(b, 1, 18, dump), fflush(dump);
    }
    n = xfer(1, q, sizeof(q), &t, r, sizeof(r));
    if (n >= 1 && t == 0 && r[0] == 0x39) {     /* READY: it rebooted, at its power-on rate */
        rate = -1;
        return 0;
    }
    if (n < 2 || t != 2 || r[0] != 0x00) {
        if (n < 0) {
            close(fd);
            fd = -1;
        }
        return 0;
    }
    int ns = r[1] > 160 ? 160 : r[1];

    for (int i = 0; i < ns && 3 + 2 * i < n; i++) out[i] = (float)(int16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);
    return ns;
}

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

/* Hamming (15,11) of c4-c6: bit 14..0 of a row of the parity checks, data in 14..4, parity in 3..0 (each data bit's
 * checks distinct, two or more; the parity bits one each). The chip's own frames are all good codewords by it */
static const int hamm_rows[4] = {0x7F08, 0x78E4, 0x66D2, 0x55B1};

static int hamm_syn(const char *c) {
    int w = 0, s = 0;

    for (int i = 0; i < 15; i++) w |= (c[i] & 1) << i;
    for (int r = 0; r < 4; r++) s = s << 1 | (__builtin_popcount((unsigned)(w & hamm_rows[r])) & 1);
    return s;
}

static int hamm_fix(char *c) {
    int s = hamm_syn(c);

    if (!s) return 0;
    for (int i = 0; i < 15; i++) {      /* the bit whose checks are the syndrome */
        int col = 0;

        for (int r = 0; r < 4; r++) col = col << 1 | (hamm_rows[r] >> i & 1);
        if (col == s) {
            c[i] ^= 1;
            return 1;
        }
    }
    return 2;                           /* more than one: left as it is */
}

int mbe_eccImbe7200x4400Data(char imbe_fr[8][23], char *imbe_d) {
    int n = 0, k = 0;

    for (int i = 0; i < 4; i++) {
        char c[23];

        memcpy(c, imbe_fr[i], 23);
        if (i > 0) n += golay_fix(c);
        for (int j = 22; j >= 11; j--) imbe_d[k++] = c[j];
    }
    for (int i = 4; i < 7; i++) {
        char c[15];

        memcpy(c, imbe_fr[i], 15);
        n += hamm_fix(c);
        for (int j = 14; j >= 4; j--) imbe_d[k++] = c[j];
    }
    for (int j = 6; j >= 0; j--) imbe_d[k++] = imbe_fr[7][j];
    return n;
}

/* The 88 voice bits (u0-u3 12, u4-u6 11, u7 7) coded again into the 144 sent (Golay, Hamming, the scrambling from
 * u0, P25's order): dsd-fme corrects a frame itself and hands on only those 88 */
static void imbe_encode(const char imbe_d[88], uint8_t b[18]) {
    char fr[8][23];
    int  k = 0;

    memset(fr, 0, sizeof(fr));
    for (int i = 0; i < 4; i++) {
        uint32_t w = 0;

        for (int j = 22; j >= 11; j--) fr[i][j] = imbe_d[k++] & 1, w |= (uint32_t)fr[i][j] << j;
        for (int j = 22; j >= 11; j--)
            if (w >> j & 1) w ^= (uint32_t)GOLAY_G << (j - 11);
        for (int j = 10; j >= 0; j--) fr[i][j] = (char)(w >> j & 1);
    }
    for (int i = 4; i < 7; i++) {
        int w = 0;

        for (int j = 14; j >= 4; j--) fr[i][j] = imbe_d[k++] & 1, w |= fr[i][j] << j;
        for (int r = 0; r < 4; r++) fr[i][3 - r] = (char)(__builtin_popcount((unsigned)(w & hamm_rows[r])) & 1);
    }
    for (int j = 6; j >= 0; j--) fr[7][j] = imbe_d[k++] & 1;
    mbe_demodulateImbe7200x4400Data(fr);        /* the scrambling: its own inverse */
    memset(b, 0, 18);
    for (int j = 0; j < 72; j++) {
        if (fr[iW[j]][iX[j]]) b[(2 * j) / 8] |= (uint8_t)(0x80 >> ((2 * j) % 8));
        if (fr[iY[j]][iZ[j]]) b[(2 * j + 1) / 8] |= (uint8_t)(0x80 >> ((2 * j + 1) % 8));
    }
}

/* For the test (p25_test.c): the 88 bits coded again as the chip would send them, and 144 sent bits sorted into
 * imbe_fr as dsd-fme sorts them */
void mbe_dvsi_imbe_encode(const char imbe_d[88], unsigned char b[18]) { imbe_encode(imbe_d, b); }

void mbe_dvsi_imbe_sort(const unsigned char b[18], char imbe_fr[8][23]) {
    memset(imbe_fr, 0, 8 * 23);
    for (int j = 0; j < 72; j++) {
        imbe_fr[iW[j]][iX[j]] = (char)(b[(2 * j) / 8] >> (7 - (2 * j) % 8) & 1);
        imbe_fr[iY[j]][iZ[j]] = (char)(b[(2 * j + 1) / 8] >> (7 - (2 * j + 1) % 8) & 1);
    }
}

/* The 88 bits after dsd-fme's own error correction (how it plays P25 phase 1, and ProVoice converted, and .imb
 * files): coded again into the 144 and to the chip in its P25 mode */
void mbe_processImbe4400Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char imbe_d[88],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality) {
    static char last[88];
    static int  again = 99;
    uint8_t     b[18];

    (void)errs, (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
    if (!may(cur_mp)) return;
    /* past what the codes put right (more than 5 bits changed in a frame: three in a Golay word may be a wrong
     * "correction"): its 88 bits are next to random, and some random frames make this chip reboot in its P25 mode
     * (5% of random frames, by the pitch with the rest; none of 471 the chip coded itself): the last good frame
     * again, three times, then silence */
    if ((errs2 ? *errs2 : 0) > 5) {
        if (++again > 3) return;
        if (err_str) strcpy(err_str, "R");
        imbe_encode(last, b);
    } else {
        again = 0;
        memcpy(last, imbe_d, 88);
        imbe_encode(imbe_d, b);
    }
    decode_p25(b, aout_buf);
}

void mbe_processImbe7200x4400Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char imbe_fr[8][23],
                                    char imbe_d[88], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality) {
    uint8_t b[18] = {0};

    (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    for (int j = 0; j < 72; j++) {      /* as it came off the air, before the codes below change imbe_fr */
        if (imbe_fr[iW[j]][iX[j]] & 1) b[(2 * j) / 8] |= (uint8_t)(0x80 >> ((2 * j) % 8));
        if (imbe_fr[iY[j]][iZ[j]] & 1) b[(2 * j + 1) / 8] |= (uint8_t)(0x80 >> ((2 * j + 1) % 8));
    }
    *errs = mbe_eccImbe7200x4400C0(imbe_fr);    /* for dsd-fme's own counts and its imbe_d */
    mbe_demodulateImbe7200x4400Data(imbe_fr);
    *errs2 = *errs + mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
    if (may(cur_mp)) decode_p25(b, aout_buf);
}

/* ---- AMBE 3600x2400 (D-STAR): the 72 bits as sent, back into the 9 bytes D-STAR carries them in (each byte sent
 * least significant bit first), to the chip as they are in its D-STAR mode (rate words 0x0130 0x0763 0x4000 0 0
 * 0x0048: 2400 bit/s of speech, 1200 of FEC, which the chip corrects itself), as DV3000s are used for D-STAR. Where
 * dsd-fme put each bit sent (dstar.c: ambe_fr[dW[i]][dX[i]] = bit i; its dstar_const.h's tables, ISC) ---- */

static const int dW[72] = {0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 2, 1, 1, 0, 0, 3, 2,
                           0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 2, 1, 1, 0, 0, 3, 2,
                           0, 0, 3, 2, 1, 1, 0, 0, 1, 1, 0, 0, 3, 2, 1, 1, 3, 3, 2, 1, 0, 0, 3, 3};
static const int dX[72] = {10, 22, 11, 9,  10, 22, 11, 23, 8,  20, 9,  21, 10, 8,  9,  21, 8,  6,
                           7,  19, 8,  20, 9,  7,  6,  18, 7,  5,  6,  18, 7,  19, 4,  16, 5,  17,
                           6,  4,  5,  17, 4,  2,  3,  15, 4,  16, 5,  3,  2,  14, 3,  1,  2,  14,
                           3,  15, 0,  12, 1,  13, 2,  0,  1,  13, 0,  12, 10, 11, 0,  12, 1,  13};
#define RATE_DSTAR 1000                 /* rate's value while the chip is in D-STAR's mode (not a rate index) */

static int decode_dstar(char ambe_fr[4][24], float *out) {
    static const uint8_t ratep[13] = {0x0A, 0x01, 0x30, 0x07, 0x63, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48};
    uint8_t              q[11] = {0x01, 72}, r[340];
    int                  t, n;

    open_chip();
    if (fd < 0) return 0;
    if ((rate != RATE_DSTAR || now_ms() - rate_ms > 100) && ctl(ratep, sizeof(ratep)) == 0) rate = RATE_DSTAR;
    rate_ms = now_ms();
    for (int i = 0; i < 72; i++)
        if (ambe_fr[dW[i]][dX[i]] & 1) q[2 + i / 8] |= (uint8_t)(1 << (i % 8));
    n = xfer(1, q, sizeof(q), &t, r, sizeof(r));
    if (n < 2 || t != 2 || r[0] != 0x00) {
        if (n < 0) {
            close(fd);
            fd = -1;
        }
        return 0;
    }
    int ns = r[1] > 160 ? 160 : r[1];

    for (int i = 0; i < ns && 3 + 2 * i < n; i++) out[i] = (float)(int16_t)(r[2 + 2 * i] << 8 | r[3 + 2 * i]);
    return ns;
}

void mbe_processAmbe2400Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_d[49],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality) {
    /* 49 bits with no FEC (dsd-fme's .amb files of D-STAR): the chip's order for them not measured: silence */
    (void)errs, (void)errs2, (void)ambe_d, (void)cur_mp, (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
}

void mbe_processAmbe3600x2400Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_fr[4][24],
                                    char ambe_d[49], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality) {
    (void)ambe_d, (void)prev_mp, (void)prev_mp_enhanced, (void)uvquality;
    memset(aout_buf, 0, 160 * sizeof(float));
    if (err_str) err_str[0] = 0;
    *errs = *errs2 = 0;                 /* the chip's own FEC: what it put right is not told */
    if (may(cur_mp)) decode_dstar(ambe_fr, aout_buf);
}

/* ---- IMBE 7100x4400 (ProVoice): silent for now ---- */

int  mbe_eccImbe7100x4400C0(char imbe_fr[7][24]) { (void)imbe_fr; return 0; }
void mbe_demodulateImbe7100x4400Data(char imbe_fr[7][24]) { (void)imbe_fr; }
int  mbe_eccImbe7100x4400Data(char imbe_fr[7][24], char *imbe_d) { (void)imbe_fr; memset(imbe_d, 0, 88); return 0; }
void mbe_convertImbe7100to7200(char *imbe_d) { (void)imbe_d; }

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
