/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - ADS-B receiver (adsb_rx.h). Two USB receivers, both CDC-ACM serial devices streaming Mode-S frames
 *  as ASCII hex, "*8D4840D6202CC371C32CE0576098;" or with a 48-bit time stamp first, "@0000626E0AE8<frame>;":
 *  - GNS 5890 (04d8:f8e8): sends with no command (GNS's AdsbTest app only reads it). It calls itself
 *    vendor-specific (device class ff), so the kernel's cdc_acm does not take it by its interfaces: told its ID
 *    (new_id), it does, and /dev/ttyACMn appears. That lasts till the next boot; it is done here each time the stick
 *    is found without its port.
 *  - microADSB (sprut's adsbPIC, 04d8:000a, "ADSB-PIC" in its product name: 04d8:000a alone is Microchip's generic
 *    serial ID): a plain CDC device, silent till started. The start of picadsb-multiplexer (smkrv), which is
 *    adsbScope's: "#00" (its firmware, "#00-00-0E-..."), "#43-00" (stop), "#51-01-00", "#37-03", "#51-00-00",
 *    "#37-03", "#38" (start reception), "#43-02" (all frames), a second after the port opens, each command's
 *    answer waited for as the multiplexer does (sent 300 ms apart, "#38" was missed and the stick stayed stopped).
 *    With "#38" it gave 0.65 frames/s against 0.28 with "#43" alone, more of them good. Its V.14 firmware
 *    time-stamps every frame whatever the mode. After 120 s without a frame, the whole start again.
 *  - RTL-SDR (an RTL2832U DVB-T stick, 0bda:2838 and the rest librtlsdr knows): no decoder of its own. dump1090-fa
 *    (FlightAware's; tools/rtlsdr) decodes it, headless, its raw output alone on 127.0.0.1:30002, "*8D...;" lines as
 *    the sticks send, read here over TCP. The stick itself is the Sentry's daemon's (x6x00-rtlspec, rtl_spectrum.c),
 *    which keeps it open for as long as the radio runs: dump1090 reads its samples from a pipe ("--ifile -"), the
 *    daemon writing them at 1090 MHz while AIRSPACE's screen is open, so the stick is never closed and opened for
 *    it. Without the daemon, dump1090 opens the stick itself (rtl-sdr-blog's librtlsdr built in, which knows the
 *    Blog V4's R828D too; adaptive gain). It ends with the GUI (PR_SET_PDEATHSIG) and is started again when the stick
 *    changes. The tuner ("R820T", "R828D") is the daemon's word, or said from dump1090's log, /tmp/dump1090.log.
 *  Which one, when more than one is on USB: the one AIRSPACE chose (adsb_rx_choose: its chooser, F4 LISTEN), else a
 *  GNS 5890, a microADSB, a spare RTL-SDR (one beside a HackRF or a bladeRF, which the daemon takes: dump1090 opens the
 *  stick itself, so B keeps its SDR, and it listens with AIRSPACE closed), then the daemon's SDR. With AIRSPACE closed
 *  the daemon's SDR is the Sentry's: a serial one listens meanwhile, if there is one.
 *  The thread looks for a receiver every second while it has none, and each second that the one it has is still there
 *  under the USB device number it had: pulled out, or plugged back (another number), or another receiver in its
 *  place, it is let go (its dump1090 ended) and looked for again. A serial port's read fails when it goes; dump1090 on
 *  the daemon's samples does not end when the stick does, it only waits, so the number is what says it.
 */

#define _GNU_SOURCE    /* pipe2, F_SETPIPE_SZ */
#include "adsb_rx.h"

#include "adsb.h"
#include "rtl_spectrum.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define USB_DEVICES "/sys/bus/usb/devices"
#define CDC_ACM     "/sys/bus/usb/drivers/cdc_acm"
#define QUIET_S     120
#define DUMP1090    "/usr/bin/dump1090-fa"
#define D1090_LOG   "/tmp/dump1090.log"
#define D1090_PORT  30002

static const struct {
    const char *vid, *pid, *product;    /* product: a part its USB product name must have, or NULL */
    const char *name;
    bool        new_id;                 /* cdc_acm told its ID */
    const char *init;                   /* sent on opening, each answered, and again after QUIET_S without a frame */
    bool        sdr;                    /* an SDR: dump1090 decodes it, from x6x00-rtlspec's samples (an RTL-SDR also
                                         * without: dump1090 opens it itself) */
} receivers[] = {
    {"04d8", "f8e8", NULL, "GNS 5890", true, NULL, false},
    {"04d8", "000a", "ADSB", "MICROADSB", false,
     "#00\r#43-00\r#51-01-00\r#37-03\r#51-00-00\r#37-03\r#43-00\r#38\r#43-02\r", false},
    /* RTL2832U sticks, librtlsdr's commonest IDs */
    {"0bda", "2838", NULL, "RTL-SDR", false, NULL, true},
    {"0bda", "2832", NULL, "RTL-SDR", false, NULL, true},
    {"0ccd", "00a9", NULL, "RTL-SDR", false, NULL, true},
    {"0ccd", "00b3", NULL, "RTL-SDR", false, NULL, true},
    {"0ccd", "00d3", NULL, "RTL-SDR", false, NULL, true},
    {"1554", "5020", NULL, "RTL-SDR", false, NULL, true},
    {"15f4", "0131", NULL, "RTL-SDR", false, NULL, true},
    {"185b", "0620", NULL, "RTL-SDR", false, NULL, true},
    {"185b", "0650", NULL, "RTL-SDR", false, NULL, true},
    {"1b80", "d393", NULL, "RTL-SDR", false, NULL, true},
    {"1b80", "d3a4", NULL, "RTL-SDR", false, NULL, true},
    {"1d19", "1101", NULL, "RTL-SDR", false, NULL, true},
    {"1d19", "1102", NULL, "RTL-SDR", false, NULL, true},
    {"1d19", "1103", NULL, "RTL-SDR", false, NULL, true},
    {"1f4d", "b803", NULL, "RTL-SDR", false, NULL, true},
    {"1f4d", "c803", NULL, "RTL-SDR", false, NULL, true},
    {"1f4d", "d803", NULL, "RTL-SDR", false, NULL, true},
    /* HackRF One, rad1o, Jawbreaker; bladeRF 2.0 micro (5250), bladeRF x40/x115: through x6x00-rtlspec only */
    {"1d50", "6089", NULL, "HACKRF", false, NULL, true},
    {"1d50", "cc15", NULL, "HACKRF", false, NULL, true},
    {"1d50", "604b", NULL, "HACKRF", false, NULL, true},
    {"2cf0", "5250", NULL, "BLADERF", false, NULL, true},
    {"2cf0", "5246", NULL, "BLADERF", false, NULL, true},
    {"1d50", "6066", NULL, "BLADERF", false, NULL, true},
};
#define N_RECEIVERS (int)(sizeof(receivers) / sizeof(receivers[0]))

/* Two at once, each on a thread of its own: a serial receiver (GNS 5890, microADSB) and an SDR through dump1090. Both
 * feed adsb.c (BOTH: the same aircraft heard by each, its messages counted from both) */
enum { SLOT_SERIAL, SLOT_SDR, SLOTS };

static atomic_bool     started;
static atomic_int      st[SLOTS] = {ADSB_RX_OFF, ADSB_RX_OFF};
static atomic_ullong   bytes;
static atomic_bool     lent;            /* the Sentry's SDR lent to it: adsb_rx_lend() */
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static char            dev_path[SLOTS][32], dev_name[SLOTS][16], dev_info[SLOTS][48];
static atomic_uint     want_mask;       /* adsb_rx_choose()'s, 0 the first by order in each slot */
static atomic_int      kind_now[SLOTS]; /* the one each listens on or is opening */

#define KB(k)   (1u << (k))
#define SERIALS (KB(ADSB_KIND_GNS) | KB(ADSB_KIND_MICRO))
#define SDRS    (KB(ADSB_KIND_RTL) | KB(ADSB_KIND_HACKRF) | KB(ADSB_KIND_BLADERF))

static adsb_kind_t kind_of(int r) {
    const char *n = receivers[r].name;

    return !strcmp(n, "GNS 5890")  ? ADSB_KIND_GNS
         : !strcmp(n, "MICROADSB") ? ADSB_KIND_MICRO
         : !strcmp(n, "HACKRF")    ? ADSB_KIND_HACKRF
         : !strcmp(n, "BLADERF")   ? ADSB_KIND_BLADERF
                                   : ADSB_KIND_RTL;
}

static adsb_kind_t kind_of_dev(rtl_dev_t d) {
    return d == RTL_DEV_HACKRF ? ADSB_KIND_HACKRF : d == RTL_DEV_BLADERF ? ADSB_KIND_BLADERF
         : d == RTL_DEV_RTL    ? ADSB_KIND_RTL    : ADSB_KIND_NONE;
}

/* The daemon's SDR (NONE: no daemon): its word, or the one found on USB before it said */
static rtl_dev_t daemon_dev(void) {
    return rtl_spectrum_daemon() ? rtl_spectrum_dev() : RTL_DEV_NONE;
}

/* An RTL-SDR beside the daemon's HackRF or bladeRF (B's choice, or its order): free for a dump1090 of its own */
static bool spare_of(unsigned present, rtl_dev_t dd) {
    return (present & KB(ADSB_KIND_RTL)) && (dd == RTL_DEV_HACKRF || dd == RTL_DEV_BLADERF);
}

/* Of the kinds on USB (present), those that can be listened on: the serial ones; with dump1090, the daemon's SDR (dd)
 * and an RTL-SDR beside it (a spare), or with no daemon an RTL-SDR (dump1090 opens it itself). Only what tunes 1090
 * MHz is here: the X6200 does not */
static unsigned usable_of(unsigned present, rtl_dev_t dd, bool dump) {
    unsigned u = present & SERIALS;

    if (!dump) return u;
    if (dd != RTL_DEV_NONE) u |= present & KB(kind_of_dev(dd));
    if (dd == RTL_DEV_NONE || spare_of(present, dd)) u |= present & KB(ADSB_KIND_RTL);
    return u;
}

/* What a slot listens on: the kind chosen for it (want) when one chosen can be listened on, NONE when the others
 * chosen can and its own not (the slot idle); none chosen can: the first by order (a GNS 5890, a microADSB; a spare
 * RTL-SDR, the daemon's SDR). *direct: a spare RTL-SDR (its own dump1090); *held_back: the daemon's SDR lent to the
 * Sentry (AIRSPACE closed) */
static adsb_kind_t pick_of(int slot, unsigned present, rtl_dev_t dd, bool dump, unsigned want, bool is_lent,
                           bool *direct, bool *held_back) {
    static const adsb_kind_t serial_order[] = {ADSB_KIND_GNS, ADSB_KIND_MICRO};
    unsigned    usable = usable_of(present, dd, dump), mine = usable & (slot == SLOT_SERIAL ? SERIALS : SDRS);
    adsb_kind_t k = ADSB_KIND_NONE;

    want &= usable;
    if (want) mine &= want;
    if (slot == SLOT_SERIAL) {
        for (int i = 0; i < 2 && !k; i++)
            if (mine & KB(serial_order[i])) k = serial_order[i];
    } else if ((mine & KB(ADSB_KIND_RTL)) && spare_of(present, dd)) {
        k = ADSB_KIND_RTL;                              /* a spare first: B keeps its SDR */
    } else if (dd != RTL_DEV_NONE && (mine & KB(kind_of_dev(dd)))) {
        k = kind_of_dev(dd);
    } else if (dd == RTL_DEV_NONE && (mine & KB(ADSB_KIND_RTL))) {
        k = ADSB_KIND_RTL;                              /* no daemon: dump1090 on the stick itself */
    }
    *direct    = k == ADSB_KIND_RTL && spare_of(present, dd);
    *held_back = slot == SLOT_SDR && k != ADSB_KIND_NONE && !*direct && dd != RTL_DEV_NONE && is_lent;
    return k;
}

static bool read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");

    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

static bool write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);

    if (fd < 0) return false;
    bool ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s);
    close(fd);
    return ok;
}

/* What is on USB, a bit each kind; the first of each kind: its row in receivers[] (-1 none) and USB device ("3-1") */
typedef struct {
    int  row;
    char usb[64];
} seen_t;

static unsigned scan_usb(seen_t seen[ADSB_KIND_N]) {
    DIR           *d = opendir(USB_DEVICES);
    struct dirent *e;
    unsigned       present = 0;

    for (int k = 0; k < ADSB_KIND_N; k++) seen[k].row = -1;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char path[300], v[8], p[8], prod[64];

        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;     /* interfaces have a ':' */
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idVendor", e->d_name);
        if (!read_line(path, v, sizeof(v))) continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idProduct", e->d_name);
        if (!read_line(path, p, sizeof(p))) continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/product", e->d_name);
        if (!read_line(path, prod, sizeof(prod))) prod[0] = 0;
        for (int r = 0; r < N_RECEIVERS; r++)
            if (!strcmp(v, receivers[r].vid) && !strcmp(p, receivers[r].pid) &&
                (!receivers[r].product || strstr(prod, receivers[r].product))) {
                adsb_kind_t k = kind_of(r);

                present |= KB(k);
                if (seen[k].row < 0) {
                    seen[k].row = r;
                    snprintf(seen[k].usb, sizeof(seen[k].usb), "%s", e->d_name);
                }
                break;
            }
    }
    closedir(d);
    return present;
}

/* What slot listens on, as things are on USB now (pick_of) */
static adsb_kind_t pick_slot(int slot, seen_t seen[ADSB_KIND_N], bool *direct, bool *held_back) {
    unsigned present = scan_usb(seen);

    return pick_of(slot, present, daemon_dev(), access(DUMP1090, X_OK) == 0, atomic_load(&want_mask),
                   atomic_load(&lent), direct, held_back);
}

/* A USB device's number ("3-1": its devnum), 0 when it is not there; plugged back, it has another */
static int usb_devnum(const char *usb) {
    char path[300], n[8];

    snprintf(path, sizeof(path), USB_DEVICES "/%s/devnum", usb);
    return read_line(path, n, sizeof(n)) ? atoi(n) : 0;
}

/* Its serial port, if cdc_acm has it: "<usb>:1.0/tty/ttyACMn" */
static bool find_tty(const char *usb, char *dev, size_t n) {
    char           path[300];
    DIR           *d;
    struct dirent *e;
    bool           found = false;

    snprintf(path, sizeof(path), USB_DEVICES "/%s:1.0/tty", usb);
    if (!(d = opendir(path))) return false;
    while (!found && (e = readdir(d)))
        if (!strncmp(e->d_name, "ttyACM", 6)) {
            snprintf(dev, n, "/dev/%s", e->d_name);
            found = true;
        }
    closedir(d);
    return found;
}

/* cdc_acm told the stick's ID; already told (since boot), its control interface bound instead */
static void bind_stick(int r, const char *usb) {
    char intf[64], id[16];

    snprintf(id, sizeof(id), "%s %s", receivers[r].vid, receivers[r].pid);
    if (write_str(CDC_ACM "/new_id", id)) return;
    snprintf(intf, sizeof(intf), "%s:1.0", usb);
    write_str(CDC_ACM "/bind", intf);
}

static int open_port(const char *dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        t.c_cflag |= CLOCAL | CREAD;
        cfsetspeed(&t, B115200);            /* the CDC device ignores the rate */
        tcsetattr(fd, TCSANOW, &t);
        tcflush(fd, TCIFLUSH);
    }
    return fd;
}

static void set_dev(int slot, const char *dev, const char *name) {
    pthread_mutex_lock(&dev_lock);
    snprintf(dev_path[slot], sizeof(dev_path[slot]), "%s", dev);
    snprintf(dev_name[slot], sizeof(dev_name[slot]), "%s", name);
    dev_info[slot][0] = 0;
    pthread_mutex_unlock(&dev_lock);
}

/* A receiver's answer to a command, "#00-00-0E-04-...": its firmware, said */
static void set_info(int slot, const char *line) {
    unsigned a, b, fw;

    if (sscanf(line, "#%x-%x-%x", &a, &b, &fw) == 3 && a == 0) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev_info[slot], sizeof(dev_info[slot]), "FW %02X", fw);
        pthread_mutex_unlock(&dev_lock);
    }
}

/* One command ("#38\r"), then its answer waited for ("#38-..."), up to 2.5 s; whatever else comes meanwhile is
 * dropped. Sent 300 ms apart without waiting, the stick took "#43-00" (stop) and missed "#38" (start): silent */
static bool command(int fd, const char *cmd, size_t n) {
    char   line[96];
    size_t ll = 0;
    bool   in = false;
    double end;

    if (write(fd, cmd, n) != (ssize_t)n) return false;
    end = adsb_now() + 2.5;
    while (adsb_now() < end) {
        fd_set         rd;
        struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
        char           buf[256];
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        if (select(fd + 1, &rd, NULL, NULL, &tv) <= 0) continue;
        ssize_t got = read(fd, buf, sizeof(buf));
        if (got <= 0) return false;
        bytes += (unsigned long long)got;
        for (ssize_t i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '#') {
                in = true;
                ll = 0;
            }
            if (!in) continue;
            if (c == '\r' || c == '\n') {
                line[ll] = 0;
                in       = false;
                if (ll >= 3 && !strncmp(line, cmd, 3)) {    /* "#43" for "#43-00" */
                    set_info(SLOT_SERIAL, line);     /* the serial ones alone take commands */
                    return true;
                }
            } else if (ll < sizeof(line) - 1) {
                line[ll++] = c;
            }
        }
    }
    return false;
}

/* Commands one after another, each answered; false when one was not */
static bool send_cmds(int fd, const char *s) {
    bool all = true;

    if (!s) return true;
    while (*s) {
        const char *e = strchr(s, '\r');
        size_t      n = e ? (size_t)(e - s + 1) : strlen(s);
        all &= command(fd, s, n);
        s += n;
    }
    return all;
}

/* ---- dump1090 for an RTL-SDR ---- */

static pid_t d1090 = -1;
static bool  d1090_fed;                 /* on the daemon's samples, not the stick itself */

static bool d1090_alive(void) {
    if (d1090 <= 0) return false;
    pid_t r = waitpid(d1090, NULL, WNOHANG);
    if (r == d1090 || (r < 0 && kill(d1090, 0) != 0)) {
        d1090 = -1;
        return false;
    }
    return true;
}

/* SIGTERM: dump1090 cancels its transfers and closes the stick. SIGKILL only after 8 s: killed with transfers in flight
 * (it was after 2 s) it left the radio's USB host (MUSB) broken for the stick, which then opened for the next user
 * and sent nothing ("THE RTL-SDR SENDS NOTHING"), till it was unplugged; rtl_spectrum.c's helper is ended so too */
static void d1090_stop(void) {
    if (d1090 <= 0) return;
    if (d1090_fed) rtl_spectrum_feed(-1);      /* the daemon's samples end: so does its input, and it ends */
    kill(d1090, SIGTERM);
    for (int i = 0; i < 80 && d1090_alive(); i++) usleep(100000);
    if (d1090_alive()) kill(d1090, SIGKILL);
    waitpid(d1090, NULL, 0);
    d1090 = -1;
}

/* On the daemon's samples when it is there (a pipe: dump1090 reads one end, the daemon is given the other), else on
 * the stick itself: an RTL-SDR only (rtl: dump1090 here has librtlsdr alone; a HackRF or a bladeRF waits for the
 * daemon). direct: on the stick itself in any case (a spare RTL-SDR, the daemon holding a HackRF or a bladeRF) */
static bool d1090_start(bool rtl, bool direct) {
    int  pp[2] = {-1, -1};
    bool fed;

    if (access(DUMP1090, X_OK) != 0) return false;
    fed = !direct && rtl_spectrum_daemon() && pipe2(pp, O_CLOEXEC) == 0;
    if (!fed && !rtl) return false;
    if (fed) fcntl(pp[1], F_SETPIPE_SZ, 1 << 20);  /* room for 0.2 s of samples: dump1090 reads in big gulps */
    pid_t p = fork();
    if (p < 0) {
        if (fed) close(pp[0]), close(pp[1]);
        return false;
    }
    if (p == 0) {                               /* the child: nothing of the GUI's but what dump1090 needs */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int nul = open("/dev/null", O_RDONLY), log = open(D1090_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fed) dup2(pp[0], 0);
        else if (nul >= 0) dup2(nul, 0);
        if (log >= 0) dup2(log, 1), dup2(log, 2);
        for (int f = 3; f < 1024; f++) close(f);
        if (fed)
            execl(DUMP1090, DUMP1090, "--ifile", "-", "--iformat", "UC8", "--quiet", "--net",
                  "--net-bind-address", "127.0.0.1", "--net-ro-port", "30002", "--net-ri-port", "0", "--net-sbs-port",
                  "0", "--net-bi-port", "0", "--net-bo-port", "0", "--net-stratux-port", "0", (char *)NULL);
        else
            execl(DUMP1090, DUMP1090, "--device-index", "0", "--adaptive-range", "--quiet", "--net",
                  "--net-bind-address", "127.0.0.1", "--net-ro-port", "30002", "--net-ri-port", "0", "--net-sbs-port",
                  "0", "--net-bi-port", "0", "--net-bo-port", "0", "--net-stratux-port", "0", (char *)NULL);
        _exit(127);
    }
    if (fed) {
        close(pp[0]);
        if (!rtl_spectrum_feed(pp[1])) fed = false;    /* nothing to read: it ends, and is started again */
        close(pp[1]);
    }
    d1090     = p;
    d1090_fed = fed;
    return true;
}

static int d1090_connect(void) {
    int                fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a  = {.sin_family = AF_INET, .sin_port = htons(D1090_PORT)};

    if (fd < 0) return -1;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The tuner: the daemon's word ("R820T"), or what librtlsdr found from dump1090's log ("Found Rafael Micro R820T
 * tuner", "RTL-SDR Blog V4 Detected") */
static void d1090_tuner(void) {
    FILE *f;
    char  line[160], tuner[24] = "";

    if (d1090_fed) {
        rtl_spectrum_tuner(tuner, sizeof(tuner));
        if (tuner[0]) {
            pthread_mutex_lock(&dev_lock);
            snprintf(dev_info[SLOT_SDR], sizeof(dev_info[SLOT_SDR]), "%s", tuner);
            pthread_mutex_unlock(&dev_lock);
        }
        return;
    }
    if (!(f = fopen(D1090_LOG, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        char *p;
        if ((p = strstr(line, "Blog V4"))) snprintf(tuner, sizeof(tuner), "BLOG V4");
        else if (!tuner[0] && (p = strstr(line, "Found ")) && strstr(line, "tuner")) {
            char *last = strstr(p, " tuner"), *w = last;
            while (w > p + 6 && w[-1] != ' ') w--;      /* the word before " tuner": R820T, E4000, FC0013 */
            snprintf(tuner, sizeof(tuner), "%.*s", (int)(last - w), w);
        }
    }
    fclose(f);
    if (tuner[0]) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev_info[SLOT_SDR], sizeof(dev_info[SLOT_SDR]), "%s", tuner);
        pthread_mutex_unlock(&dev_lock);
    }
}

/* One slot's receiver: looked for, opened, read into adsb.c, let go when it goes or another is wanted */
static void *rx_thread(void *arg) {
    const int slot = (int)(intptr_t)arg;
    int    fd = -1, rx = -1, held_num = 0;
    char   frame[56], reply[64], held[64] = "";     /* held: the receiver's USB device ("3-1"), held_num its number */
    size_t len = 0, rlen = 0;
    bool   inside = false, stamped = false, replying = false, direct = false;  /* direct: a spare RTL-SDR's own
                                                                                * dump1090, not the daemon's SDR */
    double last = 0, init_at = 0, looked = 0;       /* init_at: the start commands are due, 0 sent */

    for (;;) {
        /* The Sentry's SDR lent: its dump1090 stopped, not started again till it is back (a spare RTL-SDR's goes on) */
        if (atomic_load(&lent) && fd >= 0 && rx >= 0 && receivers[rx].sdr && !direct) {
            close(fd);
            fd = -1;
            d1090_stop();
            continue;
        }
        if (fd < 0) {
            seen_t      seen[ADSB_KIND_N];
            bool        held_back;
            adsb_kind_t k = pick_slot(slot, seen, &direct, &held_back);
            const char *usb = k ? seen[k].usb : "";
            char        dev[32];

            set_dev(slot, "", "");
            rx = k ? seen[k].row : -1;
            atomic_store(&kind_now[slot], k);
            if (held_back) {                    /* the Sentry's SDR: AIRSPACE's only while its screen is open */
                d1090_stop();
                st[slot] = ADSB_RX_LENT;
                usleep(300000);
                continue;
            }
            if (rx < 0) {
                st[slot] = ADSB_RX_NO_STICK;
                if (slot == SLOT_SDR) d1090_stop();     /* the RTL-SDR went, or none wanted: its dump1090 too */
            } else if (receivers[rx].sdr) {     /* dump1090 decodes it; its raw frames over TCP */
                if (d1090_alive() && d1090_fed != (!direct && rtl_spectrum_daemon()))
                    d1090_stop();               /* on the daemon's samples, wanted on a spare stick, or the other way */
                if (!d1090_alive()) {
                    if (!d1090_start(kind_of(rx) == ADSB_KIND_RTL, direct)) {
                        st[slot] = ADSB_RX_NO_DRIVER;
                        sleep(2);
                        continue;
                    }
                    st[slot] = ADSB_RX_OPENING;
                    /* its port as soon as it is up, looked for each 200 ms (5 s at most): a stick swapped in while
                     * the screen is open is heard within a second or two */
                    for (int i = 0; i < 25 && (fd = d1090_connect()) < 0 && d1090_alive(); i++) usleep(200000);
                }
                if (fd >= 0 || (fd = d1090_connect()) >= 0) {
                    snprintf(held, sizeof(held), "%s", usb);
                    held_num = usb_devnum(usb);
                    set_dev(slot, "127.0.0.1:30002", receivers[rx].name);
                    d1090_tuner();
                    st[slot] = ADSB_RX_ON;
                    inside = replying = false;
                    last   = adsb_now();
                    init_at = 0;
                    continue;
                }
                st[slot] = ADSB_RX_OPENING;
            } else if (access(CDC_ACM, F_OK) != 0) {
                st[slot] = ADSB_RX_NO_DRIVER;
            } else if (!find_tty(usb, dev, sizeof(dev))) {
                st[slot] = ADSB_RX_OPENING;
                if (receivers[rx].new_id) bind_stick(rx, usb);
            } else if ((fd = open_port(dev)) >= 0) {
                snprintf(held, sizeof(held), "%s", usb);
                held_num = usb_devnum(usb);
                set_dev(slot, dev, receivers[rx].name);
                st[slot] = ADSB_RX_ON;
                inside  = replying = false;
                last    = adsb_now();
                init_at = receivers[rx].init ? last + 1.0 : 0;     /* sent at once, the microADSB missed them */
                continue;
            } else {
                st[slot] = ADSB_RX_OPENING;
            }
            sleep(1);                           /* looked for each second: a swap is seen at once */
            continue;
        }
        if (adsb_now() - looked >= 1.0) {       /* still the receiver it opened, where it was, and the one wanted */
            seen_t seen[ADSB_KIND_N];
            bool   d, hb;

            looked = adsb_now();
            if (!held_num || usb_devnum(held) != held_num || pick_slot(slot, seen, &d, &hb) != atomic_load(&kind_now[slot]) ||
                d != direct) {
                close(fd);
                fd = -1;
                if (receivers[rx].sdr) d1090_stop();
                continue;
            }
        }
        if (init_at > 0 && adsb_now() >= init_at) {
            send_cmds(fd, receivers[rx].init);
            init_at = 0;
            last    = adsb_now();
        }
        if (receivers[rx].init && !init_at && adsb_now() - last > QUIET_S) {   /* quiet: started again */
            send_cmds(fd, receivers[rx].init);
            last = adsb_now();
        }

        fd_set         rd;
        struct timeval tv = {.tv_sec = 1};
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        int r = select(fd + 1, &rd, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) continue;

        char    buf[512];
        ssize_t n = r > 0 ? read(fd, buf, sizeof(buf)) : -1;
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            close(fd);                          /* unplugged: look for it again */
            fd = -1;
            continue;
        }
        bytes += (unsigned long long)n;

        double now = adsb_now();
        adsb_lock();
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (replying) {                     /* "#00-00-0E-...": a command's answer, to the end of its line */
                if (c == '\r' || c == '\n') {
                    reply[rlen] = 0;
                    replying    = false;
                    set_info(slot, reply);
                } else if (rlen < sizeof(reply) - 1) {
                    reply[rlen++] = c;
                }
                continue;
            }
            if (c == '#') {
                replying = true;
                inside   = false;
                rlen     = 0;
                reply[rlen++] = c;
            } else if (c == '*' || c == '@') {
                inside  = true;
                stamped = c == '@';
                len     = 0;
            } else if (c == ';' && inside) {
                inside = false;
                last   = now;
                if (stamped && len >= 12)       /* '@': a 48-bit time stamp first */
                    adsb_frame_hex(frame + 12, len - 12, now);
                else if (!stamped)
                    adsb_frame_hex(frame, len, now);
            } else if (inside) {
                if (len < sizeof(frame) && c != '\r' && c != '\n')
                    frame[len++] = c;
                else if (c != '\r' && c != '\n')
                    inside = false;             /* too long: not a frame */
            }
        }
        adsb_unlock();
    }
    return NULL;
}

void adsb_rx_start(void) {
    bool no = false;

    if (!atomic_compare_exchange_strong(&started, &no, true)) return;
    for (int i = 0; i < SLOTS; i++) {
        pthread_t t;

        st[i] = ADSB_RX_NO_STICK;
        if (pthread_create(&t, NULL, rx_thread, (void *)(intptr_t)i) == 0) pthread_detach(t);
        else st[i] = ADSB_RX_OFF;
    }
}

/* The two slots as one: on when either is (its port, the serial one's first), else lent, opening, no driver, none */
adsb_rx_state_t adsb_rx_state(char *dev, size_t n) {
    adsb_rx_state_t a = (adsb_rx_state_t)st[SLOT_SERIAL], b = (adsb_rx_state_t)st[SLOT_SDR], r;
    int             from = a == ADSB_RX_ON ? SLOT_SERIAL : SLOT_SDR;

    r = a == ADSB_RX_ON || b == ADSB_RX_ON               ? ADSB_RX_ON
      : b == ADSB_RX_LENT                                ? ADSB_RX_LENT
      : a == ADSB_RX_OPENING || b == ADSB_RX_OPENING     ? ADSB_RX_OPENING
      : a == ADSB_RX_NO_DRIVER || b == ADSB_RX_NO_DRIVER ? ADSB_RX_NO_DRIVER
      : a == ADSB_RX_OFF && b == ADSB_RX_OFF             ? ADSB_RX_OFF
                                                         : ADSB_RX_NO_STICK;
    if (dev && n) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev, n, "%s", r == ADSB_RX_ON ? dev_path[from] : "");
        pthread_mutex_unlock(&dev_lock);
    }
    return r;
}

/* Each slot that listens: "GNS 5890", "RTL-SDR R820T", "GNS 5890 + RTL-SDR R820T" */
void adsb_rx_name(char *name, size_t n) {
    size_t at = 0;

    if (!n) return;
    name[0] = 0;
    pthread_mutex_lock(&dev_lock);
    for (int i = 0; i < SLOTS && at < n; i++) {
        if (st[i] != ADSB_RX_ON || !dev_name[i][0]) continue;
        int w = snprintf(name + at, n - at, "%s%s%s%s", at ? " + " : "", dev_name[i], dev_info[i][0] ? " " : "",
                         dev_info[i]);
        if (w > 0) at += (size_t)w;
    }
    pthread_mutex_unlock(&dev_lock);
}

uint64_t adsb_rx_bytes(void) { return bytes; }

void adsb_rx_lend(bool on) { atomic_store(&lent, on); }

/* ---- the RTL-SDR between its two users ---- */

/* What is on USB, looked at each second by a thread of its own, so no screen ever reads sysfs for it: a USB host stuck
 * in a disconnect (the radio's MUSB has been) must not take the GUI with it. Any number of receivers on a hub: an
 * RTL-SDR and a serial one are each seen */
static atomic_bool    p_sdr, p_rcv;
static atomic_int     p_sdr_id;         /* the RTL-SDR's bus * 1000 + device number: another one once it is replugged */
static const char *_Atomic p_sdr_kind = "";     /* its kind: receivers[]'s name */
static atomic_int     p_looks;          /* looks at USB so far: 0, nothing known yet */
static atomic_uint    p_mask;           /* the receivers' kinds on USB, a bit each */
static atomic_bool    p_dump;           /* dump1090 there */
static pthread_once_t p_once = PTHREAD_ONCE_INIT;

static void *presence_thread(void *arg) {
    (void)arg;
    for (;;) {
        DIR           *d   = opendir(USB_DEVICES);
        struct dirent *e;
        bool           sdr = false, rcv = false;
        int            id  = 0, rank = 0;
        const char    *kind = "";
        unsigned       mask = 0;

        while (d && (e = readdir(d))) {
            char path[300], v[8], p[8], prod[64], bus[8], dev[8];

            if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
            snprintf(path, sizeof(path), USB_DEVICES "/%s/idVendor", e->d_name);
            if (!read_line(path, v, sizeof(v))) continue;
            snprintf(path, sizeof(path), USB_DEVICES "/%s/idProduct", e->d_name);
            if (!read_line(path, p, sizeof(p))) continue;
            snprintf(path, sizeof(path), USB_DEVICES "/%s/product", e->d_name);
            if (!read_line(path, prod, sizeof(prod))) prod[0] = 0;
            for (int r = 0; r < N_RECEIVERS; r++) {
                if (strcmp(v, receivers[r].vid) || strcmp(p, receivers[r].pid) ||
                    (receivers[r].product && !strstr(prod, receivers[r].product)))
                    continue;
                mask |= KB(kind_of(r));
                if (receivers[r].sdr) {
                    /* more than one: the daemon's choice, a HackRF, then a bladeRF, then an RTL-SDR (rtlspec.c) */
                    int k = !strcmp(receivers[r].name, "HACKRF") ? 3 : !strcmp(receivers[r].name, "BLADERF") ? 2 : 1;

                    sdr = true;
                    if (k <= rank) break;
                    rank = k;
                    kind = receivers[r].name;
                    snprintf(path, sizeof(path), USB_DEVICES "/%s/busnum", e->d_name);
                    if (!read_line(path, bus, sizeof(bus))) bus[0] = 0;
                    snprintf(path, sizeof(path), USB_DEVICES "/%s/devnum", e->d_name);
                    if (!read_line(path, dev, sizeof(dev))) dev[0] = 0;
                    id = atoi(bus) * 1000 + atoi(dev);
                } else {
                    rcv = true;
                }
                break;
            }
        }
        if (d) closedir(d);
        atomic_store(&p_sdr, sdr);
        atomic_store(&p_rcv, rcv);
        atomic_store(&p_sdr_id, sdr ? id : 0);
        atomic_store(&p_sdr_kind, kind);
        atomic_store(&p_mask, mask);
        atomic_store(&p_dump, access(DUMP1090, X_OK) == 0);
        atomic_fetch_add(&p_looks, 1);
        sleep(1);
    }
    return NULL;
}

static void presence_start(void) {
    pthread_t t;

    if (pthread_create(&t, NULL, presence_thread, NULL) == 0) pthread_detach(t);
}

bool adsb_rx_receiver_present(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_rcv);
}

bool adsb_rx_sdr_present(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_sdr);
}

const char *adsb_rx_sdr_kind(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_sdr_kind);
}

int adsb_rx_sdr_id(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_sdr_id);
}

int adsb_rx_usb_looks(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_looks);
}


unsigned adsb_rx_choices(void) {
    pthread_once(&p_once, presence_start);
    return usable_of(atomic_load(&p_mask), daemon_dev(), atomic_load(&p_dump));
}

unsigned adsb_rx_present(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_mask);
}

bool adsb_rx_rtl_spare(void) {
    pthread_once(&p_once, presence_start);
    return spare_of(atomic_load(&p_mask), daemon_dev());
}

const char *adsb_rx_kind_name(adsb_kind_t k) {
    static const char *const names[ADSB_KIND_N] = {"", "GNS 5890", "MICROADSB", "RTL-SDR", "HACKRF", "BLADERF"};

    return k >= 0 && k < ADSB_KIND_N ? names[k] : "";
}

const char *adsb_rx_kind_short(adsb_kind_t k) {
    static const char *const names[ADSB_KIND_N] = {"", "GNS", "MICRO", "RTL", "HACKRF", "BLADERF"};

    return k >= 0 && k < ADSB_KIND_N ? names[k] : "";
}

void adsb_rx_choose(unsigned kinds) { atomic_store(&want_mask, kinds & (SERIALS | SDRS)); }

unsigned adsb_rx_kinds_now(void) {
    unsigned m = 0;

    for (int i = 0; i < SLOTS; i++) {
        int s = st[i], k = atomic_load(&kind_now[i]);

        if (s != ADSB_RX_OFF && s != ADSB_RX_NO_STICK && k > ADSB_KIND_NONE && k < ADSB_KIND_N) m |= KB(k);
    }
    return m;
}

unsigned adsb_rx_kinds_next(void) {
    unsigned m = 0;

    pthread_once(&p_once, presence_start);
    for (int i = 0; i < SLOTS; i++) {
        bool        d, hb;
        adsb_kind_t k = pick_of(i, atomic_load(&p_mask), daemon_dev(), atomic_load(&p_dump), atomic_load(&want_mask),
                                false, &d, &hb);

        if (k != ADSB_KIND_NONE) m |= KB(k);
    }
    return m;
}
