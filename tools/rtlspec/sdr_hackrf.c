/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a HackRF (One, and its kin the rad1o and Jawbreaker) for x6x00-rtlspec (sdr.h), through libhackrf
 *  (linked in whole: tools/rtlsdr/build.sh, buildroot/br2_external/package/libhackrf).
 *
 *  Receiving: its signed 8-bit I/Q made the RTL's unsigned (sdr_s8_to_u8), the DC spike its zero-IF receiver puts in
 *  the middle taken out there. Its amplifier is never on to receive (a strong signal near the radio would overload
 *  it); the RTL's gain scale (0-49.6 dB) is spread over its LNA (0-40 dB in 8) and baseband VGA (0-62 dB in 2).
 *  1 MHz to 6 GHz; its 2 MS/s and up, with the baseband filter the narrowest over 3/4 of the rate.
 *
 *  Transmitting (rtlspec.c's FM, only where it allows): the samples as they come, its TX gain (0-47 dB) and amplifier
 *  as asked, and an underrun limit, so the HackRF stops by itself a tenth of a second after the samples stop coming
 *  (this program gone, the USB host hung). Gain and amplifier back to 0 and off after.
 */

#include "sdr.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <hackrf.h>

static hackrf_device *volatile hd;
static bool                    inited;
static sdr_dc_t                dc;
static sdr_rx_cb               rx_cb;
static volatile int           *rx_halt;
static sdr_tx_fill             tx_fill;
static volatile int            tx_end;

static int hk_present(void) {
    return sdr_usb_has("1d50", "6089") || sdr_usb_has("1d50", "cc15") || sdr_usb_has("1d50", "604b");
}

static int hk_open(char *tuner, size_t n, uint64_t *lo, uint64_t *hi) {
    hackrf_device *d;
    uint8_t        id = BOARD_ID_UNDETECTED;
    int            r;

    if (!inited) {
        if (hackrf_init() != HACKRF_SUCCESS) return -1;
        inited = true;
    }
    if ((r = hackrf_open(&d)) != HACKRF_SUCCESS) {
        fprintf(stderr, "rtlspec: the HackRF does not open: %s\n", hackrf_error_name(r));
        return r == HACKRF_ERROR_NOT_FOUND ? -1 : 1;
    }
    hackrf_board_id_read(d, &id);
    snprintf(tuner, n, "%s", id == BOARD_ID_RAD1O ? "RAD1O" : id == BOARD_ID_JAWBREAKER ? "JAWBREAKER" : "HACKRF-ONE");
    *lo = 1000000;
    *hi = 6000000000ULL;
    hackrf_set_amp_enable(d, 0);
    hackrf_set_antenna_enable(d, 0);    /* no bias tee: nothing powered up the antenna cable */
    memset(&dc, 0, sizeof(dc));
    hd = d;
    return 0;
}

/* The MAX2837's baseband filter: the widest no wider than the rate and 5%. 2.5 MHz at B's 2.646 MS/s (its picture
 * 2.2 MHz, the aliases kept off it) and at ADS-B's 2.4 MS/s, where libhackrf's own rounding down to under the rate
 * took 1.75 MHz: narrower than what dump1090 samples, the half-microsecond pulses blunted. 1.75 MHz at TX's 2.1168 */
static uint32_t hk_bw(uint32_t rate) { return hackrf_compute_baseband_filter_bw(rate + rate / 20); }

static int hk_rate(uint32_t want, uint32_t *got) {
    if (want < 2000000 || hackrf_set_sample_rate(hd, want) != HACKRF_SUCCESS) return -1;
    hackrf_set_baseband_filter_bandwidth(hd, hk_bw(want));
    *got = want;
    {   /* said when it changes (B's 2.646 MS/s, ADS-B's 2.4), as the bladeRF's: the filter is checkable */
        static uint32_t said_rate, said_bw;
        if (want != said_rate || hk_bw(want) != said_bw)
            fprintf(stderr, "rtlspec: the HackRF at %u S/s, its baseband filter %u Hz\n", want, hk_bw(want));
        said_rate = want;
        said_bw   = hk_bw(want);
    }
    return 0;
}

static int hk_freq(uint64_t hz) {
    return hackrf_set_freq(hd, hz) == HACKRF_SUCCESS ? 0 : -1;
}

/* The RTL's tenths (0-496) as a total over LNA and VGA, 0-102 dB: the LNA first to 32 (its noise figure), then the VGA
 * to 40, then both to their tops. 28 dB, B's own, is LNA 32 VGA 24. -1 (no AGC here): the same; 999 (ADS-B): LNA 32
 * VGA 48, as dump1090's own HackRF defaults */
static void hk_gain(int tenths) {
    int lna, vga, t;

    if (tenths == 999) {
        lna = 32;
        vga = 48;
    } else {
        t   = (tenths < 0 ? 280 : tenths > 496 ? 496 : tenths) * 102 / 496;
        lna = t >= 32 ? 32 : t / 8 * 8;
        t  -= lna;
        vga = t >= 40 ? 40 : t / 2 * 2;
        t  -= vga;
        if (t >= 8) {
            lna += 8;
            t -= 8;
        }
        vga += t / 2 * 2;
        if (vga > 62) vga = 62;
    }
    hackrf_set_lna_gain(hd, (uint32_t)lna);
    hackrf_set_vga_gain(hd, (uint32_t)vga);
}

static int on_rx(hackrf_transfer *t) {
    if (*rx_halt) return -1;
    sdr_s8_to_u8(&dc, t->buffer, (uint32_t)t->valid_length);
    rx_cb(t->buffer, (uint32_t)t->valid_length);
    return 0;
}

static int hk_rx(sdr_rx_cb cb, volatile int *halt) {
    bool asked;

    rx_cb   = cb;
    rx_halt = halt;
    if (*halt) return 0;
    if (hackrf_start_rx(hd, on_rx, NULL) != HACKRF_SUCCESS) return -1;
    while (!*halt && hackrf_is_streaming(hd) == HACKRF_TRUE) usleep(50000);
    asked = *halt;
    hackrf_stop_rx(hd);
    return asked ? 0 : -1;
}

static void hk_rx_stop(void) {
    /* the stream's loop looks at the halt every 50 ms */
}

static int on_tx(hackrf_transfer *t) {
    if (tx_end) {
        memset(t->buffer, 0, (size_t)t->buffer_length);
        t->valid_length = t->buffer_length;
        return -1;
    }
    if (tx_fill((int8_t *)t->buffer, (uint32_t)t->buffer_length / 2)) tx_end = 1;
    t->valid_length = t->buffer_length;
    return 0;
}

static int hk_tx(uint64_t hz, uint32_t rate, int gain_db, bool amp, sdr_tx_fill fill) {
    int ok = -1;

    gain_db = gain_db < 0 ? 0 : gain_db > 47 ? 47 : gain_db;
    tx_fill = fill;
    tx_end  = 0;
    if (hackrf_set_sample_rate(hd, rate) == HACKRF_SUCCESS &&
        hackrf_set_baseband_filter_bandwidth(hd, hk_bw(rate)) == HACKRF_SUCCESS &&
        hackrf_set_freq(hd, hz) == HACKRF_SUCCESS && hackrf_set_txvga_gain(hd, (uint32_t)gain_db) == HACKRF_SUCCESS &&
        hackrf_set_amp_enable(hd, amp ? 1 : 0) == HACKRF_SUCCESS) {
        if (hackrf_set_tx_underrun_limit(hd, rate / 10) != HACKRF_SUCCESS)
            fprintf(stderr, "rtlspec: this HackRF's firmware has no underrun limit (2023.01.1 and up have)\n");
        if (hackrf_start_tx(hd, on_tx, NULL) == HACKRF_SUCCESS) {
            ok = 0;
            while (!tx_end && hackrf_is_streaming(hd) == HACKRF_TRUE) usleep(10000);
            usleep(100000);             /* the last transfers, ramped down to nothing, out */
            hackrf_stop_tx(hd);
        }
    }
    hackrf_set_amp_enable(hd, 0);
    hackrf_set_txvga_gain(hd, 0);
    hackrf_set_tx_underrun_limit(hd, 0);
    return ok;
}

static void hk_tx_stop(void) {
    tx_end = 1;
}

static void hk_close(void) {
    hackrf_device *d = hd;

    hd = NULL;
    if (d) hackrf_close(d);
}

const sdr_ops_t sdr_hackrf = {
    .kind        = "HACKRF",
    .present     = hk_present,
    .open        = hk_open,
    .rate        = hk_rate,
    .freq        = hk_freq,
    .gain        = hk_gain,
    .rx          = hk_rx,
    .rx_stop     = hk_rx_stop,
    .tx          = hk_tx,
    .tx_stop     = hk_tx_stop,
    .tx_gain_max = 47,
    .close       = hk_close,
};
