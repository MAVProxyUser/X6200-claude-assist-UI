/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#include "gps.h"

#include "lvgl/lvgl.h"
#include "events.h"
#include "dialog_gps.h"
#include "usb_devices.h"
#include "pubsub_ids.h"

#include <errno.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <math.h>       /* isfinite */
#include <time.h>
#include <unistd.h>

/* gpsd, and how the GUI stays with it (#237). Times in ms; the host test (tools/gps) makes them short */
#ifndef GPSD_HOST
#define GPSD_HOST       "localhost"
#endif
#ifndef GPSD_PORT
#define GPSD_PORT       "2947"
#endif
#ifndef QUIET_MS
#define QUIET_MS        5000        /* no report this long: connected again, as gpsd may have let this client go */
#endif
#ifndef QUIET_MAX_MS
#define QUIET_MAX_MS    60000       /* and twice as long each time it stays quiet (no GPS plugged in), up to this */
#endif
#ifndef RETRY_MAX_MS
#define RETRY_MAX_MS    60000       /* gpsd not there or its connection lost: again after 1 s, doubling up to this */
#endif
#ifndef RESTART_MS
#define RESTART_MS      60000       /* a GPS plugged in and no report this long: gpsd started again */
#endif
#ifndef RESTART_MAX_MS
#define RESTART_MAX_MS  1800000     /* and while that brings nothing, the next one twice as long after, up to this */
#endif
#ifndef WINDOW_MS
#define WINDOW_MS       100         /* the GPS window's copies at most this often */
#endif
#ifndef GPS_LINKS
#define GPS_LINKS       "/dev/gps*" /* udev's names for the ports of the GPS receivers it knows (25-gpsd.rules) */
#endif
#ifndef GPSD_RESTART
#ifdef HOST_BUILD
#define GPSD_RESTART    NULL        /* a PC's gpsd is not the GUI's to start */
#else
#define GPSD_RESTART    "/etc/init.d/S50gpsd restart >/dev/null 2>&1"
#endif
#endif

#define MIN_MS(a, b)    ((a) < (b) ? (a) : (b))

#if HAVE_LIBGPS
static struct gps_data_t    gpsdata;
static int64_t              heard;          /* the last report from gpsd, or the last time the GUI started it */
static char                 port[sizeof(gpsdata.dev.path)];    /* where the last report came from */
static gps_mask_t           win_set;        /* the flags of the reports not yet in a copy for the GPS window */
static int64_t              win_at;         /* the last copy */
#endif
static volatile gps_status_t status = GPS_STATUS_WAITING;

/* A USB device plugged in cuts the pause before the next try short */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond;
static bool            woken;

/* Positions handed to other threads: gpsd's last 2D/3D fix */
static pthread_mutex_t pos_lock = PTHREAD_MUTEX_INITIALIZER;
static double          gpsd_lat, gpsd_lon;
static int64_t         gpsd_time;    /* when it came */
static bool            gpsd_fix;     /* one came */

/* The monotonic clock in ms: the wall clock jumps when NTP or the GPS first sets it, the radio having no RTC battery */
static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* The phone's position, from the Panel page's GPS button (remote_server.c "pos"), under the same lock; its age on
 * the monotonic clock too, as the phone's own clock may set the wall clock (clk) */
/* gpsd's last report, for gps_health(), under pos_lock: copied as it comes, its rate counted */
static gps_health_t health;
static int64_t      health_at = -1;     /* now_ms() of the last report */
static int64_t      fix_at = -1;        /* of the last 2D/3D fix */
static int64_t      rate_from;          /* the rate's window: from when, how many new positions in it */
static int          rate_n, reports_n;
static long         rate_last_ns;       /* the last position's GPS time, to count each once */
static long         rate_last_s;

static double  phone_lat, phone_lon;
static int     phone_acc;           /* m, as the phone says */
static int64_t phone_time;          /* when it came (now_ms) */
static bool    phone_fix;           /* one came */

#if HAVE_LIBGPS

/* The pause before the next try, cut short by a USB device plugged in */
static void pause_ms(int64_t ms) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000;
    if (t.tv_nsec >= 1000000000) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&lock);
    while (!woken && pthread_cond_timedwait(&cond, &lock, &t) == 0) {
    }
    woken = false;
    pthread_mutex_unlock(&lock);
}

static bool gpsd_connect(void) {
    if (gps_open(GPSD_HOST, GPSD_PORT, &gpsdata) != 0) {
        return false;
    }
    if (gps_stream(&gpsdata, WATCH_ENABLE | WATCH_JSON, NULL) != 0) {
        gps_close(&gpsdata);
        return false;
    }
    return true;
}

/* Closed without a WATCH_DISABLE first: gpsd lets a client go when it closes, and a write to a connection gpsd has
 * closed already would end the GUI (SIGPIPE) */
static void gpsd_disconnect(void) {
    gps_close(&gpsdata);
}

/* The health of it all, from the report in gpsdata (libgps keeps each part from its own last message) */
static void health_take(void) {
    static const char sys[] = {'G', 'S', 'E', 'B', '?', 'Q', 'R', 'I'};      /* by u-blox gnssid */
    int64_t           now   = now_ms();
    int               n     = 0;

    pthread_mutex_lock(&pos_lock);
    health_at = now;
    snprintf(health.path, sizeof(health.path), "%s", gpsdata.dev.path[0] ? gpsdata.dev.path : port);
    if (gpsdata.dev.driver[0]) snprintf(health.driver, sizeof(health.driver), "%s", gpsdata.dev.driver);
    health.mode = gpsdata.fix.mode;
    health.seen = gpsdata.satellites_visible;
    health.used = gpsdata.satellites_used;
    for (int i = 0; i < gpsdata.satellites_visible && i < MAXCHANNELS && n < GPS_SATS; i++) {
        const struct satellite_t *v = &gpsdata.skyview[i];

        if (v->PRN <= 0) continue;
        health.sat[n].prn  = v->PRN;
        health.sat[n].ss   = isfinite(v->ss) && v->ss > 0 ? (short)lround(v->ss) : 0;
        health.sat[n].used = v->used;
        health.sat[n].el   = isfinite(v->elevation) ? (short)lround(v->elevation) : -99;
        health.sat[n].az   = isfinite(v->azimuth) ? (short)lround(v->azimuth) : 0;
        health.sat[n].sys  = v->gnssid < sizeof(sys) ? sys[v->gnssid] : '?';
        n++;
    }
    health.nsat  = n;
    health.hdop  = gpsdata.dop.hdop;
    health.pdop  = gpsdata.dop.pdop;
    health.eph   = gpsdata.fix.eph;
    health.epv   = gpsdata.fix.epv;
    health.speed = gpsdata.fix.speed;
    health.track = gpsdata.fix.track;
    health.utc   = gpsdata.fix.time.tv_sec > 0 ? (long)gpsdata.fix.time.tv_sec : health.utc;
    if (gpsdata.fix.mode >= MODE_2D && isfinite(gpsdata.fix.latitude) && isfinite(gpsdata.fix.longitude)) {
        health.lat = gpsdata.fix.latitude;
        health.lon = gpsdata.fix.longitude;
        health.alt = gpsdata.fix.mode == MODE_3D ? gpsdata.fix.altMSL : NAN;
        fix_at     = now;
    }
    if (gpsdata.fix.time.tv_sec > 0 &&                  /* a new position: counted once, for the rate */
        (gpsdata.fix.time.tv_sec != rate_last_s || gpsdata.fix.time.tv_nsec != rate_last_ns)) {
        rate_last_s  = (long)gpsdata.fix.time.tv_sec;
        rate_last_ns = gpsdata.fix.time.tv_nsec;
        rate_n++;
    }
    reports_n++;
    if (now - rate_from >= 3000) {                      /* over three seconds */
        health.rate    = rate_from ? rate_n * 1000.0 / (double)(now - rate_from) : 0;
        health.reports = rate_from ? reports_n * 1000.0 / (double)(now - rate_from) : 0;
        rate_from      = now;
        rate_n = reports_n = 0;
    }
    pthread_mutex_unlock(&pos_lock);
}

/* A report from gpsd: the position for the other threads; for the GPS window, its flags into the next copy */
static void report(void) {
    status = GPS_STATUS_WORKING;
    if (gpsdata.dev.path[0]) {
        snprintf(port, sizeof(port), "%s", gpsdata.dev.path);
    }
    if (gpsdata.fix.mode >= MODE_2D && isfinite(gpsdata.fix.latitude) && isfinite(gpsdata.fix.longitude)) {
        pthread_mutex_lock(&pos_lock);
        gpsd_lat = gpsdata.fix.latitude;
        gpsd_lon = gpsdata.fix.longitude;
        gpsd_time = now_ms();
        gpsd_fix = true;
        pthread_mutex_unlock(&pos_lock);
    }
    health_take();
    win_set |= gpsdata.set;
}

/* The GPS window's copy of what gpsd said, with the flags of every report since the last copy: once no more lines
 * are waiting, at most every WINDOW_MS. A TPV and its SKY come as one; and the GUI's queue (63) is not filled when
 * lines come faster than ever before, the 100 ms sleep a line having held the copies to ten a second (#237) */
static void window_copy(int64_t now) {
    if (!win_set || now - win_at < WINDOW_MS) {
        return;
    }
    if (dialog_gps->run) {
        struct gps_data_t *msg = malloc(sizeof(struct gps_data_t));

        if (msg) {
            memcpy(msg, &gpsdata, sizeof(*msg));
            msg->set |= win_set;
            event_send(dialog_gps->obj, EVENT_GPS, msg);
        }
    }
    win_set = 0;
    win_at = now;
}

enum { RX_DOWN, RX_GONE, RX_QUIET };

/* The GPS thread's: the pause before the next try once gpsd was not there or let the GUI go, the quiet that makes it
 * connect again, the quiet that makes it start gpsd again; and what the log has said */
static int64_t wait_ms = 1000;
static int64_t quiet_ms = QUIET_MS;
static int64_t restart_ms = RESTART_MS;
static bool    down;        /* gpsd not there */
static bool    trouble;     /* the GPS went quiet, or gpsd let the GUI go */

/* The first report on a connection: all as at the start, and the log says so after trouble */
static void reporting(void) {
    if (trouble) {
        LV_LOG_USER("gps: reports again");
    }
    trouble = false;
    wait_ms = 1000;
    quiet_ms = QUIET_MS;
    restart_ms = RESTART_MS;
}

/* Every line gpsd sends, as soon as it is there, until it closes the connection or it fails (RX_GONE), or no report
 * comes for quiet_ms (RX_QUIET); talked: one came. Up to v0.3.10 one line every 100 ms at most: a GPS that gave gpsd
 * more than ten lines a second to send backed up in the socket until gpsd, a write to the GUI cut short, let it go;
 * and a connection closed by gpsd was then read on and on, never made again, the icon grey until a restart (#237) */
static int data_receive(bool *talked) {
    int64_t last = now_ms();

    while (true) {
        int     r = gps_read(&gpsdata, NULL, 0);
        int64_t now;

        if (r < 0) {
            return RX_GONE;     /* closed, failed, or a line longer than libgps's buffer: this connection is done */
        }
        now = now_ms();
        /* Any line once a TPV has come (libgps keeps its MODE_SET): the SKYs too, for the window's satellites */
        if (r > 0 && (gpsdata.set & MODE_SET)) {
            if (!*talked) {
                reporting();
                *talked = true;
            }
            report();
            last = heard = now;
        }
        if (now - last >= quiet_ms) {
            return RX_QUIET;
        }
        if (r == 0) {           /* no whole line in libgps's buffer: what comes next, or the end of the wait */
            struct pollfd p = {gpsdata.gps_fd, POLLIN, 0};
            int64_t       left = quiet_ms - (now - last);

            window_copy(now);
            if (win_set && WINDOW_MS - (now - win_at) < left) {
                left = WINDOW_MS - (now - win_at);
            }
            if (poll(&p, 1, (int)left) < 0 && errno != EINTR) {
                return RX_GONE;
            }
        }
    }
}

/* A GPS plugged in, into what: the port the last report came from, or one udev knows as a GPS receiver */
static bool plugged(char *what, size_t size) {
    glob_t g;

    if (port[0] && access(port, F_OK) == 0) {
        snprintf(what, size, "%s", port);
        return true;
    }
    if (glob(GPS_LINKS, 0, NULL, &g) == 0) {
        snprintf(what, size, "%s", g.gl_pathv[0]);
        globfree(&g);
        return true;
    }
    return false;
}

/* No report for restart_ms with a GPS plugged in: gpsd started again (S50gpsd), and the next time twice as long after,
 * up to RESTART_MAX_MS, while that brings nothing. For a gpsd that stopped, or that has a port that went quiet or came
 * back under a name it was not told of; a GPS that only a new plug-in brings back costs a start of gpsd now and then.
 * A working GPS reports every second, fix or not: it is never touched (#237) */
static void restart_gpsd(void) {
    const char *cmd = GPSD_RESTART;
    char        what[sizeof(port)];
    int64_t     quiet = now_ms() - heard;

    if (quiet < restart_ms || !plugged(what, sizeof(what))) {
        return;
    }
    heard = now_ms();
    restart_ms = MIN_MS(restart_ms * 2, RESTART_MAX_MS);
    if (!cmd) {
        return;
    }
    LV_LOG_USER("gps: no report for %d s with a GPS on %s: gpsd started again", (int)(quiet / 1000), what);
    if (system(cmd) != 0) {
        LV_LOG_USER("gps: %s failed", cmd);
    }
}

static void * gps_thread(void *arg) {
    heard = now_ms();
    while (true) {
        bool talked = false;
        int  why    = RX_DOWN;

        status = GPS_STATUS_WAITING;
        if (gpsd_connect()) {
            if (down) {
                LV_LOG_USER("gps: gpsd there again");
            }
            down = false;
            why = data_receive(&talked);
            status = GPS_STATUS_RESTARTING;
            gpsd_disconnect();
        } else if (!down) {
            LV_LOG_USER("gps: gpsd not there (%s), trying again, less and less often", gps_errstr(errno));
            down = true;
        }
        if (why == RX_QUIET) {
            /* Connected again at once; when nothing came, later each time */
            if (talked) {
                LV_LOG_USER("gps: no report for %d s, connecting to gpsd again", (int)(quiet_ms / 1000));
                trouble = true;
            } else {
                quiet_ms = MIN_MS(quiet_ms * 2, QUIET_MAX_MS);
            }
        } else {
            if (why == RX_GONE && !trouble) {
                LV_LOG_USER("gps: gpsd closed the connection, connecting again");
                trouble = true;
            }
            pause_ms(wait_ms);
            if (!talked) {
                wait_ms = MIN_MS(wait_ms * 2, RETRY_MAX_MS);
            }
        }
        restart_gpsd();
    }
    return NULL;
}

#endif /* HAVE_LIBGPS */

static void on_usb_device_change(void * s, lv_msg_t * msg) {
    enum usb_devices_event_t event = (enum usb_devices_event_t)(intptr_t)msg->payload;

    if (event == USB_DEV_ADDED) {
        pthread_mutex_lock(&lock);
        woken = true;
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&lock);
    }
}

void gps_init() {
    pthread_condattr_t a;

    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&cond, &a);
    pthread_condattr_destroy(&a);
#if HAVE_LIBGPS
    pthread_t thread;

    pthread_create(&thread, NULL, gps_thread, NULL);
    pthread_detach(thread);
#else
    LV_LOG_WARN("Built without libgps: no GPS");
#endif
    lv_msg_subscribe(MSG_USB_DEVICE_CHANGED, on_usb_device_change, NULL);
}


gps_status_t gps_status() {
    return status;
}

bool gps_last_fix(double *lat, double *lon, int *age) {
    bool have;

    pthread_mutex_lock(&pos_lock);
    have = gpsd_fix;
    if (have) {
        *lat = gpsd_lat;
        *lon = gpsd_lon;
        *age = (int)((now_ms() - gpsd_time) / 1000);
    }
    pthread_mutex_unlock(&pos_lock);
    return have;
}

void gps_health(gps_health_t *h) {
    int64_t now = now_ms();

    pthread_mutex_lock(&pos_lock);
    *h = health;
    h->report_age_ms = health_at < 0 ? -1 : (int)(now - health_at);
    h->fix_age_ms    = fix_at < 0 ? -1 : (int)(now - fix_at);
    h->reporting     = health_at >= 0 && now - health_at < 5000;
    if (fix_at < 0) h->lat = h->lon = h->alt = NAN;
    pthread_mutex_unlock(&pos_lock);
}

/* The first line of a file, its end cut */
static void first_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");

    out[0] = 0;
    if (!f) return;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

void gps_bt_link(char *mac, size_t mac_n, char *name, size_t name_n, char *state, size_t state_n) {
    FILE  *f = fopen("/mnt/btcat.conf", "r");
    char   l[128];
    glob_t g;

    mac[0] = name[0] = 0;
    while (f && fgets(l, sizeof(l), f))
        if (!strncmp(l, "BTGPS_MAC=", 10)) {
            snprintf(mac, mac_n, "%s", l + 10);
            mac[strcspn(mac, "\r\n \"'")] = 0;
        }
    if (f) fclose(f);
    first_line("/tmp/btgps.state", state, state_n);
    if (!mac[0]) return;
    snprintf(l, sizeof(l), "/var/lib/bluetooth/*/%s/info", mac);   /* BlueZ's own record of it: its name */
    if (glob(l, 0, NULL, &g) == 0) {
        FILE *i = fopen(g.gl_pathv[0], "r");

        while (i && fgets(l, sizeof(l), i))
            if (!strncmp(l, "Name=", 5)) {
                snprintf(name, name_n, "%s", l + 5);
                name[strcspn(name, "\r\n")] = 0;
                break;
            }
        if (i) fclose(i);
        globfree(&g);
    }
}

void gps_phone_set(double lat, double lon, int acc) {
    pthread_mutex_lock(&pos_lock);
    phone_lat  = lat;
    phone_lon  = lon;
    phone_acc  = acc;
    phone_time = now_ms();
    phone_fix  = true;
    pthread_mutex_unlock(&pos_lock);
}

bool gps_phone_fix(double *lat, double *lon, int *age, int *acc) {
    bool have;

    pthread_mutex_lock(&pos_lock);
    have = phone_fix;
    if (have) {
        *lat = phone_lat;
        *lon = phone_lon;
        *acc = phone_acc;
        *age = (int)((now_ms() - phone_time) / 1000);
    }
    pthread_mutex_unlock(&pos_lock);
    return have;
}
