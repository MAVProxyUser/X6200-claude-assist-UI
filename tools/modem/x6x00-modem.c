/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: one end of the link, on a radio. It hears and keys the radio through the radio's own TCI server
 *  (the GUI's, on this radio: 24 kHz int16 mono, "trx:0,true,tci" and an audio frame for every TX_CHRONO), runs
 *  link.c over it, and is driven and watched from the Mac over a TCP port: commands in, a line of JSON ten times
 *  a second out (the live view, tools/modem/view).
 *
 *  It transmits only while the Mac says it may ("may 1", sent every 50 ms, good for 1.5 s, "may 0" ends it at once):
 *  the Mac says so
 *  only while the other radio's own state says it is off the air, which is a second lock on "one radio on the
 *  air at a time", apart from the link's own turn-taking. With no Mac connected it does not transmit. It never
 *  keys for more than MAX_KEY_S, and unkeys and stops when the Mac goes away.
 *
 *      x6x00-modem [--tci PORT] [--port PORT] [--name NAME] [--tx-peak INT16] [--record FILE]
 *
 *  Commands (a line each): "start role=sender|receiver bytes=N max=0-3 both=0|1 burst=S lead=S rounds=N",
 *  "stop", "may 0|1", "quit".
 */
#define _GNU_SOURCE
#include "link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define FS          24000.0
#define MAX_KEY_S   15.0        /* the radio on the air for longer than this: unkeyed, the link stopped */
#define MAY_FOR_MS  1500        /* a "may 1" holds this long (the Mac sends one every 50 ms; a "may 0" ends it at once) */
#define TXQ         (1 << 17)   /* samples waiting for TX_CHRONO: 5.4 s */
#define AT_RX       1
#define AT_TX       2
#define AT_CHRONO   3

static int      tci = -1, lsn = -1, ctl = -1;
static char     name[24] = "radio";
static int      tci_port = 50001, ctl_port = 7362;
static double   tx_peak  = 3277.0;      /* int16 the link's loudest goes out at: -20 dBFS, the X6100's TCI level */
static link_t   L;
static bool     running;
static bool     key_asked, radio_tx;
static uint64_t key_asked_ms, radio_tx_since, radio_tx_off_ms, may_until;
static float    txq[TXQ];
static uint32_t txq_head, txq_tail;
static uint8_t  inbuf[1 << 17];
static size_t   inlen;
static char     ctlbuf[2048];
static size_t   ctllen;
static uint32_t log_sent, pts_sent;
static float    spec_ring[512];
static uint32_t spec_at;
static double   rx_level;
static uint64_t frames_rx, chronos;
static FILE    *rec;                    /* --record FILE: every received sample, int16, for a look at the line */
static FILE    *gaps;                   /* and FILE.gaps: each gap of 40 ms or more between RX frames */
static FILE    *txlog;                  /* and FILE.tx: each TX_CHRONO, what was queued for it and what went */
static uint64_t last_frame_ms, start_ms, rx_samples, gap_count, gap_max;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "x6x00-modem: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- the WebSocket to TCI ---- */

static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k < 0 && errno == EINTR)
            continue;
        if (k < 0 && errno == EAGAIN) {
            struct pollfd pf = {fd, POLLOUT, 0};
            poll(&pf, 1, 50);
            continue;
        }
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int ws_send(int op, const void *p, size_t n) {
    uint8_t hdr[14], mask[4];
    size_t  h = 0;
    uint8_t buf[8192 + 14];

    hdr[h++] = (uint8_t)(0x80 | op);
    if (n < 126)
        hdr[h++] = (uint8_t)(0x80 | n);
    else {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(n >> 8);
        hdr[h++] = (uint8_t)n;
    }
    for (int i = 0; i < 4; i++)
        mask[i] = (uint8_t)rand();
    memcpy(hdr + h, mask, 4);
    h += 4;
    if (n > sizeof(buf) - h)
        return -1;
    memcpy(buf, hdr, h);
    for (size_t i = 0; i < n; i++)
        buf[h + i] = ((const uint8_t *)p)[i] ^ mask[i % 4];
    return send_all(tci, buf, h + n);
}

static int tci_text(const char *s) {
    return ws_send(1, s, strlen(s));
}

static int tci_connect(void) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)tci_port)};
    char               req[512], resp[2048];
    size_t             got = 0;

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    tci               = socket(AF_INET, SOCK_STREAM, 0);
    if (tci < 0 || connect(tci, (struct sockaddr *)&a, sizeof(a)) < 0) {
        note("TCI on port %d: %s", tci_port, strerror(errno));
        return -1;
    }
    snprintf(req, sizeof(req),
             "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: eDZ4MDAtbW9kZW0tbGluaw==\r\nSec-WebSocket-Version: 13\r\n"
             "User-Agent: x6x00-modem\r\n\r\n",
             tci_port);
    if (send_all(tci, req, strlen(req)) < 0)
        return -1;
    while (got < sizeof(resp) - 1) {
        ssize_t k = recv(tci, resp + got, sizeof(resp) - 1 - got, 0);
        if (k <= 0)
            return -1;
        got += (size_t)k;
        resp[got] = 0;
        char *end = strstr(resp, "\r\n\r\n");
        if (end) {
            if (!strstr(resp, " 101 ")) {
                note("TCI did not upgrade: %.60s", resp);
                return -1;
            }
            size_t rest = got - (size_t)(end + 4 - resp);
            memcpy(inbuf, end + 4, rest);
            inlen = rest;
            break;
        }
    }
    int one = 1;
    setsockopt(tci, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fcntl(tci, F_SETFL, fcntl(tci, F_GETFL) | O_NONBLOCK);
    tci_text("audio_samplerate:24000;");
    tci_text("audio_stream_sample_type:int16;");
    tci_text("audio_stream_channels:1;");
    tci_text("audio_start:0;");
    note("TCI on port %d: audio 24 kHz int16 mono", tci_port);
    return 0;
}

/* ---- the link, a sample at a time on the radio's own audio clock ---- */

static uint32_t txq_len(void) {
    return txq_head - txq_tail;
}

static void key(bool on) {
    if (on == key_asked)
        return;
    key_asked    = on;
    key_asked_ms = now_ms();
    tci_text(on ? "trx:0,true,tci;" : "trx:0,false;");
}

static void stop_all(const char *why) {
    if (running)
        note("stopped: %s (the Mac's last may %lld ms ago)", why, (long long)(now_ms() - (may_until - MAY_FOR_MS)));
    running = false;
    txq_head = txq_tail = 0;
    key(false);
}

static bool may_key(void) {
    return ctl >= 0 && now_ms() < may_until;
}

static void on_rx_sample(double x) {
    if (!running) {
        spec_ring[spec_at++ % 512] = (float)x;
        rx_level                   = 0.999 * rx_level + 0.001 * fabs(x);
        return;
    }
    L.hold   = !may_key();
    double y = link_tx_sample(&L);
    if (link_keyed(&L) && txq_len() < TXQ)
        txq[txq_head++ % TXQ] = (float)y;
    /* not while the radio is on the air, nor just after: its receiver is not hearing the far end */
    bool deaf = radio_tx || key_asked || now_ms() - radio_tx_off_ms < 60;
    if (!deaf)
        link_rx_sample(&L, x);
    float a = (float)(link_keyed(&L) ? y : x);
    spec_ring[spec_at++ % 512] = a;
    rx_level = 0.999 * rx_level + 0.001 * fabs(x);
}

static void on_chrono(uint32_t want) {
    uint8_t  frame[64 + 2 * 2048];
    uint32_t hdr[16] = {0, 24000, 0, 0, 0, 0, AT_TX, 1};
    int16_t *pcm     = (int16_t *)(frame + 64);

    chronos++;
    if (txlog)
        fprintf(txlog, "%.3f %u %u %d %d %d\n", (now_ms() - start_ms) / 1000.0, want, txq_len(), key_asked, radio_tx,
                link_keyed(&L));
    if (want > 2048)
        want = 2048;
    hdr[5] = want;
    memcpy(frame, hdr, 64);
    for (uint32_t i = 0; i < want; i++) {
        double v = txq_len() ? txq[txq_tail++ % TXQ] : 0.0;
        double s = v * tx_peak / 0.5;
        pcm[i]   = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
    }
    if (key_asked)
        ws_send(2, frame, 64 + 2 * want);
}

static void on_tci_text(const char *s, size_t n) {
    char t[256];
    if (n >= sizeof(t))
        n = sizeof(t) - 1;
    memcpy(t, s, n);
    t[n] = 0;
    for (char *p = strstr(t, "trx:0,"); p; p = strstr(p + 1, "trx:0,")) {
        bool on = !strncmp(p + 6, "true", 4);
        if (on && !radio_tx)
            radio_tx_since = now_ms();
        if (!on && radio_tx)
            radio_tx_off_ms = now_ms();
        radio_tx = on;
    }
}

static void tci_read(void) {
    for (;;) {
        ssize_t k = recv(tci, inbuf + inlen, sizeof(inbuf) - inlen, 0);
        if (k == 0) {
            note("TCI closed");
            stop_all("TCI closed");
            exit(1);
        }
        if (k < 0)
            break;
        inlen += (size_t)k;
    }
    size_t pos = 0;
    while (inlen - pos >= 2) {
        uint8_t *b   = inbuf + pos;
        int      op  = b[0] & 0x0f;
        uint64_t len = b[1] & 0x7f;
        size_t   h   = 2;
        if (len == 126) {
            if (inlen - pos < 4)
                break;
            len = (uint64_t)b[2] << 8 | b[3];
            h   = 4;
        } else if (len == 127) {
            if (inlen - pos < 10)
                break;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = len << 8 | b[2 + i];
            h = 10;
        }
        if (inlen - pos < h + len)
            break;
        uint8_t *p = b + h;
        if (op == 1)
            on_tci_text((const char *)p, (size_t)len);
        else if (op == 2 && len >= 64) {
            uint32_t hdr[8];
            memcpy(hdr, p, sizeof(hdr));
            if (hdr[6] == AT_RX && hdr[2] == 0) {
                uint32_t n = hdr[5] / (hdr[7] ? hdr[7] : 1);
                uint64_t t = now_ms();
                if (last_frame_ms && t - last_frame_ms >= 40) {
                    gap_count++;
                    if (t - last_frame_ms > gap_max)
                        gap_max = t - last_frame_ms;
                    if (gaps)
                        fprintf(gaps, "%.3f %llu %llu %d %d\n", (t - start_ms) / 1000.0, (unsigned long long)(t - last_frame_ms),
                                (unsigned long long)rx_samples, radio_tx, key_asked);
                }
                last_frame_ms = t;
                rx_samples += n;
                if (64 + 2 * (uint64_t)n * (hdr[7] ? hdr[7] : 1) <= len) {
                    frames_rx++;
                    for (uint32_t i = 0; i < n; i++) {
                        int16_t s;
                        memcpy(&s, p + 64 + 2 * (size_t)i * (hdr[7] ? hdr[7] : 1), 2);
                        if (rec)
                            fwrite(&s, 2, 1, rec);
                        on_rx_sample(s / 32768.0);
                    }
                }
            } else if (hdr[6] == AT_CHRONO)
                on_chrono(hdr[5] / (hdr[7] ? hdr[7] : 1));
        } else if (op == 9)
            ws_send(10, p, (size_t)len);
        else if (op == 8) {
            stop_all("TCI closed the WebSocket");
            exit(1);
        }
        pos += h + (size_t)len;
    }
    memmove(inbuf, inbuf + pos, inlen - pos);
    inlen -= pos;
}

/* ---- the Mac ---- */

static void ctl_send(const char *s) {
    if (ctl >= 0 && send_all(ctl, s, strlen(s)) < 0) {
        close(ctl);
        ctl = -1;
        stop_all("the Mac went away");
    }
}

static double arg_d(const char *line, const char *key, double dflt) {
    char        k[32];
    const char *p;
    snprintf(k, sizeof(k), " %s=", key);
    p = strstr(line, k);
    return p ? atof(p + strlen(k)) : dflt;
}

static void command(char *line) {
    if (!strncmp(line, "may ", 4)) {
        if (atoi(line + 4))
            may_until = now_ms() + MAY_FOR_MS;
        else
            may_until = 0;
        return;
    }
    note("command: %s", line);
    if (!strncmp(line, "start", 5)) {
        link_cfg_t c;
        bool       sender = strstr(line, "role=sender") != NULL;
        link_defaults(&c, FS, sender ? LINK_SENDER : LINK_RECEIVER);
        c.name       = name;
        c.data_bytes = (int)arg_d(line, "bytes", c.data_bytes);
        c.max_rate   = (rate_t)(int)arg_d(line, "max", c.max_rate);
        c.both_ways  = arg_d(line, "both", 0) != 0;
        c.rounds     = (int)arg_d(line, "rounds", 0);
        c.burst_s    = arg_d(line, "burst", c.burst_s);
        c.lead_s     = arg_d(line, "lead", c.lead_s);
        c.tail_s     = arg_d(line, "tail", c.tail_s);
        c.preamble_s = arg_d(line, "preamble", c.preamble_s);
        c.v21_channel = (int)arg_d(line, "v21", c.v21_channel) == 1 ? 1 : 2;
        if (c.max_rate >= RATE_COUNT)
            c.max_rate = RATE_9600;
        if (c.burst_s > 10.0)
            c.burst_s = 10.0;   /* a block on the air at most 10 s, inside MAX_KEY_S with its training */
        if (running)
            link_free(&L);
        link_init(&L, &c);
        log_sent = pts_sent = 0;
        txq_head = txq_tail = 0;
        running  = true;
        link_start(&L);
    } else if (!strncmp(line, "tone", 4)) {
        /* "tone hz=H secs=S lead=L tail=T": one transmission of a steady tone, for the audio path's measure */
        link_cfg_t c;
        link_defaults(&c, FS, LINK_RECEIVER);
        c.name   = name;
        c.lead_s = arg_d(line, "lead", c.lead_s);
        c.tail_s = arg_d(line, "tail", c.tail_s);
        double secs = arg_d(line, "secs", 3.0);
        if (secs > 8.0)
            secs = 8.0;
        if (running)
            link_free(&L);
        link_init(&L, &c);
        log_sent = pts_sent = 0;
        txq_head = txq_tail = 0;
        running  = true;
        link_tone(&L, arg_d(line, "hz", 1000.0), secs);
    } else if (!strncmp(line, "stop", 4)) {
        stop_all("asked");
    } else if (!strncmp(line, "quit", 4)) {
        stop_all("quit");
        usleep(100000);
        exit(0);
    }
}

static void ctl_read(void) {
    ssize_t k = recv(ctl, ctlbuf + ctllen, sizeof(ctlbuf) - 1 - ctllen, 0);
    if (k <= 0) {
        if (k < 0 && errno == EAGAIN)
            return;
        close(ctl);
        ctl = -1;
        stop_all("the Mac went away");
        return;
    }
    ctllen += (size_t)k;
    ctlbuf[ctllen] = 0;
    char *nl;
    while ((nl = strchr(ctlbuf, '\n'))) {
        *nl = 0;
        command(ctlbuf);
        size_t used = (size_t)(nl + 1 - ctlbuf);
        memmove(ctlbuf, nl + 1, ctllen - used + 1);
        ctllen -= used;
    }
    if (ctllen >= sizeof(ctlbuf) - 1)
        ctllen = 0;
}

static void json_str(char **o, const char *s) {
    *(*o)++ = '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            *(*o)++ = '\\';
        *(*o)++ = (*s >= 32) ? *s : ' ';
    }
    *(*o)++ = '"';
}

/* ten times a second: everything the view draws */
static void telemetry(void) {
    static char out[24576];
    char       *o = out;
    qamrx_t    *q = &L.rx.rx;

    o += sprintf(o, "{\"name\":\"%s\",\"t\":%.2f,\"running\":%d,\"role\":\"%s\",\"state\":\"%s\",\"keyed\":%d,"
                    "\"radio_tx\":%d,\"may\":%d,\"rate\":%d,\"rate_name\":\"%s\",\"seg\":%d,\"far\":",
                 name, running ? link_seconds(&L) : 0.0, running, L.role == LINK_SENDER ? "sender" : "receiver",
                 running ? link_state_name(L.state) : "stopped", link_keyed(&L), radio_tx, may_key(), (int)L.rate,
                 rate_name(L.rate), link_keyed(&L) && L.at < L.nplan ? (int)L.plan[L.at].kind : -1);
    json_str(&o, L.far_name);
    o += sprintf(o, ",\"rx\":{\"armed\":%d,\"state\":%d,\"snr\":%.1f,\"trained\":%.1f,\"offset\":%.2f,\"ppm\":%.1f,"
                    "\"timing\":%.2f,\"shifts\":%d,\"level_db\":%.1f}",
                 L.fast_armed, L.fast_armed ? (int)q->state : 0, q->snr_db, q->trained_snr_db, q->offset_hz,
                 -q->rate_err * 1e6, q->timing_halves, q->shifts, 20 * log10(rx_level + 1e-9));
    o += sprintf(o, ",\"stats\":{\"bytes\":%llu,\"total\":%d,\"ftt\":%llu,\"ppr\":%llu,\"resent\":%llu,\"bad\":%llu,"
                    "\"t0\":%.2f,\"t1\":%.2f,\"air\":[%.2f,%.2f,%.2f,%.2f],\"tcf_ber\":%.5f,\"round\":%d,"
                    "\"v21_good\":%u,\"v21_bad\":%u,\"fast_good\":%u,\"fast_bad\":%u,\"frames_rx\":%llu,"
                    "\"chronos\":%llu,\"txq\":%u,\"rx_gaps\":%llu,\"rx_gap_max\":%llu,\"rx_s\":%.1f,\"up_s\":%.1f}",
                 (unsigned long long)L.bytes_delivered, L.cfg.data_bytes, (unsigned long long)L.ftt_count,
                 (unsigned long long)L.ppr_count, (unsigned long long)L.frames_resent,
                 (unsigned long long)L.frames_bad, L.t_start, L.t_end, L.air_s[0], L.air_s[1], L.air_s[2],
                 L.air_s[3], L.tcf_ber, L.round, L.h21.good, L.h21.bad, L.hfast.good, L.hfast.bad,
                 (unsigned long long)frames_rx, (unsigned long long)chronos, txq_len(), (unsigned long long)gap_count,
                 (unsigned long long)gap_max, rx_samples / FS, (now_ms() - start_ms) / 1000.0);
    /* the constellation: what came since the last line, at most 160 points */
    o += sprintf(o, ",\"pts_rate\":%d,\"pts\":[", L.points_rate);
    uint32_t from = L.npoints - pts_sent > 160 ? L.npoints - 160 : pts_sent;
    for (uint32_t i = from; i < L.npoints; i++) {
        cplx p = L.points[i % LINK_POINTS];
        o += sprintf(o, "%s[%.3f,%.3f]", i == from ? "" : ",", creal(p), cimag(p));
    }
    pts_sent = L.npoints;
    o += sprintf(o, "],\"taps\":[");
    for (int i = 0; i < QAM_TAPS; i++)
        o += sprintf(o, "%s%.3f", i ? "," : "", cabs(q->taps[i]));
    /* the audio's spectrum, 0 to 3.9 kHz in 63 bins, dB */
    o += sprintf(o, "],\"spec\":[");
    for (int k = 1; k < 64; k++) {
        double re = 0, im = 0, w = TAU * k * 62.5 / FS;
        for (int i = 0; i < 512; i++) {
            double h = 0.5 - 0.5 * cos(TAU * i / 511), v = spec_ring[(spec_at + i) % 512] * h;
            re += v * cos(w * i);
            im -= v * sin(w * i);
        }
        o += sprintf(o, "%s%.1f", k > 1 ? "," : "", 10 * log10(re * re + im * im + 1e-12));
    }
    o += sprintf(o, "],\"log\":[");
    if (L.nlog - log_sent > LINK_LOG)
        log_sent = L.nlog - LINK_LOG;
    for (uint32_t i = log_sent; i < L.nlog && o - out < (long)sizeof(out) - 400; i++) {
        o += sprintf(o, "%s{\"t\":%.2f,\"text\":", i == log_sent ? "" : ",", L.log[i % LINK_LOG].t);
        json_str(&o, L.log[i % LINK_LOG].text);
        *o++ = '}';
    }
    log_sent = L.nlog;
    o += sprintf(o, "]}\n");
    ctl_send(out);
}

int main(int argc, char **argv) {
    uint64_t last_tel = 0;

    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tci") && i + 1 < argc)
            tci_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)
            ctl_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--name") && i + 1 < argc)
            snprintf(name, sizeof(name), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tx-peak") && i + 1 < argc)
            tx_peak = atof(argv[++i]);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            char g[256];
            rec = fopen(argv[++i], "wb");
            snprintf(g, sizeof(g), "%s.gaps", argv[i]);
            gaps = fopen(g, "w");
            if (gaps)
                setvbuf(gaps, NULL, _IOLBF, 0);
            snprintf(g, sizeof(g), "%s.tx", argv[i]);
            txlog = fopen(g, "w");
            if (txlog)
                setvbuf(txlog, NULL, _IOLBF, 0);
        }
    }
    srand((unsigned)time(NULL));
    start_ms = now_ms();
    if (tci_connect() < 0)
        return 1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)ctl_port)};
    int                one = 1;
    lsn                    = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(lsn, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lsn, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(lsn, 1) < 0) {
        note("port %d: %s", ctl_port, strerror(errno));
        return 1;
    }
    note("%s: waiting for the Mac on port %d", name, ctl_port);
    for (;;) {
        struct pollfd pf[3] = {{tci, POLLIN, 0}, {lsn, POLLIN, 0}, {ctl, POLLIN, 0}};
        poll(pf, ctl >= 0 ? 3 : 2, 20);
        if (pf[0].revents)
            tci_read();
        if (pf[1].revents & POLLIN) {
            int c = accept(lsn, NULL, NULL);
            if (c >= 0 && ctl >= 0) {
                /* one Mac at a time: a second connection (a port probe, another harness) is turned away, not let
                 * in over the one running the link (2026-11-21: a probe took the place of the harness and stopped
                 * the X6100's end mid-transfer) */
                const char *busy = "{\"error\":\"another controller is connected\"}\n";
                send(c, busy, strlen(busy), MSG_NOSIGNAL);
                close(c);
                note("a second connection turned away");
            } else if (c >= 0) {
                ctl = c;
                setsockopt(ctl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                fcntl(ctl, F_SETFL, fcntl(ctl, F_GETFL) | O_NONBLOCK);
                note("the Mac connected");
            }
        }
        if (ctl >= 0 && (pf[2].revents & (POLLIN | POLLHUP | POLLERR)))
            ctl_read();
        uint64_t t = now_ms();
        /* keyed while the link is, and until what it made has gone out */
        if (running && (link_keyed(&L) || txq_len() > 0) && !may_key())
            stop_all("the Mac took the right to transmit away mid-transmission");     /* off at once, and stopped */
        bool want = running && may_key() && (link_keyed(&L) || txq_len() > 0);
        key(want);
        if (!want && !link_keyed(&L))
            txq_head = txq_tail = 0;
        if (radio_tx && t - radio_tx_since > MAX_KEY_S * 1000) {
            note("on the air over %.0f s: unkeyed", MAX_KEY_S);
            stop_all("on the air too long");
        }
        if (ctl >= 0 && t - last_tel >= 100) {
            last_tel = t;
            telemetry();
        }
    }
}
