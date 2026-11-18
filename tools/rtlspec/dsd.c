/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - dsd-fme on B's narrow FM channel (dsd.h): three of it, as no one mode of it hears all, each fed the
 *  same samples: -fa (P25, D-STAR, YSF, X2-TDMA; DMR left to dmr.c), -fi (NXDN48) and -fn (NXDN96: NXDN is not in -fa,
 *  its sync too short to look for beside the others). Its own thread starts them, reads what each hears and takes the
 *  speech of the one hearing a digital signal; the stick's thread only writes the samples into their stdins, never
 *  waiting (a full pipe loses them: each reads 48 kHz at a few per cent of a core, so only a stopped one fills it).
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

#define IN_RATE  44100.0
#define OUT_RATE 48000.0
#define CHUNK    960            /* 20 ms at 48 kHz a write */
#define PCM      8000           /* a second of its 8 kHz speech in hand at most */
#define START    800            /* 100 ms of it before it is played: dsd-fme's speech can come a YSF frame at a time
                                 * (five voice frames each 100 ms). Over the cable B's speaker followed dsd-fme's own
                                 * output second by second (r 0.94-1.00), 1% slower: the GUI's playback clock */
#define GAIN     1.5f           /* its speech peaks at half scale on B (D-STAR, cabled test): DMR's level */

extern char **environ;

/* One dsd-fme: its mode, its speech's port and channels, and what it says */
typedef struct {
    const char     *mode, *what;
    int             port, channels;
    bool            calls_only;     /* a decoded call makes the channel digital, not syncs: NXDN's 20-bit sync turns
                                     * up in FM noise far too often to count */
    pthread_mutex_t in_lock;        /* in_fd closed under it: never written closed */
    int             in_fd, st, udp;
    pid_t           pid;
    char            lb[2048];
    size_t          ln;
    uint32_t        gone_ms;
    int             gone_n;
    atomic_uint     sync_ms, sync2_ms, sync3_ms;    /* the last three syncs (or calls) it said */
    char            proto[16], call[2][160], text[2][48], seen[2][48], logged[2][64];
    uint32_t        call_ms[2];
} kid_t;

static kid_t kids[] = {
    {.mode = "-fa", .what = "P25, D-STAR, YSF, X2-TDMA; DMR is dmr.c's", .port = 23470, .channels = 2,
     .in_lock = PTHREAD_MUTEX_INITIALIZER, .in_fd = -1, .st = -1, .udp = -1},
    {.mode = "-fi", .what = "NXDN48", .port = 23471, .channels = 1, .calls_only = true,
     .in_lock = PTHREAD_MUTEX_INITIALIZER, .in_fd = -1, .st = -1, .udp = -1},
    {.mode = "-fn", .what = "NXDN96", .port = 23472, .channels = 1, .calls_only = true,
     .in_lock = PTHREAD_MUTEX_INITIALIZER, .in_fd = -1, .st = -1, .udp = -1},
};
#define KIDS (int)(sizeof(kids) / sizeof(kids[0]))

static dsd_heard_fn    heard_cb;
static const char     *path;
static atomic_uint     fed_ms;      /* the last sample given (each 23 ms) */
static atomic_int      shown = -1;  /* the one whose call is said and whose speech is played, -1 none */
static uint32_t        said_ms;

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
 * signal off centre, and dsd-fme found no sync then: tested 2026-11-16), at 48 kHz, +-5 kHz as +-16000, to each ---- */
void dsd_feed(float d) {
    static float    dc, prev;
    static double   ph;
    static int16_t  buf[CHUNK];
    static int      n;
    static uint32_t tick;

    if ((++tick & 1023) == 0) atomic_store(&fed_ms, ms_now());
    dc += 0.0001f * (d - dc);
    d -= dc;
    while (ph < 1.0) {                  /* 48 kHz between the 44.1 kHz samples, a straight line */
        int s = (int)lrintf((prev + (d - prev) * (float)ph) * 16000);

        buf[n++] = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
        ph += IN_RATE / OUT_RATE;
        if (n == CHUNK) {
            for (int k = 0; k < KIDS; k++) {
                if (pthread_mutex_trylock(&kids[k].in_lock)) continue;     /* being closed: this chunk lost */
                if (kids[k].in_fd >= 0) (void)!write(kids[k].in_fd, buf, sizeof(buf));    /* full: lost */
                pthread_mutex_unlock(&kids[k].in_lock);
            }
            n = 0;
        }
    }
    ph -= 1.0;
    prev = d;
}

/* Three syncs within 1.2 s, the last within 1.3 s: a digital signal. FM's noise gave a lone sync now and then (YSF,
 * D-STAR: 2026-11-17, a dozen in an hour and a half on 441.000), never three so close; dsd-fme says a sync each
 * 100 ms at most. A call it decoded (its identity through the codes) is as good as three: some (DMR, and others that
 * keep their frames in a loop of their own) say a sync only at the first frame, and a call each second */
static bool kid_active(kid_t *k) {
    uint32_t t = ms_now(), a = atomic_load(&k->sync_ms), c = atomic_load(&k->sync3_ms);

    return a && c && t - a < 1300 && a - c < 1200;
}

/* Asked for each 44.1 kHz sample, so a flag: set by dsd.c's thread each 50 ms (and at each line it reads). Each
 * instance's test reads the clock, and three of them a sample took 27% of a core (2026-11-17: the daemon 72% with
 * them, 45% without) */
static atomic_bool any_active;

static void update_active(void) {
    bool a = false;

    for (int k = 0; k < KIDS && !a; k++) a = kid_active(&kids[k]);
    atomic_store(&any_active, a);
}

bool dsd_active(void) {
    return atomic_load(&any_active);
}

static void synced(kid_t *k, uint32_t t, bool call) {
    if (call) {
        atomic_store(&k->sync3_ms, t);
        atomic_store(&k->sync2_ms, t);
    } else {
        atomic_store(&k->sync3_ms, atomic_load(&k->sync2_ms));
        atomic_store(&k->sync2_ms, atomic_load(&k->sync_ms));
    }
    atomic_store(&k->sync_ms, t);
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
        } else {                        /* out: silent till the next 100 ms */
            b       = 0;
            playing = false;
        }
    }
    v = (a + (b - a) * (float)ph) * GAIN;
    pthread_mutex_unlock(&pcm_lock);
    return v;
}

/* Its speech, a datagram: 8 kHz, s16; -fa's two channels the same (FDMA) or a TDMA system's two slots, mixed; NXDN's
 * one (its modes' own: read as two, it played at twice the speed) */
static void speech(kid_t *k, const int16_t *s, int bytes) {
    int frames = bytes / (2 * k->channels);

    pthread_mutex_lock(&pcm_lock);
    for (int i = 0; i < frames; i++) {
        if (pcm_w - pcm_r >= PCM) pcm_r++;  /* a second behind: the oldest dropped */
        pcm[pcm_w++ % PCM] = (int16_t)(k->channels == 2 ? (s[2 * i] + s[2 * i + 1]) / 2 : s[i]);
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
        if (key[0] && val && val > extra && val <= extra + sizeof(extra) && !strcmp(word, "-1")) {
            /* NAC, RAN, CC not known yet (dsd-fme's -1: an NXDN call's first frames): the key taken off again */
            char *k0 = strrchr(extra, ' ') ? extra + strlen(extra) - strlen(key) - 1 : extra;

            if (k0 < extra) k0 = extra;
            while (k0 > extra && k0[-1] == ' ') k0--;
            *k0 = 0;
            key[0] = 0, val = NULL;
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

/* The call, its text message after it in quotes (D-STAR's slow data, YSF's, a talker alias): said for the one shown */
static void say(kid_t *k, int slot) {
    char c[220];

    if (atomic_load(&shown) != k - kids) return;
    snprintf(c, sizeof(c), k->call[slot][0] && k->text[slot][0] ? "%s  \"%s\"" : "%s", k->call[slot], k->text[slot]);
    if (heard_cb) heard_cb(k->proto, slot, c);
}

/* Shown: the first hearing a digital signal, while it does */
static void show_if_active(kid_t *k) {
    int s = atomic_load(&shown);

    if (s == k - kids || !kid_active(k) || (s >= 0 && kid_active(&kids[s]))) return;
    atomic_store(&shown, (int)(k - kids));
    if (k->proto[0]) say(k, 0);
}

/* Two timeslots (or a second channel): dsd-fme's second slot means something only there */
static bool tdma(kid_t *k) {
    return !strcmp(k->proto, "P25P2") || !strncmp(k->proto, "X2", 2) || !strcmp(k->proto, "DMR");
}

static void line(kid_t *k, char *l) {
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
        if (!k->calls_only) synced(k, t, false);
        else if (kid_active(k)) synced(k, t, false);    /* NXDN: its syncs keep a call going, never start one */
        show_if_active(k);
        if (kid_active(k) && strcmp(p, k->proto)) {
            snprintf(k->proto, sizeof(k->proto), "%s", p);
            say(k, 0);
        }
    } else if (!strncmp(l, "TX ", 3) && (l[3] == '0' || l[3] == '1') && l[4] == ' ') {
        int    s = l[3] - '0';
        char   x[48];
        size_t n = 0;

        if (s && !tdma(k)) return;
        for (const char *q = l + 5; *q && n < sizeof(x) - 1; q++)    /* printable, upper case, spaces as one */
            if (isprint((unsigned char)*q) && *q != '"' && !(*q == ' ' && (!n || x[n - 1] == ' ')))
                x[n++] = (char)toupper((unsigned char)*q);
        while (n && x[n - 1] == ' ') n--;
        x[n] = 0;
        bool twice = !strcmp(x, k->seen[s]);    /* taken when said twice running: a superframe read out of step (the
                                                 * first after a header) gives a garbled one once */

        snprintf(k->seen[s], sizeof(k->seen[s]), "%s", x);
        if (twice && strcmp(x, k->text[s])) {
            snprintf(k->text[s], sizeof(k->text[s]), "%s", x);
            if (*x) fprintf(stderr, "rtlspec: dsd-fme %s%s says \"%s\"\n", k->proto, s ? " SLOT 2" : "", x);
            if (*k->proto && k->call[s][0]) say(k, s);
        }
    } else if (!strncmp(l, "EV ", 3) && (l[3] == '0' || l[3] == '1') && l[4] == ' ') {
        int  s = l[3] - '0';
        char c[160];

        if (s && *k->proto && !tdma(k)) return;     /* D-STAR, YSF, P25 phase 1, NXDN: one channel, said twice */
        pretty(l + 5, c, sizeof(c));
        if (!strncmp(k->call[s], "TG ", 3) && !strncmp(c, "TO ", 3) && !strcmp(k->call[s] + 3, c + 3))
            memcpy(c, "TG", 2);         /* a group call said without its "Group" (some of its reports): still one */
        k->call_ms[s] = t;
        synced(k, t, true);
        show_if_active(k);
        if (!*k->proto) {               /* no sync said yet (DMR's, an NXDN call's first): the call's own system */
            size_t n = strcspn(l + 5, " ");

            snprintf(k->proto, sizeof(k->proto), "%.*s", (int)(n < sizeof(k->proto) ? n : sizeof(k->proto) - 1), l + 5);
            for (char *q = k->proto; *q; q++) *q = (char)toupper((unsigned char)*q);
            say(k, 0);
        }
        if (strcmp(c, k->call[s])) {
            size_t w = 0;               /* the log: who to whom ("TG 1234 FROM 5678"), not each change of the rest */

            for (int j = 0; j < 4 && c[w]; j++) w += strcspn(c + w, " "), w += c[w] == ' ';
            snprintf(k->call[s], sizeof(k->call[s]), "%s", c);
            if (strncmp(k->logged[s], c, w) || strlen(k->logged[s]) != w) {
                snprintf(k->logged[s], sizeof(k->logged[s]), "%.*s",
                         (int)(w < sizeof(k->logged[s]) ? w : sizeof(k->logged[s]) - 1), c);
                fprintf(stderr, "rtlspec: dsd-fme %s%s %s\n", k->proto, s ? " SLOT 2" : "", c);
            }
            if (*k->proto) say(k, s);
        }
    }
}

/* ---- dsd-fme itself ---- */
static pid_t spawn(kid_t *k) {
    int         in[2], st[2];
    pid_t       pid;
    const char *log = getenv("RTLSPEC_DSD_LOG");    /* -fa's own console, for a test: /dev/null else */
    char        out[40];
    char       *envp[256];
    int         e = 0;

    snprintf(out, sizeof(out), "udp:127.0.0.1:%d", k->port);
    char *argv[] = {(char *)path, (char *)k->mode, "-i", "-", "-o", out, NULL};

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
        int lf  = log && k == kids ? open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;

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
    pthread_mutex_lock(&k->in_lock);
    k->in_fd = in[1];
    pthread_mutex_unlock(&k->in_lock);
    k->st = st[0];
    k->ln = 0;
    fprintf(stderr, "rtlspec: dsd-fme %d %s on B's narrow FM (%s)\n", (int)pid, k->mode, k->what);
    return pid;
}

static void ended(kid_t *k) {
    atomic_store(&k->sync3_ms, 0);
    pthread_mutex_lock(&k->in_lock);
    if (k->in_fd >= 0) close(k->in_fd);
    k->in_fd = -1;
    pthread_mutex_unlock(&k->in_lock);
    if (k->st >= 0) close(k->st);
    k->st = -1;
    atomic_store(&k->sync_ms, 0);
    atomic_store(&k->sync2_ms, 0);
}

static void *run(void *arg) {
    (void)arg;
    for (int i = 0; i < KIDS; i++) {
        kid_t             *k = &kids[i];
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)k->port)};

        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        k->udp            = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (k->udp >= 0 && bind(k->udp, (struct sockaddr *)&a, sizeof(a))) {
            fprintf(stderr, "rtlspec: dsd-fme's speech port 127.0.0.1:%d taken: %s calls silent\n", k->port, k->mode);
            close(k->udp);
            k->udp = -1;
        }
    }
    for (;;) {
        uint32_t      t = ms_now(), f = atomic_load(&fed_ms);
        bool          want = f && t - f < 2000;
        struct pollfd p[2 * KIDS];
        int           s;

        for (int i = 0; i < KIDS; i++) {
            kid_t *k = &kids[i];

            if (!k->pid && want && (!k->gone_ms || t - k->gone_ms > (k->gone_n > 3 ? 60000u : 5000u)))
                k->pid = spawn(k);
            if (k->pid && f && t - f > 60000) {    /* a minute with no narrow FM to listen to: ended, memory back */
                fprintf(stderr, "rtlspec: dsd-fme %s ended (no narrow FM for a minute)\n", k->mode);
                kill(k->pid, SIGTERM);
                waitpid(k->pid, &s, 0);
                ended(k);
                k->pid    = 0;
                k->gone_n = 0;
            }
            if (k->pid && waitpid(k->pid, &s, WNOHANG) == k->pid) {
                fprintf(stderr, "rtlspec: dsd-fme %d %s ended by itself (%s %d): again in %s\n", (int)k->pid,
                        k->mode, WIFSIGNALED(s) ? "signal" : "status", WIFSIGNALED(s) ? WTERMSIG(s) : WEXITSTATUS(s),
                        k->gone_n >= 3 ? "a minute" : "5 s");
                ended(k);
                k->pid     = 0;
                k->gone_ms = t;
                k->gone_n++;
            }
            p[2 * i]     = (struct pollfd){.fd = k->st, .events = POLLIN};
            p[2 * i + 1] = (struct pollfd){.fd = k->udp, .events = POLLIN};
        }
        update_active();
        if (poll(p, 2 * KIDS, 50) > 0) {
            for (int i = 0; i < KIDS; i++) {
                kid_t *k = &kids[i];

                if (k->st >= 0 && (p[2 * i].revents & (POLLIN | POLLHUP))) {
                    ssize_t n;

                    while ((n = read(k->st, k->lb + k->ln, sizeof(k->lb) - 1 - k->ln)) > 0) {
                        char *nl;

                        k->ln += (size_t)n;
                        k->lb[k->ln] = 0;
                        while ((nl = strchr(k->lb, '\n'))) {
                            *nl = 0;
                            line(k, k->lb);
                            update_active();
                            k->ln -= (size_t)(nl + 1 - k->lb);
                            memmove(k->lb, nl + 1, k->ln + 1);
                        }
                        if (k->ln >= sizeof(k->lb) - 1) k->ln = 0;    /* a line too long: dropped */
                    }
                }
                if (k->udp >= 0 && (p[2 * i + 1].revents & POLLIN)) {
                    int16_t b[1024];
                    ssize_t n;

                    while ((n = recv(k->udp, b, sizeof(b), MSG_DONTWAIT)) > 0)
                        if (atomic_load(&shown) == i) speech(k, b, (int)n);    /* the others' (noise): dropped */
                }
            }
        }

        /* what is heard, gone: a call 2 s after its last word, the protocol 1.5 s after its last sync; said again
         * each second while it goes on */
        t = ms_now();
        for (int i = 0; i < KIDS; i++) {
            kid_t *k = &kids[i];

            for (int j = 0; j < 2; j++)
                if (k->call[j][0] && t - k->call_ms[j] > 2000) {
                    k->call[j][0] = k->text[j][0] = 0;
                    say(k, j);
                }
            if (k->proto[0] && !kid_active(k) && t - atomic_load(&k->sync_ms) > 1500) {
                k->proto[0] = k->call[0][0] = k->call[1][0] = k->text[0][0] = k->text[1][0] = 0;
                say(k, 0);
                if (atomic_load(&shown) == i) atomic_store(&shown, -1);
            }
        }
        update_active();
        s = atomic_load(&shown);
        if (s >= 0 && kids[s].proto[0] && t - said_ms >= 1000) {
            said_ms = t;
            say(&kids[s], 0);
            if (kids[s].call[1][0]) say(&kids[s], 1);
        }
    }
    return NULL;
}

bool dsd_init(dsd_heard_fn heard) {
    pthread_t   th;
    const char *p = getenv("RTLSPEC_DSD");

    if (getenv("RTLSPEC_NO_DSD")) return false;
    if ((p = getenv("RTLSPEC_DSD_PORT")) && atoi(p) > 1024)    /* a test daemon beside B's: its speech ports */
        for (int i = 0; i < KIDS; i++) kids[i].port = atoi(p) + i;
    p = getenv("RTLSPEC_DSD");
    path = p && *p ? p : "/usr/bin/dsd-fme";
    if (access(path, X_OK)) {
        fprintf(stderr, "rtlspec: no %s: P25, D-STAR, YSF and NXDN not decoded (DMR is)\n", path);
        return false;
    }
    heard_cb = heard;
    return pthread_create(&th, NULL, run, NULL) == 0;
}
