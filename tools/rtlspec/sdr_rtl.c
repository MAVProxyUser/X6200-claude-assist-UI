/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - an RTL-SDR stick for x6x00-rtlspec (sdr.h), through librtlsdr (the RTL-SDR Blog's, patched so a stick
 *  pulled out mid-stream has all its transfers back before they are freed: buildroot/br2_external/package/rtlsdr-blog).
 *  Its samples are the pipeline's own, unsigned 8-bit I/Q. Receive only.
 */

#include "sdr.h"

#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libusb.h>
#include <rtl-sdr.h>

#define READ_LEN (16384 * 2)

/* Which RTL: by its place among the RTLs on USB in serial order, then by USB location (sticks that all say 00000001),
 * RTL0 the first (-d, or "k RTL<n>"). A KrakenSDR's five are serials 1000-1004, so RTL0-RTL4 are its CH0-CH4; a stick
 * alone is RTL0 (the user, 2026-11-16: "labled with an index like RTL0 RTL1 RTL2 ... all single stick RTL should be
 * called RTL0", "by serial and usb bus location"). One not there: RTL0 meanwhile. The GUI orders them the same way from
 * sysfs (adsb_rx.c), so RTL<n> is the same stick on both sides */
int sdr_rtl_index;
int sdr_rtl_open_at = -1;               /* the open one's place (RTL<n>), -1 none */
char sdr_rtl_open_loc[32];
bool sdr_rtl_exact;
static char            want_sn[64], want_loc[32], open_sn[64];     /* sdr_rtl_want; the open one's serial */
static pthread_mutex_t want_lock = PTHREAD_MUTEX_INITIALIZER;

static rtlsdr_dev_t *volatile dev;
/* rtl_gain's last, INT_MIN none since the open: set again where librtlsdr set the tuner up anew (rtl_freq) */
static int gain_tenths = INT_MIN;
static sdr_rx_cb              rx_cb;
static volatile int          *rx_halt;

static const char *tuner_name(enum rtlsdr_tuner t) {
    switch (t) {
        case RTLSDR_TUNER_E4000: return "E4000";
        case RTLSDR_TUNER_FC0012: return "FC0012";
        case RTLSDR_TUNER_FC0013: return "FC0013";
        case RTLSDR_TUNER_FC2580: return "FC2580";
        case RTLSDR_TUNER_R820T: return "R820T";
        case RTLSDR_TUNER_R828D: return "R828D";
        default: return "UNKNOWN";
    }
}

static int rtl_present(void) {
    return rtlsdr_get_device_count() > 0;
}

int sdr_rtl_count(void) {
    return (int)rtlsdr_get_device_count();
}

/* The order: serials, numbers as numbers (1000 before 1004, 9 before 10), else as text; then the USB location
 * ("3-1.4"), its numbers as numbers; then librtlsdr's own order */
typedef struct { int idx; char sn[64], loc[32]; } rtl_sn_t;

static int num_cmp(const char *a, const char *b) {       /* "3-1.10" after "3-1.9" */
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

static int sn_cmp(const void *pa, const void *pb) {
    const rtl_sn_t *a = pa, *b = pb;
    int             c = num_cmp(a->sn, b->sn);

    if (!c) c = num_cmp(a->loc, b->loc);
    return c ? c : a->idx - b->idx;
}

static bool read_sys(const char *loc, const char *what, char *out, size_t n) {
    char  path[96];
    FILE *f;

    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/%s", loc, what);
    if (!(f = fopen(path, "r"))) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\r\n")] = 0;
    return ok;
}

/* Each librtlsdr index's serial and USB location: librtlsdr counts the known sticks in libusb's list order, so the k-th
 * 0bda:2838 or 0bda:2832 there is its index k, and its ports give the location, the serial sysfs's. Another kind of
 * stick in the list (the counts differ): serials from librtlsdr, no locations */
static int rtl_list(rtl_sn_t *l, int max) {
    int             cnt = (int)rtlsdr_get_device_count(), k = 0;
    libusb_context *ctx = NULL;
    libusb_device **list;

    if (cnt > max) cnt = max;
    if (cnt > 0 && libusb_init(&ctx) == 0) {
        ssize_t n = libusb_get_device_list(ctx, &list);

        for (ssize_t i = 0; i < n && k < max; i++) {
            struct libusb_device_descriptor dd;
            uint8_t                         ports[8];

            if (libusb_get_device_descriptor(list[i], &dd) || dd.idVendor != 0x0bda ||
                (dd.idProduct != 0x2838 && dd.idProduct != 0x2832))
                continue;
            int np = libusb_get_port_numbers(list[i], ports, 8), at;

            l[k].idx = k;
            at = snprintf(l[k].loc, sizeof(l[k].loc), "%u", libusb_get_bus_number(list[i]));
            for (int p = 0; p < np && at < (int)sizeof(l[k].loc); p++)
                at += snprintf(l[k].loc + at, sizeof(l[k].loc) - (size_t)at, "%c%u", p ? '.' : '-', ports[p]);
            if (!read_sys(l[k].loc, "serial", l[k].sn, sizeof(l[k].sn))) l[k].sn[0] = 0;
            k++;
        }
        if (n >= 0) libusb_free_device_list(list, 1);
        libusb_exit(ctx);
    }
    if (k != cnt) {
        for (k = 0; k < cnt; k++) {
            char m[256] = "", p[256] = "", sn[256] = "";

            rtlsdr_get_device_usb_strings((uint32_t)k, m, p, sn);
            l[k].idx    = k;
            l[k].loc[0] = 0;
            snprintf(l[k].sn, sizeof(l[k].sn), "%s", sn);
        }
    }
    qsort(l, (size_t)k, sizeof(l[0]), sn_cmp);
    return k;
}

/* The place of the RTL with serial sn (and location loc when several share the serial; one alone may have moved to
 * another port), -1 none or not told apart */
static int rtl_by_sn(const rtl_sn_t *l, int cnt, const char *sn, const char *loc) {
    int found = -1, n = 0;

    for (int i = 0; i < cnt; i++)
        if (!strcmp(l[i].sn, sn)) {
            n++;
            if (found < 0 || (loc[0] && !strcmp(l[i].loc, loc))) found = i;
        }
    if (n > 1 && !(loc[0] && found >= 0 && !strcmp(l[found].loc, loc))) return -1;
    return found;
}

void sdr_rtl_want(const char *sn, const char *loc) {
    pthread_mutex_lock(&want_lock);
    snprintf(want_sn, sizeof(want_sn), "%s", sn ? sn : "");
    snprintf(want_loc, sizeof(want_loc), "%s", loc && strcmp(loc, "-") ? loc : "");
    pthread_mutex_unlock(&want_lock);
}

bool sdr_rtl_want_sn(void) {
    pthread_mutex_lock(&want_lock);
    bool w = want_sn[0] != 0;
    pthread_mutex_unlock(&want_lock);
    return w;
}

bool sdr_rtl_wanted_there(void) {
    char     sn[64], loc[32];
    rtl_sn_t l[16];

    pthread_mutex_lock(&want_lock);
    snprintf(sn, sizeof(sn), "%s", want_sn);
    snprintf(loc, sizeof(loc), "%s", want_loc);
    pthread_mutex_unlock(&want_lock);
    return sn[0] && rtl_by_sn(l, rtl_list(l, 16), sn, loc) >= 0;
}

/* The stick asked for is the one open: the one rtl_by_sn finds now (a stick alone with its serial may have moved to
 * another port: by its location then, not the location asked for, or a moved stick was opened and closed in a loop) */
bool sdr_rtl_wanted_open(void) {
    char     sn[64], loc[32], osn[64], oloc[32];
    int      at;
    rtl_sn_t l[16];

    pthread_mutex_lock(&want_lock);
    snprintf(sn, sizeof(sn), "%s", want_sn);
    snprintf(loc, sizeof(loc), "%s", want_loc);
    snprintf(osn, sizeof(osn), "%s", open_sn);
    snprintf(oloc, sizeof(oloc), "%s", sdr_rtl_open_loc);
    at = sdr_rtl_open_at;
    pthread_mutex_unlock(&want_lock);
    if (!sn[0] || at < 0 || strcmp(sn, osn)) return false;
    int f = rtl_by_sn(l, rtl_list(l, 16), sn, loc);

    return f >= 0 && !strcmp(l[f].loc, oloc);
}

/* librtlsdr's index of RTL at (its serial and location in sn, loc), -1 none */
static int rtl_at(int at, char *sn, size_t n, char *loc, size_t ln) {
    rtl_sn_t l[16];
    int      cnt = rtl_list(l, 16);

    if (at < 0 || at >= cnt) return -1;
    snprintf(sn, n, "%s", l[at].sn);
    snprintf(loc, ln, "%s", l[at].loc);
    return l[at].idx;
}

static int rtl_open(char *tuner, size_t n, uint64_t *lo, uint64_t *hi) {
    rtlsdr_dev_t *d;
    char          sn[64], loc[32], wsn[64], wloc[32];
    int           cnt = (int)rtlsdr_get_device_count(), at = sdr_rtl_index < cnt ? sdr_rtl_index : sdr_rtl_exact ? -1 : 0;
    int           idx;

    pthread_mutex_lock(&want_lock);
    snprintf(wsn, sizeof(wsn), "%s", want_sn);
    snprintf(wloc, sizeof(wloc), "%s", want_loc);
    pthread_mutex_unlock(&want_lock);
    if (wsn[0]) {                       /* by its serial: that stick or none */
        rtl_sn_t l[16];
        int      k = rtl_list(l, 16);

        if ((at = rtl_by_sn(l, k, wsn, wloc)) < 0) return -1;
        cnt = k;
    }
    idx = rtl_at(at, sn, sizeof(sn), loc, sizeof(loc));
    if (at < 0) return -1;              /* -W and that RTL not on USB: none in its place */

    if (idx < 0 || rtlsdr_open(&d, (uint32_t)idx) != 0) return 1;  /* another program has it: tried again, not forced */
    pthread_mutex_lock(&want_lock);
    sdr_rtl_open_at = at;
    snprintf(sdr_rtl_open_loc, sizeof(sdr_rtl_open_loc), "%s", loc);
    snprintf(open_sn, sizeof(open_sn), "%s", sn);
    pthread_mutex_unlock(&want_lock);
    fprintf(stderr, "rtlspec: RTL%d of %d (serial %s, USB %s)%s\n", at, cnt, sn, loc[0] ? loc : "?",
            wsn[0] ? ", asked for by its serial" : at != sdr_rtl_index ? ": the one asked for is not on USB" : "");
    rtlsdr_set_bias_tee(d, 0);
    gain_tenths = INT_MIN;
    snprintf(tuner, n, "%s", tuner_name(rtlsdr_get_tuner_type(d)));
    *lo = 24000000;
    *hi = 1766000000;
    dev = d;
    return 0;
}

static int rtl_rate(uint32_t want, uint32_t *got) {
    if (rtlsdr_set_sample_rate(dev, want) != 0) return -1;
    *got = rtlsdr_get_sample_rate(dev);
    return 0;
}

static void rtl_gain(int tenths);

/* rtl-sdr-blog tunes an R820T below 24 MHz by direct sampling, by itself, its tuner off; tuned back above, the tuner
 * is initialised again (_rtlsdr_set_direct_sampling: tuner->init), its LNA and mixer back on their AGC, and the gain
 * set before is gone while librtlsdr still reports it. The daemon tunes an RTL from 24 MHz up (rtl_open), but the
 * first tune after the open is such a return: the sample rate's bandwidth step (r820t_set_bw) tunes to the frequency
 * the stick has, 0, so direct sampling is on when the first real tune comes. The daemon sets the rate, the gain, then
 * the frequency, and the gain again only when it changes: the gain a stick was opened with never took until it was
 * changed (B's from the GUI's -g, at every start, replug and stick switch; AIRSPACE's top gain when it had the stick
 * then). The bench X6200, 2026-11-22, an R820T at 145 MHz: the old daemon's rows at -43.4 dB with 0 dB asked (the
 * tuner's own level, 49.6 dB read -43.3), -74.1 once 0 dB was asked again; fixed, -74.1 from the start. So the last
 * gain is set again whenever a tune brings the tuner back */
static int rtl_freq(uint64_t hz) {
    if (hz > UINT32_MAX) return -1;
    int ds = rtlsdr_get_direct_sampling(dev);
    int r  = rtlsdr_set_center_freq(dev, (uint32_t)hz);

    if (ds > 0 && rtlsdr_get_direct_sampling(dev) == 0 && gain_tenths != INT_MIN) rtl_gain(gain_tenths);
    return r;
}

/* The nearest the tuner has; -1 its AGC, 999 its top */
static void rtl_gain(int tenths) {
    gain_tenths = tenths;
    if (tenths < 0) {
        rtlsdr_set_tuner_gain_mode(dev, 0);
        return;
    }
    int gains[64], n = rtlsdr_get_tuner_gains(dev, NULL), best = 0;

    if (n <= 0 || n > 64) return;
    rtlsdr_get_tuner_gains(dev, gains);
    for (int i = 1; i < n; i++)
        if (abs(gains[i] - tenths) < abs(gains[best] - tenths)) best = i;
    rtlsdr_set_tuner_gain_mode(dev, 1);
    rtlsdr_set_tuner_gain(dev, gains[best]);
}

static void on_read(unsigned char *buf, uint32_t n, void *ctx) {
    (void)ctx;
    if (*rx_halt) {
        rtlsdr_cancel_async(dev);
        return;
    }
    rx_cb(buf, n);
}

/* Several transfers queued all the time (as rtl_sdr and dump1090 read): one at a time, what the stick sent between
 * two of them was lost, a quarter of it whatever the rate */
static int rtl_rx(sdr_rx_cb cb, volatile int *halt) {
    rx_cb   = cb;
    rx_halt = halt;
    if (*halt) return 0;
    rtlsdr_reset_buffer(dev);
    rtlsdr_read_async(dev, on_read, NULL, 12, READ_LEN);
    return *halt ? 0 : -1;
}

/* As rtl_sdr's handler: the transfers cancelled, so a stream that has stalled (no callback to see the halt) still ends
 * cleanly; the radio's USB host (MUSB) is left broken by a stick killed with transfers in flight */
static void rtl_rx_stop(void) {
    if (dev) rtlsdr_cancel_async(dev);
}

static void rtl_close(void) {
    rtlsdr_dev_t *d = dev;

    dev                 = NULL;
    pthread_mutex_lock(&want_lock);
    sdr_rtl_open_at     = -1;
    sdr_rtl_open_loc[0] = 0;
    open_sn[0]          = 0;
    pthread_mutex_unlock(&want_lock);
    if (d) rtlsdr_close(d);
}

const sdr_ops_t sdr_rtl = {
    .kind    = "RTL",
    .present = rtl_present,
    .open    = rtl_open,
    .rate    = rtl_rate,
    .freq    = rtl_freq,
    .gain    = rtl_gain,
    .rx      = rtl_rx,
    .rx_stop = rtl_rx_stop,
    .close   = rtl_close,
};
