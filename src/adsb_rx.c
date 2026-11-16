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

#include <ctype.h>
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

/* Up to three at once, each on a thread of its own: a serial receiver (GNS 5890, microADSB), the daemon's SDR through
 * dump1090 on its samples, and a spare RTL-SDR (one beside the daemon's HackRF or bladeRF) through a dump1090 of its
 * own. All feed adsb.c (BOTH, ALL: the same aircraft heard by each, its messages counted from each) */
enum { SLOT_SERIAL, SLOT_SDR, SLOT_SPARE };
/* Spare RTLs at once (a KrakenSDR's four beside B's): the first listens with AIRSPACE closed too, the rest while its
 * screen is open (the user, 2026-11-16: every reception "color coded to the radio that recieved it ... scale for
 * however many are able to be connected") */
#define SPARES 5
#define SLOTS  (SLOT_SPARE + SPARES)

static atomic_bool     started;
static atomic_int      st[SLOTS];       /* ADSB_RX_OFF (0) at first */
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

/* The daemon's SDR: its word, or the one found on USB before it said; NONE with no daemon on this image (dump1090 opens
 * an RTL-SDR itself). DD_WAIT: the daemon not reached yet (the GUI's first seconds, the daemon starting again): no SDR
 * is taken meanwhile. Taken as "no daemon" then, the stick was opened by a dump1090 of its own beside the daemon's
 * (2026-11-16, the radio's first seconds after a GUI restart: "RTL-SDR + RTL-SDR") */
#define DD_WAIT ((rtl_dev_t)-1)
static rtl_dev_t daemon_dev(void) {
    if (rtl_spectrum_daemon()) return rtl_spectrum_dev();
    return rtl_spectrum_state() == RTL_SPEC_NO_HELPER || access("/usr/bin/x6x00-rtlspec", X_OK) != 0 ? RTL_DEV_NONE
                                                                                                      : DD_WAIT;
}

static int spare_rtl(int nth, bool self_only, char *sn, size_t sn_n, char *loc, size_t loc_n);
static rtl_dev_t daemon_dev(void);

/* Spare nth (0 the first): an RTL on a hub of its own power that B's daemon neither holds nor was asked for and no
 * watch channel has; beside a HackRF or bladeRF with none such, one RTL on the radio's own power as before. Its place
 * (-1 none), serial and USB location */
static int spare_pick(int nth, char *sn, size_t sn_n, char *loc, size_t loc_n) {
    rtl_dev_t dd     = daemon_dev();
    bool      beside = dd == RTL_DEV_HACKRF || dd == RTL_DEV_BLADERF;
    int       r;

    if (dd != RTL_DEV_RTL && !beside) return -1;
    r = spare_rtl(nth, true, sn, sn_n, loc, loc_n);
    if (r < 0 && beside && nth == 0 && spare_rtl(0, true, NULL, 0, NULL, 0) < 0) r = spare_rtl(0, false, sn, sn_n, loc, loc_n);
    return r;
}

static int spare_avail(void) {
    int n = 0;

    while (n < SPARES && spare_pick(n, NULL, 0, NULL, 0) >= 0) n++;
    return n;
}

/* A spare RTL-SDR, free for a dump1090 of its own: one beside the daemon's HackRF or bladeRF (B's choice, or its
 * order), or with two or more RTLs one that B neither holds nor asked for, behind a hub of its own power (a
 * KrakenSDR's): then AIRSPACE and B each have theirs (the user, 2026-11-16, "Tuners at once"; five streamed at
 * once on the radio). Two sticks on the radio's own power stay one at a time, as before (the brown-out of
 * 2026-11-15) */
static bool spare_of(unsigned present, rtl_dev_t dd) {
    (void)dd;
    return (present & KB(ADSB_KIND_RTL)) && spare_pick(0, NULL, 0, NULL, 0) >= 0;
}

/* Of the kinds on USB (present), those that can be listened on: the serial ones; with dump1090, the daemon's SDR (dd)
 * and an RTL-SDR beside it (a spare), or with no daemon an RTL-SDR (dump1090 opens it itself). Only what tunes 1090
 * MHz is here: the X6200 does not */
static unsigned usable_of(unsigned present, rtl_dev_t dd, bool dump) {
    unsigned u = present & SERIALS;

    if (!dump || dd == DD_WAIT) return u;
    if (dd != RTL_DEV_NONE) u |= present & KB(kind_of_dev(dd));
    if (dd == RTL_DEV_NONE || spare_of(present, dd)) u |= present & KB(ADSB_KIND_RTL);
    return u;
}

/* What each slot listens on (k[SLOTS], NONE idle). As many receivers as were chosen (want: one, BOTH two, ALL three),
 * the ones chosen first where they are there, the rest from the others in this order: a serial one, a spare RTL-SDR
 * (B keeps its SDR), the daemon's SDR. Two SDRs at once only when two were chosen: both streaming on the HOST port's
 * own power browned it out, and a disconnect then crashed the kernel (musb_h_disable, 2026-11-16); nothing chosen (0),
 * a serial one and one SDR. So a choice follows a receiver swapped for another: BOTH of a GNS 5890
 * and a HackRF, the GNS 5890 pulled out and an RTL-SDR plugged in, is the HackRF and the RTL-SDR. One serial one at a
 * time, one SDR of the daemon's, one spare */
static void assign(unsigned present, rtl_dev_t dd, bool dump, unsigned want, adsb_kind_t k[SLOTS]) {
    static const adsb_kind_t serial_order[] = {ADSB_KIND_GNS, ADSB_KIND_MICRO};
    unsigned    usable = usable_of(present, dd, dump);
    adsb_kind_t dk     = dd != RTL_DEV_NONE ? kind_of_dev(dd) : ADSB_KIND_RTL;     /* no daemon: the stick itself */
    bool        r_ok   = spare_of(present, dd) && (usable & KB(ADSB_KIND_RTL)), d_ok = (usable & KB(dk)) != 0;
    int         n      = want ? __builtin_popcount(want & (SERIALS | SDRS)) : 2, got = 0;

    for (int i = 0; i < SLOTS; i++) k[i] = ADSB_KIND_NONE;
    for (int i = 0; i < 2 && !k[SLOT_SERIAL]; i++)
        if (want & usable & KB(serial_order[i])) k[SLOT_SERIAL] = serial_order[i], got++;
    if ((want & KB(ADSB_KIND_RTL)) && r_ok) k[SLOT_SPARE] = ADSB_KIND_RTL, got++;
    if ((want & KB(dk)) && d_ok) k[SLOT_SDR] = dk, got++;
    int sdrs = __builtin_popcount(want & SDRS);     /* SDRs wanted: no more than that many stream */
    bool one = !want || sdrs <= 1;

    for (int i = 0; i < 2 && got < n && !k[SLOT_SERIAL]; i++)
        if (usable & KB(serial_order[i])) k[SLOT_SERIAL] = serial_order[i], got++;
    if (got < n && !k[SLOT_SPARE] && r_ok && !(one && k[SLOT_SDR])) k[SLOT_SPARE] = ADSB_KIND_RTL, got++;
    if (got < n && !k[SLOT_SDR] && d_ok && !(one && k[SLOT_SPARE])) k[SLOT_SDR] = dk, got++;
    /* the further spares, each a stick of its own power, while AIRSPACE's screen is open on ADS-B (B's SDR not lent
     * to the Sentry): every free one, whatever was chosen with the RTL */
    if (k[SLOT_SPARE] && !atomic_load(&lent) && (!want || (want & KB(ADSB_KIND_RTL)))) {
        int sp = spare_avail();

        for (int i = 1; i < sp; i++) k[SLOT_SPARE + i] = ADSB_KIND_RTL;
    }
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

/* What slot listens on, as things are on USB now (assign); *direct: the spare's own dump1090; *held_back: the daemon's
 * SDR lent to the Sentry (AIRSPACE closed) */
static adsb_kind_t pick_slot(int slot, seen_t seen[ADSB_KIND_N], bool *direct, bool *held_back) {
    unsigned    present = scan_usb(seen);
    rtl_dev_t   dd      = daemon_dev();
    adsb_kind_t k[SLOTS];

    assign(present, dd, access(DUMP1090, X_OK) == 0, atomic_load(&want_mask), k);
    *direct    = slot >= SLOT_SPARE;
    *held_back = slot == SLOT_SDR && k[slot] != ADSB_KIND_NONE && dd != RTL_DEV_NONE && atomic_load(&lent);
    return k[slot];
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

/* ---- dump1090, one for the daemon's SDR and one for a spare RTL-SDR ---- */

typedef struct {
    pid_t       pid;
    bool        fed;                    /* on the daemon's samples, not a stick itself */
    int         port;                   /* its raw frames, on 127.0.0.1 */
    const char *log;
    char        sn[24];                 /* a stick of its own: its serial ("" librtlsdr's first) */
} d1090_t;

static d1090_t d1090s[SLOTS] = {
    [SLOT_SERIAL] = {-1, false, 0, NULL, ""},
    [SLOT_SDR]    = {-1, false, D1090_PORT, D1090_LOG, ""},
    [SLOT_SPARE]  = {-1, false, D1090_PORT + 1, "/tmp/dump1090-spare.log", ""},
};

/* The further spares: their ports after the first's (30004...), their logs /tmp/dump1090-spare2.log... */
static void d1090_spares_init(void) {
    static char logs[SPARES][32];

    for (int i = 1; i < SPARES; i++) {
        snprintf(logs[i], sizeof(logs[i]), "/tmp/dump1090-spare%d.log", i + 1);
        d1090s[SLOT_SPARE + i] = (d1090_t){-1, false, D1090_PORT + 1 + i, logs[i], ""};
    }
}

static bool d1090_alive(d1090_t *d) {
    if (d->pid <= 0) return false;
    pid_t r = waitpid(d->pid, NULL, WNOHANG);
    if (r == d->pid || (r < 0 && kill(d->pid, 0) != 0)) {
        d->pid = -1;
        return false;
    }
    return true;
}

/* SIGTERM: dump1090 cancels its transfers and closes the stick. SIGKILL only after 8 s: killed with transfers in flight
 * (it was after 2 s) it left the radio's USB host (MUSB) broken for the stick, which then opened for the next user
 * and sent nothing ("THE RTL-SDR SENDS NOTHING"), till it was unplugged; rtl_spectrum.c's helper is ended so too */
static void d1090_stop(d1090_t *d) {
    if (d->pid <= 0) return;
    if (d->fed) rtl_spectrum_feed(-1);         /* the daemon's samples end: so does its input, and it ends */
    kill(d->pid, SIGTERM);
    for (int i = 0; i < 80 && d1090_alive(d); i++) usleep(100000);
    if (d1090_alive(d)) kill(d->pid, SIGKILL);
    waitpid(d->pid, NULL, 0);
    d->pid = -1;
}

/* On the daemon's samples when it is there (a pipe: dump1090 reads one end, the daemon is given the other), else on
 * the stick itself: an RTL-SDR only (rtl: dump1090 here has librtlsdr alone; a HackRF or a bladeRF waits for the
 * daemon). direct: on the stick itself in any case (a spare RTL-SDR, the daemon holding a HackRF or a bladeRF) */
static bool d1090_start(d1090_t *d, bool rtl, bool direct) {
    int  pp[2] = {-1, -1};
    bool fed;
    char port[8];

    if (access(DUMP1090, X_OK) != 0) return false;
    fed = !direct && rtl_spectrum_daemon() && pipe2(pp, O_CLOEXEC) == 0;
    if (!fed && !rtl) return false;
    if (fed) fcntl(pp[1], F_SETPIPE_SZ, 1 << 20);  /* room for 0.2 s of samples: dump1090 reads in big gulps */
    snprintf(port, sizeof(port), "%d", d->port);
    pid_t p = fork();
    if (p < 0) {
        if (fed) close(pp[0]), close(pp[1]);
        return false;
    }
    if (p == 0) {                               /* the child: nothing of the GUI's but what dump1090 needs */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int nul = open("/dev/null", O_RDONLY), log = open(d->log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fed) dup2(pp[0], 0);
        else if (nul >= 0) dup2(nul, 0);
        if (log >= 0) dup2(log, 1), dup2(log, 2);
        for (int f = 3; f < 1024; f++) close(f);
        if (fed)
            execl(DUMP1090, DUMP1090, "--ifile", "-", "--iformat", "UC8", "--quiet", "--net",
                  "--net-bind-address", "127.0.0.1", "--net-ro-port", port, "--net-ri-port", "0", "--net-sbs-port",
                  "0", "--net-bi-port", "0", "--net-bo-port", "0", "--net-stratux-port", "0", (char *)NULL);
        else
            execl(DUMP1090, DUMP1090, "--device-index", d->sn[0] ? d->sn : "0", "--adaptive-range", "--quiet", "--net",
                  "--net-bind-address", "127.0.0.1", "--net-ro-port", port, "--net-ri-port", "0", "--net-sbs-port",
                  "0", "--net-bi-port", "0", "--net-bo-port", "0", "--net-stratux-port", "0", (char *)NULL);
        _exit(127);
    }
    if (fed) {
        close(pp[0]);
        if (!rtl_spectrum_feed(pp[1])) fed = false;    /* nothing to read: it ends, and is started again */
        close(pp[1]);
    }
    d->pid = p;
    d->fed = fed;
    return true;
}

static int d1090_connect(d1090_t *d) {
    int                fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a  = {.sin_family = AF_INET, .sin_port = htons((uint16_t)d->port)};

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
static void d1090_tuner(d1090_t *d, int slot) {
    FILE *f;
    char  line[160], tuner[24] = "";

    if (d->fed) {
        rtl_spectrum_tuner(tuner, sizeof(tuner));
    } else if ((f = fopen(d->log, "r"))) {
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
    }
    if (tuner[0]) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev_info[slot], sizeof(dev_info[slot]), "%s", tuner);
        pthread_mutex_unlock(&dev_lock);
    }
}

/* One slot's receiver: looked for, opened, read into adsb.c, let go when it goes or another is wanted */
static void *rx_thread(void *arg) {
    const int slot = (int)(intptr_t)arg;
    d1090_t  *D    = &d1090s[slot];
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
            d1090_stop(D);
            continue;
        }
        if (fd < 0) {
            seen_t      seen[ADSB_KIND_N];
            bool        held_back;
            adsb_kind_t k = pick_slot(slot, seen, &direct, &held_back);
            const char *usb = k ? seen[k].usb : "";
            char        dev[32], sp_loc[24] = "", sp_name[16] = "";
            int         sp = -1;

            D->sn[0] = 0;
            if (slot >= SLOT_SPARE && k == ADSB_KIND_RTL &&     /* the spare RTL itself: by its serial, where it is */
                (sp = spare_pick(slot - SLOT_SPARE, D->sn, sizeof(D->sn), sp_loc, sizeof(sp_loc))) >= 0) {
                usb = sp_loc;
                snprintf(sp_name, sizeof(sp_name), "%s", rtl_spectrum_rtl_tag(sp));
            }

            set_dev(slot, "", "");
            rx = k ? seen[k].row : -1;
            atomic_store(&kind_now[slot], k);
            if (held_back) {                    /* the Sentry's SDR: AIRSPACE's only while its screen is open */
                d1090_stop(D);
                st[slot] = ADSB_RX_LENT;
                usleep(300000);
                continue;
            }
            if (rx < 0) {
                st[slot] = ADSB_RX_NO_STICK;
                if (slot != SLOT_SERIAL) d1090_stop(D);     /* the RTL-SDR went, or none wanted: its dump1090 too */
            } else if (receivers[rx].sdr) {     /* dump1090 decodes it; its raw frames over TCP */
                if (d1090_alive(D) && D->fed != (!direct && rtl_spectrum_daemon()))
                    d1090_stop(D);               /* on the daemon's samples, wanted on a spare stick, or the other way */
                if (!d1090_alive(D)) {
                    if (!d1090_start(D, kind_of(rx) == ADSB_KIND_RTL, direct)) {
                        st[slot] = ADSB_RX_NO_DRIVER;
                        sleep(2);
                        continue;
                    }
                    st[slot] = ADSB_RX_OPENING;
                    /* its port as soon as it is up, looked for each 200 ms (5 s at most): a stick swapped in while
                     * the screen is open is heard within a second or two */
                    for (int i = 0; i < 25 && (fd = d1090_connect(D)) < 0 && d1090_alive(D); i++) usleep(200000);
                }
                if (fd >= 0 || (fd = d1090_connect(D)) >= 0) {
                    snprintf(held, sizeof(held), "%s", usb);
                    held_num = usb_devnum(usb);
                    char at[24];

                    snprintf(at, sizeof(at), "127.0.0.1:%d", D->port);
                    set_dev(slot, at,
                            sp_name[0]                              ? sp_name   /* RTL1... */
                            : kind_of(rx) == ADSB_KIND_RTL && !direct ? rtl_spectrum_dev_tag()     /* B's: RTL0 */
                                                                      : receivers[rx].name);
                    d1090_tuner(D, slot);
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
            char sn_now[24] = "";
            bool moved = slot >= SLOT_SPARE && D->sn[0] &&
                         (spare_pick(slot - SLOT_SPARE, sn_now, sizeof(sn_now), NULL, 0) < 0 ||
                          strcmp(sn_now, D->sn) != 0);  /* B took it, or asked for it: another spare */

            if (!held_num || usb_devnum(held) != held_num || pick_slot(slot, seen, &d, &hb) != atomic_load(&kind_now[slot]) ||
                d != direct || moved) {
                close(fd);
                fd = -1;
                if (receivers[rx].sdr) d1090_stop(D);
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
                    adsb_frame_from(frame + 12, len - 12, now, slot);
                else if (!stamped)
                    adsb_frame_from(frame, len, now, slot);
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
    d1090_spares_init();
    for (int i = 0; i < SLOTS; i++) {
        pthread_t t;

        st[i] = ADSB_RX_NO_STICK;
        if (pthread_create(&t, NULL, rx_thread, (void *)(intptr_t)i) == 0) pthread_detach(t);
        else st[i] = ADSB_RX_OFF;
    }
}

/* The slots as one: on when any is (its port, the first's), else lent, opening, no driver, off, none */
adsb_rx_state_t adsb_rx_state(char *dev, size_t n) {
    static const adsb_rx_state_t order[] = {ADSB_RX_ON, ADSB_RX_LENT, ADSB_RX_OPENING, ADSB_RX_NO_DRIVER};
    adsb_rx_state_t r = ADSB_RX_NO_STICK;
    int             from = -1, off = 0;

    for (int o = 0; o < 4 && from < 0; o++)
        for (int i = 0; i < SLOTS && from < 0; i++)
            if (st[i] == (int)order[o]) r = order[o], from = i;
    for (int i = 0; i < SLOTS; i++) off += st[i] == ADSB_RX_OFF;
    if (from < 0 && off == SLOTS) r = ADSB_RX_OFF;
    if (dev && n) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev, n, "%s", r == ADSB_RX_ON ? dev_path[from] : "");     /* from: the one on */
        pthread_mutex_unlock(&dev_lock);
    }
    return r;
}

/* Each slot that listens: "GNS 5890", "RTL-SDR R820T", "GNS 5890 + RTL-SDR R820T" */
_Static_assert(SLOT_SERIAL == ADSB_SLOT_SERIAL && SLOT_SDR == ADSB_SLOT_SDR && SLOT_SPARE == ADSB_SLOT_SPARE &&
                   SLOTS <= ADSB_SRCS && SLOTS == ADSB_SLOTS, "adsb_rx.h's slots");

/* Slot i's name, dev_lock held: the tuner alone when it says the name already ("HACKRF-ONE", not "HACKRF HACKRF-ONE") */
static void slot_name(int i, char *s, size_t n) {
    bool said = dev_info[i][0] && !strncmp(dev_info[i], dev_name[i], strlen(dev_name[i]));

    snprintf(s, n, "%s%s%s", said ? "" : dev_name[i], dev_info[i][0] && !said ? " " : "", dev_info[i]);
}

int adsb_rx_slots(int *slot, char (*name)[40], int max) {
    int k = 0;

    pthread_mutex_lock(&dev_lock);
    for (int i = 0; i < SLOTS && k < max; i++) {
        if (st[i] != ADSB_RX_ON || !dev_name[i][0]) continue;
        slot[k] = i;
        slot_name(i, name[k], sizeof(name[k]));
        k++;
    }
    pthread_mutex_unlock(&dev_lock);
    return k;
}

void adsb_rx_name(char *name, size_t n) {
    size_t at = 0;

    if (!n) return;
    name[0] = 0;
    pthread_mutex_lock(&dev_lock);
    for (int i = 0; i < SLOTS && at < n; i++) {
        char one[48];

        if (st[i] != ADSB_RX_ON || !dev_name[i][0]) continue;
        slot_name(i, one, sizeof(one));
        int w = snprintf(name + at, n - at, "%s%s", at ? " + " : "", one);
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
static atomic_int     p_rtls;           /* RTL-SDRs on USB (a KrakenSDR is five): RTL0, RTL1... (rtl_spectrum.h) */
/* Their serials and USB locations in RTL order, as x6x00-rtlspec orders them (sdr_rtl.c): serial, numbers as numbers,
 * then the location ("3-1.4"), so RTL<n> is the same stick there and here */
typedef struct { char sn[24], loc[24]; bool self; } rtl_id_t;   /* self: behind a hub of its own power */
static rtl_id_t        p_rtl_id[8];
static pthread_mutex_t p_rtl_lock = PTHREAD_MUTEX_INITIALIZER;

static int num_cmp(const char *a, const char *b) {       /* "3-1.10" after "3-1.9", 1004 after 1000 */
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            unsigned long long x = strtoull(a, (char **)&a, 10), y = strtoull(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
        } else {
            if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
            a++, b++;
        }
    }
    return *a ? 1 : *b ? -1 : 0;
}

static int rtl_id_cmp(const void *pa, const void *pb) {
    const rtl_id_t *a = pa, *b = pb;
    int             c = num_cmp(a->sn, b->sn);

    return c ? c : num_cmp(a->loc, b->loc);
}
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
        int            rtls = 0;
        rtl_id_t       ids[8];

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
                if (kind_of(r) == ADSB_KIND_RTL && rtls < 8) {
                    char  hub[32], attr[8];
                    char *dot;

                    snprintf(ids[rtls].loc, sizeof(ids[rtls].loc), "%s", e->d_name);
                    snprintf(path, sizeof(path), USB_DEVICES "/%s/serial", e->d_name);
                    if (!read_line(path, ids[rtls].sn, sizeof(ids[rtls].sn))) ids[rtls].sn[0] = 0;
                    /* its hub's own power (bmAttributes bit 6): a stick straight on the HOST port ("3-1") has the
                     * radio's, whatever the root hub says */
                    snprintf(hub, sizeof(hub), "%s", e->d_name);
                    ids[rtls].self = false;
                    if ((dot = strrchr(hub, '.'))) {
                        *dot = 0;
                        snprintf(path, sizeof(path), USB_DEVICES "/%s/bmAttributes", hub);
                        ids[rtls].self = read_line(path, attr, sizeof(attr)) && (strtoul(attr, NULL, 16) & 0x40);
                    }
                    rtls++;
                }
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
        qsort(ids, (size_t)rtls, sizeof(ids[0]), rtl_id_cmp);
        pthread_mutex_lock(&p_rtl_lock);
        memcpy(p_rtl_id, ids, sizeof(ids[0]) * (size_t)rtls);
        pthread_mutex_unlock(&p_rtl_lock);
        atomic_store(&p_rtls, rtls);
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

int adsb_rx_rtl_count(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_rtls);
}

/* RTLs the watch channels have (rtl_watch.c), a bit each place: no spare of AIRSPACE's takes them */
static atomic_uint p_claimed;

void adsb_rx_rtl_claim(unsigned places) { atomic_store(&p_claimed, places); }

/* The nth RTL (0 the first) in RTL order that B's daemon neither holds nor was asked for and no watch channel has
 * (self_only: behind a hub of its own power); its place, serial and location; -1 none */
static int spare_rtl(int nth, bool self_only, char *sn, size_t sn_n, char *loc, size_t loc_n) {
    int      held = rtl_spectrum_dev_rtl(), want = rtl_spectrum_used() == RTL_DEV_RTL ? rtl_spectrum_used_rtl() : -1;
    int      n = atomic_load(&p_rtls), got = -1;
    unsigned claimed = atomic_load(&p_claimed);

    pthread_mutex_lock(&p_rtl_lock);
    for (int i = 0; i < n && got < 0; i++)
        if (i != held && i != want && !(claimed & (1u << i)) && (!self_only || p_rtl_id[i].self) && nth-- == 0) {
            got = i;
            if (sn) snprintf(sn, sn_n, "%s", p_rtl_id[i].sn);
            if (loc) snprintf(loc, loc_n, "%s", p_rtl_id[i].loc);
        }
    pthread_mutex_unlock(&p_rtl_lock);
    return got;
}

bool adsb_rx_rtl_id(int n, char *sn, size_t sn_n, char *loc, size_t loc_n) {
    pthread_once(&p_once, presence_start);
    if (n < 0 || n >= atomic_load(&p_rtls)) return false;
    pthread_mutex_lock(&p_rtl_lock);
    snprintf(sn, sn_n, "%s", p_rtl_id[n].sn);
    snprintf(loc, loc_n, "%s", p_rtl_id[n].loc);
    pthread_mutex_unlock(&p_rtl_lock);
    return true;
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
    adsb_kind_t k[SLOTS];

    assign(atomic_load(&p_mask), daemon_dev(), atomic_load(&p_dump), atomic_load(&want_mask), k);
    for (int i = 0; i < SLOTS; i++)
        if (k[i] != ADSB_KIND_NONE) m |= KB(k[i]);
    return m;
}
