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
 *  None found: it opens and says so (2, tuner the file it wants: "hostedxA4.rbf"), and is opened again once the file
 *  is there (bl_ready_again) or it is pulled out. An image its USB firmware is too old for (FPGA v0.16 wants v2.6):
 *  said so too ("FIRMWARE"), and tried again when another image is put in /mnt/bladerf; the one loaded stays in the
 *  board till it loses power, so it has to be unplugged and plugged in for an older one.
 *
 *  Its USB firmware (the FX3's, in its own SPI flash): a Nuand image put in /mnt/bladerf as bladeRF_fw_vX.Y.Z.img (the
 *  version in its name, as Nuand names them) newer than the board's is written to the board at its next open, once a
 *  file (fw_flash): the board opened with no FPGA image to look for (none loaded, the AD9361 left alone: less current
 *  from the HOST port, and an FPGA too new for the old firmware does not stop the open), the image written, the board
 *  reset so it starts the new firmware, then opened as ever. Nothing is written for an image as old as the board's or
 *  older. The FPGA image after it (v0.16 needs firmware v2.6) is the one in /mnt/bladerf.
 *
 *  Receiving: 8-bit I/Q from the FPGA itself (SC8 Q7, FPGA v0.15 on), its DC taken out as the HackRF's: 2 bytes a
 *  sample over USB as the RTL-SDR's and the HackRF's. 12-bit SC16 (4 bytes a sample, cut to 8 bits here) only for an
 *  FPGA without it: at 2.1 MS/s that was 8.5 MB/s, more than the radio's USB host carries (68% of the samples came,
 *  2026-11-16, and B's picture was a comb of spikes, the gaps' own). Manual gain in dB as the RTL's (its tenths, 0-49.6
 *  dB) and 10 dB more, within the board's own range (the RTL's 7.7 dB was -3 dB on the bladeRF, spread over its whole
 *  range); its AGC for -1. Transmitting: the FM's signed 8 bits made SC16, the gain
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

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
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
    __typeof__(&bladerf_fw_version)          fwver;
    __typeof__(&bladerf_flash_firmware)      flashfw;
    __typeof__(&bladerf_device_reset)        reset;
    __typeof__(&bladerf_get_power_source)    psrc;      /* bladeRF 2.0 only; NULL in a library without them */
    __typeof__(&bladerf_get_pmic_register)   pmic;
} L;

static struct bladerf *volatile bd;
static sdr_dc_t                 dc;
static volatile int             tx_end;
static int16_t                  sbuf[N_SAMP * 2];
static int8_t                   bbuf[N_SAMP * 2];

static int  lib_tried;
static long fpga_dir_at;                /* FPGA_DIR's state when the last open failed on versions (dir_stamp) */

/* What is in FPGA_DIR, as one number: its files' sizes and times added (another image put there changes it) */
static long dir_stamp(void) {
    DIR           *d = opendir(FPGA_DIR);
    struct dirent *e;
    long           t = 0;

    if (!d) return 0;
    while ((e = readdir(d))) {
        char        p[300];
        struct stat st;

        snprintf(p, sizeof(p), "%s/%s", FPGA_DIR, e->d_name);
        if (e->d_name[0] != '.' && stat(p, &st) == 0) t += (long)st.st_mtime + (long)st.st_size;
    }
    closedir(d);
    return t;
}

/* libbladeRF loaded, once (again after bl_ready_again saw it come): false when it is not on this image */
static bool lib(void) {
    if (L.so || lib_tried) return L.so != NULL;
    lib_tried = 1;
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
    SYM(fwver, "bladerf_fw_version");
    SYM(flashfw, "bladerf_flash_firmware");
    SYM(reset, "bladerf_device_reset");
#undef SYM
    *(void **)&L.psrc = dlsym(L.so, "bladerf_get_power_source");
    *(void **)&L.pmic = dlsym(L.so, "bladerf_get_pmic_register");
    L.verbosity(BLADERF_LOG_LEVEL_WARNING);
    return true;
}

/* Nuand's IDs: 5250 the bladeRF 2.0 micro, 5246 the bladeRF x40/x115 (OpenMoko's 1d50:6066 on older ones) */
static int bl_present(void) {
    return sdr_usb_has("2cf0", "5250") || sdr_usb_has("2cf0", "5246") || sdr_usb_has("1d50", "6066");
}

/* The newest bladeRF_fw_vX.Y.Z.img in FPGA_DIR: its path and version (major*1e6 + minor*1e3 + patch), 0 none */
static long fw_file(char *path, size_t n) {
    DIR           *d = opendir(FPGA_DIR);
    struct dirent *e;
    long           best = 0;
    unsigned       a, b, c;

    if (!d) return 0;
    while ((e = readdir(d))) {
        int  used = 0;

        if (sscanf(e->d_name, "bladeRF_fw_v%u.%u.%u.img%n", &a, &b, &c, &used) == 3 && used &&
            !e->d_name[used] && a < 100 && b < 1000 && c < 1000 && (long)a * 1000000 + b * 1000 + c > best) {
            best = (long)a * 1000000 + b * 1000 + c;
            snprintf(path, n, "%s/%s", FPGA_DIR, e->d_name);
        }
    }
    closedir(d);
    return best;
}

/* A firmware image newer than the board's written to it (the comment at the top). 1: written, the board resetting
 * (opened again in a moment: the caller says busy); 0 nothing to do; each image's file tried once (fw_done: its path,
 * size and time), so one that fails is not written again and again */
static int fw_flash(char *tuner, size_t n) {
    static char             done[340];
    char                    path[300], key[340];
    struct stat             st;
    struct bladerf         *d;
    struct bladerf_version  v;
    long                    want = fw_file(path, sizeof(path)), have;
    int                     e;

    if (!want || stat(path, &st) != 0) return 0;
    snprintf(key, sizeof(key), "%s %ld %ld", path, (long)st.st_size, (long)st.st_mtime);
    if (!strcmp(key, done)) return 0;
    snprintf(done, sizeof(done), "%s", key);
    setenv("BLADERF_SEARCH_DIR", "/nonexistent", 1);   /* no FPGA image looked for: none loaded for this */
    e = L.fopen_(&d, NULL);
    setenv("BLADERF_SEARCH_DIR", FPGA_DIR, 1);
    if (e != 0) {
        fprintf(stderr, "rtlspec: the bladeRF does not open for its firmware: %s\n", L.strerror_(e));
        return 0;
    }
    if (L.fwver(d, &v) != 0) {
        L.close_(d);
        return 0;
    }
    have = (long)v.major * 1000000 + v.minor * 1000 + v.patch;
    if (have >= want) {
        fprintf(stderr, "rtlspec: the bladeRF's firmware is v%u.%u.%u: %s not needed\n", v.major, v.minor, v.patch,
                path);
        L.close_(d);
        return 0;
    }
    fprintf(stderr, "rtlspec: the bladeRF's firmware v%u.%u.%u -> %s: writing\n", v.major, v.minor, v.patch, path);
    if ((e = L.flashfw(d, path)) != 0) {
        fprintf(stderr, "rtlspec: the bladeRF's firmware not written: %s\n", L.strerror_(e));
        L.close_(d);
        return 0;
    }
    fprintf(stderr, "rtlspec: the bladeRF's firmware written; the board starts it now\n");
    snprintf(tuner, n, "FIRMWARE-WRITTEN");
    L.reset(d);                         /* it reloads its firmware from flash and comes back on USB */
    L.close_(d);
    return 1;
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

/* Which power the bladeRF 2.0 runs on (its TPS2115A: the DC jack or USB's VBUS) and what it draws (its INA219), said
 * at each open: a board that dropped off USB as it started was put down to bus power (2026-11-15), and on 2026-11-18
 * it did so with its own supply plugged in, as the owner said. This says which it was */
static void bl_power_say(struct bladerf *d, const char *when) {
    bladerf_power_sources ps;
    float                 v = 0, a = 0;

    if (!L.psrc || strcmp(L.board(d), "bladerf2") || L.psrc(d, &ps) != 0) return;
    if (!L.pmic || L.pmic(d, BLADERF_PMIC_VOLTAGE_BUS, &v) != 0 || L.pmic(d, BLADERF_PMIC_CURRENT, &a) != 0) v = a = 0;
    fprintf(stderr, "rtlspec: the bladeRF %s on %s: %.2f V, %.2f A\n", when,
            ps == BLADERF_PS_DC ? "its DC jack" : ps == BLADERF_PS_USB_VBUS ? "USB power (VBUS)" : "an unknown supply",
            (double)v, (double)a);
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
    if (fw_flash(tuner, n)) return 1;               /* a newer firmware written: opened again once it is back */
    setenv("BLADERF_SEARCH_DIR", FPGA_DIR, 0);      /* the FPGA image looked for there first, when the board has none */
    if ((e = L.fopen_(&d, NULL)) != 0) {
        fprintf(stderr, "rtlspec: the bladeRF does not open: %s\n", L.strerror_(e));
        if (e == BLADERF_ERR_UPDATE_FW || e == BLADERF_ERR_UPDATE_FPGA) {
            /* its FPGA image and its USB firmware do not go together (FPGA v0.16 wants firmware v2.6): said so, and
             * left till it is unplugged or another image is put in place (an FPGA loaded stays till it loses power) */
            snprintf(tuner, n, "%s", e == BLADERF_ERR_UPDATE_FW ? "FIRMWARE" : "FPGA-UPDATE");
            fpga_dir_at = dir_stamp();
            return 2;
        }
        if (e == BLADERF_ERR_NODEV) return -1;
        /* It went while it started (a bladeRF 2.0 off USB as its AD9361 started: 2026-11-15 on the HOST port's power,
         * 2026-11-18 with its own supply plugged in, the owner said; the errors are "unexpected", "no devices" after
         * it): gone, not busy. Still there after a failed start: said so, left till it is pulled out (up to v0.3.12.25
         * both said BUSY, as if another program had it). bl_power_say tells which power it opens on */
        if (!bl_present()) {
            fprintf(stderr, "rtlspec: the bladeRF dropped off USB as it started\n");
            return -1;
        }
        if (e == BLADERF_ERR_IO || e == BLADERF_ERR_PERMISSION) return 1;     /* held by another program */
        snprintf(tuner, n, "STARTFAIL");
        return 2;
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
    bl_power_say(d, "open");
    return 0;
}

static int bl_rate(uint32_t want, uint32_t *got) {
    bladerf_sample_rate a = 0;
    bladerf_bandwidth   b;

    if (L.rate(bd, BLADERF_CHANNEL_RX(0), want, &a) != 0) return -1;
    /* the AD9361's analog bandwidth (both sides): 9/10 of the rate, over B's picture (5/6 of it; 3/4 cut its edges)
     * and most of ADS-B's 2.4 MS/s; its own half-band filters keep the aliases off */
    L.bw(bd, BLADERF_CHANNEL_RX(0), want / 10 * 9, &b);
    *got = a;
    {   /* what the board took, said when it changes (B's 2.646 MS/s, ADS-B's 2.4): the filter is checkable */
        static bladerf_sample_rate said_a;
        static bladerf_bandwidth   said_b;
        if (a != said_a || b != said_b)
            fprintf(stderr, "rtlspec: the bladeRF at %u S/s, its analog filter %u Hz\n", (unsigned)a, (unsigned)b);
        said_a = a;
        said_b = b;
    }
    return 0;
}

static int bl_freq(uint64_t hz) {
    return L.freq(bd, BLADERF_CHANNEL_RX(0), hz) == 0 ? 0 : -1;
}

/* The RTL's tenths (0-496), in dB as there and 10 dB more, within the board's own range; -1 its AGC; 999 three
 * quarters up (ADS-B) */
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
    else g = (int64_t)((tenths > 496 ? 496 : tenths) / 10 + 10);
    if (g < lo) g = (int64_t)lo;
    if (g > hi) g = (int64_t)hi;
    L.gain(bd, BLADERF_CHANNEL_RX(0), (bladerf_gain)g);
}

static int bl_rx(sdr_rx_cb cb, volatile int *halt) {
    static int said = -1;
    int        e = 0;
    bool       sc8;

    if (*halt) return 0;
    sc8 = L.sync_config(bd, BLADERF_RX_X1, BLADERF_FORMAT_SC8_Q7, 16, 8192, 8, 3500) == 0;
    if (!sc8 && L.sync_config(bd, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, 8192, 8, 3500) != 0) return -1;
    if (L.enable(bd, BLADERF_CHANNEL_RX(0), true) != 0) return -1;
    if (said != sc8) {
        fprintf(stderr, "rtlspec: the bladeRF sends %s\n", sc8 ? "8-bit samples (SC8 Q7)"
                                                                : "12-bit samples (SC16 Q11: its FPGA has no 8-bit format)");
        said = sc8;
    }
    for (int nb = 0; !*halt; nb++) {
        if ((e = L.sync_rx(bd, sc8 ? (void *)bbuf : (void *)sbuf, N_SAMP, NULL, 2000)) != 0) {
            fprintf(stderr, "rtlspec: the bladeRF's stream ended: %s\n", L.strerror_(e));
            break;
        }
        if (nb == 50) bl_power_say(bd, "receiving");      /* its receiver's current in it too */
        for (int k = 0; !sc8 && k < N_SAMP * 2; k++) {     /* +-2048 to +-128 */
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

/* What the last open wanted is there now: libbladeRF on the image, or the FPGA image put where it is looked for */
static int bl_ready_again(const char *tuner) {
    char path[160];

    if (!strcmp(tuner, "FIRMWARE") || !strcmp(tuner, "FPGA-UPDATE")) return dir_stamp() != fpga_dir_at;
    if (!strcmp(tuner, "LIBBLADERF")) {
        if (access("/usr/lib/libbladeRF.so.2", F_OK) != 0) return 0;
        lib_tried = 0;
        return 1;
    }
    snprintf(path, sizeof(path), "%s/%s", FPGA_DIR, tuner);
    if (access(path, R_OK) == 0) return 1;
    snprintf(path, sizeof(path), "/usr/share/Nuand/bladeRF/%s", tuner);
    return access(path, R_OK) == 0;
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
    .ready_again = bl_ready_again,
};

#endif
