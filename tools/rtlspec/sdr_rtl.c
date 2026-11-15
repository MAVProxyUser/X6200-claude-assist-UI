/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - an RTL-SDR stick for x6x00-rtlspec (sdr.h), through librtlsdr (the RTL-SDR Blog's, patched so a stick
 *  pulled out mid-stream has all its transfers back before they are freed: buildroot/br2_external/package/rtlsdr-blog).
 *  Its samples are the pipeline's own, unsigned 8-bit I/Q. Receive only.
 */

#include "sdr.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <rtl-sdr.h>

#define READ_LEN (16384 * 2)

int sdr_rtl_index;                      /* -d: which stick */

static rtlsdr_dev_t *volatile dev;
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
    return (int)rtlsdr_get_device_count() > sdr_rtl_index;
}

static int rtl_open(char *tuner, size_t n, uint64_t *lo, uint64_t *hi) {
    rtlsdr_dev_t *d;

    if (rtlsdr_open(&d, (uint32_t)sdr_rtl_index) != 0) return 1;     /* another program has it: tried again, not forced */
    rtlsdr_set_bias_tee(d, 0);
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

static int rtl_freq(uint64_t hz) {
    return hz > UINT32_MAX ? -1 : rtlsdr_set_center_freq(dev, (uint32_t)hz);
}

/* The nearest the tuner has; -1 its AGC, 999 its top */
static void rtl_gain(int tenths) {
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

    dev = NULL;
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
