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
 *  The thread looks for a receiver every 2 s while it has none, and each second that the one it has is still there
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
    bool        sdr;                    /* an RTL-SDR: dump1090 decodes it */
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
};
#define N_RECEIVERS (int)(sizeof(receivers) / sizeof(receivers[0]))

static pthread_t       thread;
static atomic_bool     started;
static atomic_int      state = ADSB_RX_OFF;
static atomic_ullong   bytes;
static atomic_bool     lent;            /* the RTL-SDR lent: adsb_rx_lend() */
static pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
static char            dev_path[32], dev_name[16], dev_info[48];

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

/* A receiver's USB device ("3-1") and its row in receivers[], or -1 */
static int find_stick(char *usb, size_t n) {
    DIR           *d = opendir(USB_DEVICES);
    struct dirent *e;
    int            found = -1;

    if (!d) return -1;
    while (found < 0 && (e = readdir(d))) {
        char path[300], v[8], p[8], prod[64];

        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;     /* interfaces have a ':' */
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idVendor", e->d_name);
        if (!read_line(path, v, sizeof(v))) continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/idProduct", e->d_name);
        if (!read_line(path, p, sizeof(p))) continue;
        snprintf(path, sizeof(path), USB_DEVICES "/%s/product", e->d_name);
        if (!read_line(path, prod, sizeof(prod))) prod[0] = 0;
        for (int r = 0; r < N_RECEIVERS && found < 0; r++)
            if (!strcmp(v, receivers[r].vid) && !strcmp(p, receivers[r].pid) &&
                (!receivers[r].product || strstr(prod, receivers[r].product))) {
                snprintf(usb, n, "%s", e->d_name);
                found = r;
            }
    }
    closedir(d);
    return found;
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

static void set_dev(const char *dev, const char *name) {
    pthread_mutex_lock(&dev_lock);
    snprintf(dev_path, sizeof(dev_path), "%s", dev);
    snprintf(dev_name, sizeof(dev_name), "%s", name);
    dev_info[0] = 0;
    pthread_mutex_unlock(&dev_lock);
}

/* A receiver's answer to a command, "#00-00-0E-04-...": its firmware, said */
static void set_info(const char *line) {
    unsigned a, b, fw;

    if (sscanf(line, "#%x-%x-%x", &a, &b, &fw) == 3 && a == 0) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev_info, sizeof(dev_info), "FW %02X", fw);
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
                    set_info(line);
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
 * the stick itself */
static bool d1090_start(void) {
    int  pp[2] = {-1, -1};
    bool fed;

    if (access(DUMP1090, X_OK) != 0) return false;
    fed = rtl_spectrum_daemon() && pipe2(pp, O_CLOEXEC) == 0;
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
            snprintf(dev_info, sizeof(dev_info), "%s", tuner);
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
        snprintf(dev_info, sizeof(dev_info), "%s", tuner);
        pthread_mutex_unlock(&dev_lock);
    }
}

static void *rx_thread(void *arg) {
    (void)arg;
    int    fd = -1, rx = -1, held_num = 0;
    char   frame[56], reply[64], held[64] = "";     /* held: the receiver's USB device ("3-1"), held_num its number */
    size_t len = 0, rlen = 0;
    bool   inside = false, stamped = false, replying = false;
    double last = 0, init_at = 0, looked = 0;       /* init_at: the start commands are due, 0 sent */

    for (;;) {
        /* The RTL-SDR lent: its dump1090 stopped, not started again till it is back */
        if (atomic_load(&lent) && (rx < 0 || receivers[rx].sdr)) {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
            d1090_stop();
            set_dev("", "");
            state = ADSB_RX_LENT;
            usleep(300000);
            rx = -1;
            continue;
        }
        if (fd < 0) {
            char usb[64], dev[32];

            set_dev("", "");
            if ((rx = find_stick(usb, sizeof(usb))) < 0) {
                state = ADSB_RX_NO_STICK;
                d1090_stop();                   /* the RTL-SDR went: its dump1090 too */
            } else if (receivers[rx].sdr) {     /* dump1090 decodes it; its raw frames over TCP */
                if (!d1090_alive()) {
                    if (!d1090_start()) {
                        state = ADSB_RX_NO_DRIVER;
                        sleep(2);
                        continue;
                    }
                    state = ADSB_RX_OPENING;
                    sleep(3);                   /* the tuner set up, the port open */
                }
                if ((fd = d1090_connect()) >= 0) {
                    snprintf(held, sizeof(held), "%s", usb);
                    held_num = usb_devnum(usb);
                    set_dev("127.0.0.1:30002", receivers[rx].name);
                    d1090_tuner();
                    state  = ADSB_RX_ON;
                    inside = replying = false;
                    last   = adsb_now();
                    init_at = 0;
                    continue;
                }
                state = ADSB_RX_OPENING;
            } else if (access(CDC_ACM, F_OK) != 0) {
                state = ADSB_RX_NO_DRIVER;
            } else if (!find_tty(usb, dev, sizeof(dev))) {
                state = ADSB_RX_OPENING;
                if (receivers[rx].new_id) bind_stick(rx, usb);
            } else if ((fd = open_port(dev)) >= 0) {
                snprintf(held, sizeof(held), "%s", usb);
                held_num = usb_devnum(usb);
                set_dev(dev, receivers[rx].name);
                state   = ADSB_RX_ON;
                inside  = replying = false;
                last    = adsb_now();
                init_at = receivers[rx].init ? last + 1.0 : 0;     /* sent at once, the microADSB missed them */
                continue;
            } else {
                state = ADSB_RX_OPENING;
            }
            sleep(2);
            continue;
        }
        if (adsb_now() - looked >= 1.0) {       /* still the receiver it opened, where it was */
            looked = adsb_now();
            if (!held_num || usb_devnum(held) != held_num) {
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
                    set_info(reply);
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
    state = ADSB_RX_NO_STICK;
    if (pthread_create(&thread, NULL, rx_thread, NULL) != 0) {
        state   = ADSB_RX_OFF;
        started = false;
        return;
    }
    pthread_detach(thread);
}

adsb_rx_state_t adsb_rx_state(char *dev, size_t n) {
    if (dev && n) {
        pthread_mutex_lock(&dev_lock);
        snprintf(dev, n, "%s", dev_path);
        pthread_mutex_unlock(&dev_lock);
    }
    return (adsb_rx_state_t)state;
}

void adsb_rx_name(char *name, size_t n) {
    pthread_mutex_lock(&dev_lock);
    if (dev_info[0]) snprintf(name, n, "%s %s", dev_name, dev_info);
    else snprintf(name, n, "%s", dev_name);
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
static atomic_int     p_looks;          /* looks at USB so far: 0, nothing known yet */
static pthread_once_t p_once = PTHREAD_ONCE_INIT;

static void *presence_thread(void *arg) {
    (void)arg;
    for (;;) {
        DIR           *d   = opendir(USB_DEVICES);
        struct dirent *e;
        bool           sdr = false, rcv = false;
        int            id  = 0;

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
                if (receivers[r].sdr) {
                    sdr = true;
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

int adsb_rx_sdr_id(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_sdr_id);
}

int adsb_rx_usb_looks(void) {
    pthread_once(&p_once, presence_start);
    return atomic_load(&p_looks);
}

