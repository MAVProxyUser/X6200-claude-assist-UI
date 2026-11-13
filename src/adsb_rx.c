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
 *  The thread looks for a receiver every 2 s while it has none.
 */

#include "adsb_rx.h"

#include "adsb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define USB_DEVICES "/sys/bus/usb/devices"
#define CDC_ACM     "/sys/bus/usb/drivers/cdc_acm"
#define QUIET_S     120

static const struct {
    const char *vid, *pid, *product;    /* product: a part its USB product name must have, or NULL */
    const char *name;
    bool        new_id;                 /* cdc_acm told its ID */
    const char *init;                   /* sent on opening, each answered, and again after QUIET_S without a frame */
} receivers[] = {
    {"04d8", "f8e8", NULL, "GNS 5890", true, NULL},
    {"04d8", "000a", "ADSB", "MICROADSB", false,
     "#00\r#43-00\r#51-01-00\r#37-03\r#51-00-00\r#37-03\r#43-00\r#38\r#43-02\r"},
};
#define N_RECEIVERS (int)(sizeof(receivers) / sizeof(receivers[0]))

static pthread_t       thread;
static atomic_bool     started;
static atomic_int      state = ADSB_RX_OFF;
static atomic_ullong   bytes;
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

static void *rx_thread(void *arg) {
    (void)arg;
    int    fd = -1, rx = -1;
    char   frame[56], reply[64];
    size_t len = 0, rlen = 0;
    bool   inside = false, stamped = false, replying = false;
    double last = 0, init_at = 0;           /* init_at: the start commands are due, 0 sent */

    for (;;) {
        if (fd < 0) {
            char usb[64], dev[32];

            set_dev("", "");
            if ((rx = find_stick(usb, sizeof(usb))) < 0) {
                state = ADSB_RX_NO_STICK;
            } else if (access(CDC_ACM, F_OK) != 0) {
                state = ADSB_RX_NO_DRIVER;
            } else if (!find_tty(usb, dev, sizeof(dev))) {
                state = ADSB_RX_OPENING;
                if (receivers[rx].new_id) bind_stick(rx, usb);
            } else if ((fd = open_port(dev)) >= 0) {
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
