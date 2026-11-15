/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - a bladeRF (2.0 micro xA4/xA5/xA9; x40/x115) for x6x00-rtlspec (sdr.h), through libbladeRF, opened at
 *  run time (dlopen): the daemon runs without it, and a bladeRF says what is missing (LIBBLADERF) rather than the
 *  daemon not starting at all on an image that lacks it. Built against libbladeRF.h (-DSDR_BLADERF); without it this
 *  is a backend that never finds one.
 *
 *  Its FPGA image: loaded by libbladeRF when the board has none (a board set to load its own from flash, bladeRF-cli
 *  -L, needs nothing), found in /mnt/bladerf (the DATA partition: BLADERF_SEARCH_DIR) or /usr/share/Nuand/bladeRF.
 *  None found: it opens and says so (2, tuner the file it wants: "hostedxA4.rbf"), and is left so till pulled out.
 *
 *  Receiving: 12-bit I/Q (SC16 Q11, +-2048) cut to the RTL's 8 bits (one in 16), its DC taken out as the HackRF's;
 *  manual gain over the board's own range, its AGC for -1. Transmitting: the FM's signed 8 bits made SC16, the gain
 *  in dB over the board's lowest (0 is its least), no amplifier to switch.
 */

#include "sdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDR_BLADERF

static int  bl_none(void) { return 0; }
static int  bl_open(char *tuner, size_t n, uint64_t *lo, uint64_t *hi) { (void)tuner, (void)n, (void)lo, (void)hi; return -1; }
const sdr_ops_t sdr_bladerf = {.kind = "BLADERF", .present = bl_none, .open = bl_open};

#else

#include <dlfcn.h>
#include <unistd.h>

#include <libbladeRF.h>

#define FPGA_DIR "/mnt/bladerf"         /* the DATA partition: where an FPGA image is put by hand */
#define N_SAMP   16384                  /* a transfer's pairs */

static struct {
    void *so;
    __typeof__(&bladerf_open)                fopen_;
    __typeof__(&bladerf_close)               close_;
    __typeof__(&bladerf_get_board_name)      board;
    __typeof__(&bladerf_is_fpga_configured)  fpga_ok;
    __typeof__(&bladerf_get_fpga_size)       fpga_size;
    __typeof__(&bladerf_set_sample_rate)     rate;
    __typeof__(&bladerf_set_bandwidth)       bw;
    __typeof__(&bladerf_set_frequency)       freq;
    __typeof__(&bladerf_get_frequency_range) frange;
    __typeof__(&bladerf_set_gain_mode)       gmode;
    __typeof__(&bladerf_set_gain)            gain;
    __typeof__(&bladerf_get_gain_range)      grange;
    __typeof__(&bladerf_sync_config)         sync_config;
    __typeof__(&bladerf_enable_module)       enable;
    __typeof__(&bladerf_sync_rx)             sync_rx;
    __typeof__(&bladerf_sync_tx)             sync_tx;
    __typeof__(&bladerf_strerror)            strerror_;
    __typeof__(&bladerf_log_set_verbosity)   verbosity;
} L;

static struct bladerf *volatile bd;
static sdr_dc_t                 dc;
static volatile int             tx_end;
static int16_t                  sbuf[N_SAMP * 2];
static int8_t                   bbuf[N_SAMP * 2];

/* libbladeRF loaded, once: false when it is not on this image */
static bool lib(void) {
    static int tried;

    if (L.so || tried) return L.so != NULL;
    tried = 1;
    if (!(L.so = dlopen("libbladeRF.so.2", RTLD_NOW)) && !(L.so = dlopen("libbladeRF.so", RTLD_NOW))) {
        fprintf(stderr, "rtlspec: a bladeRF, and no libbladeRF on this image: %s\n", dlerror());
        return false;
    }
#define SYM(f, n) if (!(*(void **)&L.f = dlsym(L.so, n))) { fprintf(stderr, "rtlspec: libbladeRF has no %s\n", n); \
                                                             dlclose(L.so); L.so = NULL; return false; }
    SYM(fopen_, "bladerf_open");
    SYM(close_, "bladerf_close");
    SYM(board, "bladerf_get_board_name");
    SYM(fpga_ok, "bladerf_is_fpga_configured");
    SYM(fpga_size, "bladerf_get_fpga_size");
    SYM(rate, "bladerf_set_sample_rate");
    SYM(bw, "bladerf_set_bandwidth");
    SYM(freq, "bladerf_set_frequency");
    SYM(frange, "bladerf_get_frequency_range");
    SYM(gmode, "bladerf_set_gain_mode");
    SYM(gain, "bladerf_set_gain");
    SYM(grange, "bladerf_get_gain_range");
    SYM(sync_config, "bladerf_sync_config");
    SYM(enable, "bladerf_enable_module");
    SYM(sync_rx, "bladerf_sync_rx");
    SYM(sync_tx, "bladerf_sync_tx");
    SYM(strerror_, "bladerf_strerror");
    SYM(verbosity, "bladerf_log_set_verbosity");
#undef SYM
    L.verbosity(BLADERF_LOG_LEVEL_WARNING);
    return true;
}

/* 2.0 micro (Nuand's own ID), x40/x115 (OpenMoko's) */
static int bl_present(void) {
    return sdr_usb_has("2cf0", "5246") || sdr_usb_has("1d50", "6066");
}

static const char *fpga_file(bladerf_fpga_size s) {
    switch (s) {
        case BLADERF_FPGA_A4: return "hostedxA4.rbf";
        case BLADERF_FPGA_A5: return "hostedxA5.rbf";
        case BLADERF_FPGA_A9: return "hostedxA9.rbf";
        case BLADERF_FPGA_40KLE: return "hostedx40.rbf";
        case BLADERF_FPGA_115KLE: return "hostedx115.rbf";
        default: return "FPGA";
    }
}

static int bl_open(char *tuner, size_t n, uint64_t *lo, uint64_t *hi) {
    struct bladerf             *d;
    const struct bladerf_range *r = NULL;
    bladerf_fpga_size           sz = BLADERF_FPGA_UNKNOWN;
    int                         e;

    if (!lib()) {
        snprintf(tuner, n, "LIBBLADERF");
        return 2;
    }
    setenv("BLADERF_SEARCH_DIR", FPGA_DIR, 0);      /* the FPGA image looked for there first, when the board has none */
    if ((e = L.fopen_(&d, NULL)) != 0) {
        fprintf(stderr, "rtlspec: the bladeRF does not open: %s\n", L.strerror_(e));
        return e == BLADERF_ERR_NODEV ? -1 : 1;
    }
    bd = d;
    if (L.fpga_ok(d) != 1) {
        L.fpga_size(d, &sz);
        snprintf(tuner, n, "%s", fpga_file(sz));
        fprintf(stderr, "rtlspec: the bladeRF has no FPGA image: %s wanted in %s or /usr/share/Nuand/bladeRF\n",
                 fpga_file(sz), FPGA_DIR);
        return 2;
    }
    snprintf(tuner, n, "%s", !strcmp(L.board(d), "bladerf2") ? "BLADERF2" : "BLADERF1");
    *lo = 47000000;
    *hi = 6000000000ULL;
    if (L.frange(d, BLADERF_CHANNEL_RX(0), &r) == 0 && r) {
        *lo = (uint64_t)(r->min * r->scale);
        *hi = (uint64_t)(r->max * r->scale);
        if (*lo < 1000000) *lo = 1000000;  /* no HF to speak of: the picture's maths are not made for 0 Hz */
    }
    L.gmode(d, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
    memset(&dc, 0, sizeof(dc));
    return 0;
}

static int bl_rate(uint32_t want, uint32_t *got) {
    bladerf_sample_rate a = 0;
    bladerf_bandwidth   b;

    if (L.rate(bd, BLADERF_CHANNEL_RX(0), want, &a) != 0) return -1;
    L.bw(bd, BLADERF_CHANNEL_RX(0), want * 3 / 4, &b);
    *got = a;
    return 0;
}

static int bl_freq(uint64_t hz) {
    return L.freq(bd, BLADERF_CHANNEL_RX(0), hz) == 0 ? 0 : -1;
}

/* The RTL's tenths (0-496) over the board's own range; -1 its AGC; 999 three quarters up (ADS-B) */
static void bl_gain(int tenths) {
    const struct bladerf_range *r = NULL;
    int64_t                     g;

    if (tenths < 0) {
        L.gmode(bd, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_DEFAULT);
        return;
    }
    L.gmode(bd, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
    if (L.grange(bd, BLADERF_CHANNEL_RX(0), &r) != 0 || !r) return;
    double lo = r->min * r->scale, hi = r->max * r->scale;

    if (tenths == 999) g = (int64_t)(lo + (hi - lo) * 3 / 4);
    else g = (int64_t)(lo + (hi - lo) * (tenths > 496 ? 496 : tenths) / 496);
    L.gain(bd, BLADERF_CHANNEL_RX(0), (bladerf_gain)g);
}

static int bl_rx(sdr_rx_cb cb, volatile int *halt) {
    int e = 0;

    if (*halt) return 0;
    if (L.sync_config(bd, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, 8192, 8, 3500) != 0 ||
        L.enable(bd, BLADERF_CHANNEL_RX(0), true) != 0)
        return -1;
    while (!*halt) {
        if ((e = L.sync_rx(bd, sbuf, N_SAMP, NULL, 2000)) != 0) {
            fprintf(stderr, "rtlspec: the bladeRF's stream ended: %s\n", L.strerror_(e));
            break;
        }
        for (int k = 0; k < N_SAMP * 2; k++) {    /* +-2048 to +-128 */
            int v = sbuf[k] / 16;
            bbuf[k] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
        }
        sdr_s8_to_u8(&dc, (uint8_t *)bbuf, N_SAMP * 2);
        cb((unsigned char *)bbuf, N_SAMP * 2);
    }
    L.enable(bd, BLADERF_CHANNEL_RX(0), false);
    return *halt ? 0 : -1;
}

static void bl_rx_stop(void) {
    /* the stream looks at the halt after each transfer (a few ms) */
}

static int bl_tx(uint64_t hz, uint32_t rate, int gain_db, bool amp, sdr_tx_fill fill) {
    const struct bladerf_range *r = NULL;
    bladerf_sample_rate         a;
    bladerf_bandwidth           b;
    int                         ok = -1, last = 0;
    (void)amp;

    tx_end = 0;
    if (L.grange(bd, BLADERF_CHANNEL_TX(0), &r) != 0 || !r) return -1;
    int64_t lo = (int64_t)(r->min * r->scale), hi = (int64_t)(r->max * r->scale);
    int64_t g  = lo + (gain_db < 0 ? 0 : gain_db);

    if (g > hi) g = hi;
    L.gmode(bd, BLADERF_CHANNEL_TX(0), BLADERF_GAIN_MGC);     /* not on every board: its result left */
    if (L.rate(bd, BLADERF_CHANNEL_TX(0), rate, &a) == 0 && L.bw(bd, BLADERF_CHANNEL_TX(0), 1500000, &b) == 0 &&
        L.freq(bd, BLADERF_CHANNEL_TX(0), hz) == 0 && L.gain(bd, BLADERF_CHANNEL_TX(0), (bladerf_gain)g) == 0 &&
        L.sync_config(bd, BLADERF_TX_X1, BLADERF_FORMAT_SC16_Q11, 16, 8192, 8, 3500) == 0 &&
        L.enable(bd, BLADERF_CHANNEL_TX(0), true) == 0) {
        ok = 0;
        while (!tx_end && !last) {
            last = fill(bbuf, N_SAMP);
            for (int k = 0; k < N_SAMP * 2; k++) sbuf[k] = (int16_t)(bbuf[k] * 16);
            if (L.sync_tx(bd, sbuf, N_SAMP, NULL, 1000) != 0) break;
        }
        memset(sbuf, 0, sizeof(sbuf));  /* silence after it, so the DAC is not left on the last sample */
        L.sync_tx(bd, sbuf, N_SAMP, NULL, 1000);
        usleep(100000);
        L.enable(bd, BLADERF_CHANNEL_TX(0), false);
    }
    L.gain(bd, BLADERF_CHANNEL_TX(0), (bladerf_gain)lo);
    return ok;
}

static void bl_tx_stop(void) {
    tx_end = 1;
}

static void bl_close(void) {
    struct bladerf *d = bd;

    bd = NULL;
    if (d) L.close_(d);
}

const sdr_ops_t sdr_bladerf = {
    .kind        = "BLADERF",
    .present     = bl_present,
    .open        = bl_open,
    .rate        = bl_rate,
    .freq        = bl_freq,
    .gain        = bl_gain,
    .rx          = bl_rx,
    .rx_stop     = bl_rx_stop,
    .tx          = bl_tx,
    .tx_stop     = bl_tx_stop,
    .tx_gain_max = 60,
    .close       = bl_close,
};

#endif
