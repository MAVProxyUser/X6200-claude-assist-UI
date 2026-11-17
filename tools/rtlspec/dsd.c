/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - dsd-fme on B's narrow FM channel (dsd.h). Its own thread starts it, reads what it hears and takes its
 *  speech; the stick's thread only writes the channel's samples into its stdin, never waiting (a full pipe loses
 *  them: dsd-fme reads 48 kHz at a few per cent of a core, so only a stopped one fills it).
 */

#define _GNU_SOURCE
#include "dsd.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define UDP_PORT 23470          /* its speech (-o udp:127.0.0.1:23470): 8 kHz, s16, two channels (its -fa's) */
#define IN_RATE  44100.0
#define OUT_RATE 48000.0
#define CHUNK    960            /* 20 ms at 48 kHz a write */
#define PCM      8000           /* a second of its 8 kHz speech in hand at most */
#define START    480            /* 60 ms of it before it is played */
#define GAIN     1.5f           /* its speech peaks at half scale on B (D-STAR, cabled test): DMR's level */

extern char **environ;

static dsd_heard_fn    heard_cb;
static const char     *path;
static pthread_mutex_t in_lock = PTHREAD_MUTEX_INITIALIZER;     /* in_fd closed under it: never written closed */
static int             in_fd   = -1;                           /* dsd-fme's stdin, -1 none running */
static atomic_uint     fed_ms;                                 /* the last sample given (each 23 ms) */
static atomic_uint     sync_ms, sync2_ms, sync3_ms;            /* the last three syncs it said */

static pthread_mutex_t pcm_lock = PTHREAD_MUTEX_INITIALIZER;
static int16_t         pcm[PCM];
static uint32_t        pcm_w, pcm_r;
static bool            playing;

static uint32_t ms_now(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000);
}

/* ---- the feed: the channel's FM, its carrier's offset taken off (a radio 2 kHz off its frequency moves the whole
 * signal off centre, and dsd-fme found no sync then: tested 2026-11-16), at 48 kHz, +-5 kHz as +-16000 ---- */
void dsd_feed(float d) {
    static float    dc, prev;
    static double   ph;
    static int16_t  buf[CHUNK];
    static int      n;
    static uint32_t tick;

    if ((++tick & 1023) == 0) atomic_store(&fed_ms, ms_now());
    if (pthread_mutex_trylock(&in_lock)) return;    /* being closed: this sample lost */
    if (in_fd < 0) {
        pthread_mutex_unlock(&in_lock);
        n = 0;
        return;
    }
    dc += 0.0001f * (d - dc);
    d -= dc;
    while (ph < 1.0) {                  /* 48 kHz between the 44.1 kHz samples, a straight line */
        int s = (int)lrintf((prev + (d - prev) * (float)ph) * 16000);

        buf[n++] = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
        ph += IN_RATE / OUT_RATE;
        if (n == CHUNK) {
            (void)!write(in_fd, buf, sizeof(buf));  /* non-blocking: full (it stopped), lost */
            n = 0;
        }
    }
    ph -= 1.0;
    prev = d;
    pthread_mutex_unlock(&in_lock);
}

/* Three syncs within 1.2 s, the last within 1.3 s: a digital signal. FM's noise gave a lone sync now and then (YSF,
 * D-STAR: 2026-11-17, a dozen in an hour and a half on 441.000), never three so close; dsd-fme says a sync each
 * 100 ms at most. A call it decoded (its identity through the codes) is as good as three: some (DMR, and others that
 * keep their frames in a loop of their own) say a sync only at the first frame, and a call each second */
bool dsd_active(void) {
    uint32_t t = ms_now(), a = atomic_load(&sync_ms), c = atomic_load(&sync3_ms);

    return a && c && t - a < 1300 && a - c < 1200;
}

static void synced(uint32_t t, bool call) {
    if (call) {
        atomic_store(&sync3_ms, t);
        atomic_store(&sync2_ms, t);
    } else {
        atomic_store(&sync3_ms, atomic_load(&sync2_ms));
        atomic_store(&sync2_ms, atomic_load(&sync_ms));
    }
    atomic_store(&sync_ms, t);
}

float dsd_audio(void) {
    static double ph;
    static float  a, b;
    float         v;

    pthread_mutex_lock(&pcm_lock);
    if (!playing && pcm_w - pcm_r >= START) playing = true;
    if (!playing) {
        pthread_mutex_unlock(&pcm_lock);
        return 0;
    }
    ph += 8000.0 / IN_RATE;
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
    pthread_mutex_unlock(&pcm_lock);
    return v;
}

/* Its speech, a datagram: 8 kHz, s16, two channels the same (FDMA) or a TDMA system's two slots, mixed */
static void speech(const int16_t *s, int bytes) {
    int frames = bytes / 4;

    pthread_mutex_lock(&pcm_lock);
    for (int i = 0; i < frames; i++) {
        if (pcm_w - pcm_r >= PCM) pcm_r++;  /* a second behind: the oldest dropped */
        pcm[pcm_w++ % PCM] = (int16_t)((s[2 * i] + s[2 * i + 1]) / 2);
    }
    pthread_mutex_unlock(&pcm_lock);
}

/* ---- what it says ---- */

/* "P25p1 TGT: 00001234; SRC: 05678901; NAC: 293; Group; " -> "TG 1234 FROM 5678901 NAC 293"; YSF's and D-STAR's
 * callsigns as they are ("TGT: CQCQCQ   SRC: N0CALL  /ID51" -> "TO CQCQCQ FROM N0CALL /ID51") */
static void pretty(const char *ev, char *out, size_t max) {
    char        tgt[64] = "", src[64] = "", extra[96] = "", word[64], *val = NULL;
    const char *p       = ev;
    bool        group = false, enc = false, emerg = false, first = true;
    size_t      vmax = 0;
    char        key[16] = "";

    while (*p) {
        size_t n = 0;

        while (*p == ' ') p++;
        if (!*p) break;
        while (*p && *p != ' ' && *p != ';' && n < sizeof(word) - 1) word[n++] = *p++;
        word[n] = 0;
        bool semi = *p == ';';

        if (*p == ';') p++;
        if (first) {                    /* the system's name: said by the sync */
            first = false;
            continue;
        }
        if (n && word[n - 1] == ':') {  /* a key: its value the words after it, to a ';' or the next key */
            word[n - 1] = 0;
            snprintf(key, sizeof(key), "%s", word);
            val = !strcmp(key, "TGT") ? tgt : !strcmp(key, "SRC") ? src : NULL;
            vmax = val == tgt ? sizeof(tgt) : sizeof(src);
            if (!val && (!strcmp(key, "NAC") || !strcmp(key, "RAN") || !strcmp(key, "CC") || !strcmp(key, "CAN"))) {
                val  = extra + strlen(extra);
                vmax = sizeof(extra) - strlen(extra);
                snprintf(val, vmax, "%s%s ", *extra ? " " : "", key);
                val += strlen(val);
                vmax = sizeof(extra) - strlen(extra);
            } else if (!val) {
                key[0] = 0;
            }
            continue;
        }
        if (key[0] && val) {            /* a value's word */
            const char *w = word;

            if (*w && strspn(w, "0123456789") == strlen(w))
                while (w[0] == '0' && w[1]) w++;    /* 00001234: 1234 */
            snprintf(val + strlen(val), vmax - strlen(val), "%s%s", *val && val[strlen(val) - 1] != ' ' ? " " : "", w);
        } else if (!strcasecmp(word, "Group")) {
            group = true;
        } else if (!strcasecmp(word, "ENC")) {
            enc = true;
        } else if (!strcasecmp(word, "Emergency")) {
            emerg = true;
        }
        if (semi) key[0] = 0, val = NULL;
    }
    snprintf(out, max, "%s %s FROM %s%s%s%s%s", group ? "TG" : "TO", *tgt ? tgt : "?", *src ? src : "?",
             *extra ? " " : "", extra, enc ? " ENCRYPTED" : "", emerg ? " EMERGENCY" : "");
    for (char *c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
}

static char     proto[16], call[2][160], text[2][48];
static uint32_t call_ms[2], said_ms;

/* The call, its text message after it in quotes (D-STAR's slow data, YSF's, a talker alias) */
static void say(int slot) {
    char c[220];

    snprintf(c, sizeof(c), call[slot][0] && text[slot][0] ? "%s  \"%s\"" : "%s", call[slot], text[slot]);
    if (heard_cb) heard_cb(proto, slot, c);
}

/* Two timeslots (or a second channel): dsd-fme's second slot means something only there */
static bool tdma(void) {
    return !strcmp(proto, "P25P2") || !strncmp(proto, "X2", 2) || !strcmp(proto, "DMR");
}

static void line(char *l) {
    uint32_t t = ms_now();

    for (const char *q = l; *q; q++)    /* a garbled one (a header decoded out of noise): not taken */
        if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7E) return;

    if (!strncmp(l, "SY ", 3) && strlen(l) > 5) {   /* "SY 0 +P25p1 ": the protocol, its polarity off */
        char        p[16];
        const char *s = l + 5;
        size_t      n = 0;

        if (*s == '+' || *s == '-') s++;
        while (*s && *s != ' ' && n < sizeof(p) - 1) p[n++] = (char)toupper((unsigned char)*s++);
        p[n] = 0;
        if (!strncmp(p, "DSTAR", 5)) p[5] = 0;     /* DSTAR_HD, its header: D-STAR */
        synced(t, false);
        if (dsd_active() && strcmp(p, proto)) {
            snprintf(proto, sizeof(proto), "%s", p);
            say(0);
        }
    } else if (!strncmp(l, "TX ", 3) && (l[3] == '0' || l[3] == '1') && l[4] == ' ') {
        int    s = l[3] - '0';
        char   x[48];
        size_t n = 0;

        if (s && !tdma()) return;
        for (const char *q = l + 5; *q && n < sizeof(x) - 1; q++)    /* printable, upper case, spaces as one */
            if (isprint((unsigned char)*q) && *q != '"' && !(*q == ' ' && (!n || x[n - 1] == ' ')))
                x[n++] = (char)toupper((unsigned char)*q);
        while (n && x[n - 1] == ' ') n--;
        x[n] = 0;
        static char seen[2][48];        /* taken when said twice running: a superframe read out of step (the first
                                         * after a header) gives a garbled one once */
        bool        twice = !strcmp(x, seen[s]);

        snprintf(seen[s], sizeof(seen[s]), "%s", x);
        if (twice && strcmp(x, text[s])) {
            snprintf(text[s], sizeof(text[s]), "%s", x);
            if (*x) fprintf(stderr, "rtlspec: dsd-fme %s%s says \"%s\"\n", proto, s ? " SLOT 2" : "", x);
            if (*proto && call[s][0]) say(s);
        }
    } else if (!strncmp(l, "EV ", 3) && (l[3] == '0' || l[3] == '1') && l[4] == ' ') {
        int         s = l[3] - '0';
        char        c[160];
        static char logged[2][64];

        if (s && *proto && !tdma()) return;     /* D-STAR, YSF, P25 phase 1: one channel, said twice */
        pretty(l + 5, c, sizeof(c));
        if (!strncmp(call[s], "TG ", 3) && !strncmp(c, "TO ", 3) && !strcmp(call[s] + 3, c + 3))
            memcpy(c, "TG", 2);         /* a group call said without its "Group" (some of its reports): still one */
        call_ms[s] = t;
        synced(t, true);
        if (!*proto && dsd_active()) {  /* no sync said (DMR's): the call's own system ("DMR TGT: ...") */
            size_t n = strcspn(l + 5, " ");

            snprintf(proto, sizeof(proto), "%.*s", (int)(n < sizeof(proto) ? n : sizeof(proto) - 1), l + 5);
            for (char *q = proto; *q; q++) *q = (char)toupper((unsigned char)*q);
            say(0);
        }
        if (strcmp(c, call[s])) {
            size_t w = 0;               /* the log: who to whom ("TG 1234 FROM 5678"), not each change of the rest */

            for (int k = 0; k < 4 && c[w]; k++) w += strcspn(c + w, " "), w += c[w] == ' ';
            snprintf(call[s], sizeof(call[s]), "%s", c);
            if (strncmp(logged[s], c, w) || strlen(logged[s]) != w) {
                snprintf(logged[s], sizeof(logged[s]), "%.*s", (int)(w < sizeof(logged[s]) ? w : sizeof(logged[s]) - 1), c);
                fprintf(stderr, "rtlspec: dsd-fme %s%s %s\n", proto, s ? " SLOT 2" : "", c);
            }
            if (*proto) say(s);
        }
    }
}

/* ---- dsd-fme itself ---- */
static pid_t spawn(int *st_rd) {
    int         in[2], st[2];
    pid_t       pid;
    const char *log = getenv("RTLSPEC_DSD_LOG");    /* its own console, for a test: /dev/null else */
    char       *argv[] = {(char *)path, "-fa", "-i", "-", "-o", "udp:127.0.0.1:23470", NULL};
    char       *envp[256];
    int         e = 0;

    for (char **v = environ; *v && e < 250; v++)
        if (strncmp(*v, "DSD_STATUS_FD=", 14) && strncmp(*v, "DSD_NO_DMR=", 11)) envp[e++] = *v;
    envp[e++] = "DSD_STATUS_FD=3";
    envp[e++] = "DSD_NO_DMR=1";                     /* DMR: dmr.c's, with the vocoder's DMR stream */
    envp[e]   = NULL;
    if (pipe2(in, O_CLOEXEC)) return 0;
    if (pipe2(st, O_CLOEXEC)) {
        close(in[0]);
        close(in[1]);
        return 0;
    }
    if ((pid = fork()) == 0) {          /* the child: only what is safe after fork, then dsd-fme */
        int nul = open("/dev/null", O_RDWR);
        int lf  = log ? open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;

        dup2(in[0], 0);
        dup2(nul, 1);
        dup2(lf >= 0 ? lf : nul, 2);
        dup2(st[1], 3);
        for (int f = 4; f < 1024; f++) close(f);    /* none of the daemon's (its client, the stick) */
        execve(path, argv, envp);
        _exit(127);
    }
    close(in[0]);
    close(st[1]);
    if (pid < 0) {
        close(in[1]);
        close(st[0]);
        return 0;
    }
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    fcntl(st[0], F_SETFL, O_NONBLOCK);
    pthread_mutex_lock(&in_lock);
    in_fd = in[1];
    pthread_mutex_unlock(&in_lock);
    *st_rd = st[0];
    fprintf(stderr, "rtlspec: dsd-fme %d on B's narrow FM (P25, D-STAR, YSF, X2-TDMA; DMR is dmr.c's)\n", (int)pid);
    return pid;
}

static void ended(int *st_rd) {
    atomic_store(&sync3_ms, 0);
    pthread_mutex_lock(&in_lock);
    if (in_fd >= 0) close(in_fd);
    in_fd = -1;
    pthread_mutex_unlock(&in_lock);
    if (*st_rd >= 0) close(*st_rd);
    *st_rd = -1;
    atomic_store(&sync_ms, 0);
    atomic_store(&sync2_ms, 0);
}

static void *run(void *arg) {
    int                udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), st = -1, gone_n = 0;
    struct sockaddr_in a   = {.sin_family = AF_INET, .sin_port = htons(UDP_PORT)};
    pid_t              pid = 0;
    uint32_t           gone_ms = 0;
    char               lb[2048];
    size_t             ln = 0;

    (void)arg;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (udp >= 0 && bind(udp, (struct sockaddr *)&a, sizeof(a))) {
        fprintf(stderr, "rtlspec: dsd-fme's speech port 127.0.0.1:%d taken: its calls silent\n", UDP_PORT);
        close(udp);
        udp = -1;
    }
    for (;;) {
        uint32_t t = ms_now(), f = atomic_load(&fed_ms);
        bool     want = f && t - f < 2000;
        int      s;

        if (!pid && want && (!gone_ms || t - gone_ms > (gone_n > 3 ? 60000u : 5000u))) {
            pid = spawn(&st);
            ln  = 0;
        }
        if (pid && f && t - f > 60000) {   /* a minute with no narrow FM to listen to: ended, its memory back */
            fprintf(stderr, "rtlspec: dsd-fme ended (no narrow FM for a minute)\n");
            kill(pid, SIGTERM);
            waitpid(pid, &s, 0);
            ended(&st);
            pid    = 0;
            gone_n = 0;
        }
        if (pid && waitpid(pid, &s, WNOHANG) == pid) {
            fprintf(stderr, "rtlspec: dsd-fme %d ended by itself (%s %d): again in %s\n", (int)pid,
                    WIFSIGNALED(s) ? "signal" : "status", WIFSIGNALED(s) ? WTERMSIG(s) : WEXITSTATUS(s),
                    gone_n >= 3 ? "a minute" : "5 s");
            ended(&st);
            pid     = 0;
            gone_ms = t;
            gone_n++;
        }

        struct pollfd p[2] = {{.fd = st, .events = POLLIN}, {.fd = udp, .events = POLLIN}};

        if (poll(p, 2, 200) > 0) {
            if (st >= 0 && (p[0].revents & (POLLIN | POLLHUP))) {
                ssize_t k;

                while ((k = read(st, lb + ln, sizeof(lb) - 1 - ln)) > 0) {
                    char *nl;

                    ln += (size_t)k;
                    lb[ln] = 0;
                    while ((nl = strchr(lb, '\n'))) {
                        *nl = 0;
                        line(lb);
                        ln -= (size_t)(nl + 1 - lb);
                        memmove(lb, nl + 1, ln + 1);
                    }
                    if (ln >= sizeof(lb) - 1) ln = 0;   /* a line too long: dropped */
                }
            }
            if (udp >= 0 && (p[1].revents & POLLIN)) {
                int16_t b[1024];
                ssize_t k;

                while ((k = recv(udp, b, sizeof(b), MSG_DONTWAIT)) > 0) speech(b, (int)k);
            }
        }

        /* what is heard, gone: a call 2 s after its last word, the protocol 1.5 s after its last sync; said again
         * each second while it goes on */
        t = ms_now();
        for (int i = 0; i < 2; i++)
            if (call[i][0] && t - call_ms[i] > 2000) {
                call[i][0] = text[i][0] = 0;
                say(i);
            }
        if (proto[0] && !dsd_active() && t - atomic_load(&sync_ms) > 1500) {
            proto[0] = call[0][0] = call[1][0] = text[0][0] = text[1][0] = 0;
            say(0);
        }
        if (proto[0] && t - said_ms >= 1000) {
            said_ms = t;
            say(0);
            if (call[1][0]) say(1);
        }
    }
    return NULL;
}

bool dsd_init(dsd_heard_fn heard) {
    pthread_t   th;
    const char *p = getenv("RTLSPEC_DSD");

    if (getenv("RTLSPEC_NO_DSD")) return false;
    path = p && *p ? p : "/usr/bin/dsd-fme";
    if (access(path, X_OK)) {
        fprintf(stderr, "rtlspec: no %s: P25, D-STAR and YSF not decoded (DMR is)\n", path);
        return false;
    }
    heard_cb = heard;
    return pthread_create(&th, NULL, run, NULL) == 0;
}
