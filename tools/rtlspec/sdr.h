/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - x6x00-rtlspec's receivers (rtlspec.c): one backend per kind of SDR behind one interface, so the
 *  spectrum, a channel's audio, SAME, RDS and AIRSPACE's dump1090 feed work the same on each. Every backend hands its
 *  samples on as an RTL-SDR stick sends them, unsigned 8-bit I/Q, so the pipeline after it never knows which one it
 *  has: an RTL-SDR (sdr_rtl.c, librtlsdr), a HackRF (sdr_hackrf.c, libhackrf: signed 8-bit I/Q) or a bladeRF
 *  (sdr_bladerf.c, libbladeRF opened at run time: 12-bit I/Q in 16). One device is open at a time; each backend keeps
 *  its own.
 *
 *  Those that transmit (HackRF, bladeRF) take a transmitter too: FM made by rtlspec.c, handed over as signed 8-bit
 *  I/Q at the rate asked. Nothing here decides where it may transmit: rtlspec.c refuses all but the amateur bands
 *  before it asks.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A transfer's samples, unsigned 8-bit I/Q pairs (n bytes), on the backend's own thread */
typedef void (*sdr_rx_cb)(unsigned char *buf, uint32_t n);

/* The transmitter's next pairs, signed 8-bit I/Q, written into iq; on the backend's own thread. Its return: 0 go on,
 * 1 the last (the transmission ends once it is out) */
typedef int (*sdr_tx_fill)(int8_t *iq, uint32_t pairs);

typedef struct {
    const char *kind;               /* the device's kind for the GUI: "RTL", "HACKRF", "BLADERF" */
    /* Plugged in: its USB IDs among the devices (sysfs: no USB traffic, nothing opened). 1 yes */
    int  (*present)(void);
    /* Opened: 0. 1: there but held by another program (tried again later); 2: open but not usable as it is (the
     * bladeRF's FPGA image missing: tuner says what); -1 failed. tuner: the model ("R820T", "HACKRF-ONE",
     * "BLADERF2"), no spaces; *lo and *hi: the tuning range it takes */
    int  (*open)(char *tuner, size_t n, uint64_t *lo, uint64_t *hi);
    int  (*rate)(uint32_t want, uint32_t *got);         /* 0 set */
    int  (*freq)(uint64_t hz);                          /* 0 set */
    void (*gain)(int tenths);       /* tenths of dB on the RTL's scale (0-496); -1 its automatic; 999 its top (ADS-B) */
    /* Streams into cb until *halt is set (0) or the device stops sending by itself (-1). Blocks. *halt is the
     * caller's, set from any thread (a signal handler too) and then rx_stop called: what it sets before the stream
     * starts is not lost */
    int  (*rx)(sdr_rx_cb cb, volatile int *halt);
    void (*rx_stop)(void);          /* any thread, a signal handler too: wakes a stream that waits on the device */
    /* NULL: receive only. Transmits at hz, rate, the transmitter's own gain (dB) and its amplifier, from fill, until
     * fill says the last or tx_stop; the receiver's settings are the caller's to put back after. 0: it went; -1 not */
    int  (*tx)(uint64_t hz, uint32_t rate, int gain_db, bool amp, sdr_tx_fill fill);
    void (*tx_stop)(void);
    int  tx_gain_max;               /* the transmitter's gain, 0 to this, dB */
    void (*close)(void);
} sdr_ops_t;

extern const sdr_ops_t sdr_rtl, sdr_hackrf, sdr_bladerf;

/* The test device (sdr_file.c, -T FILE): transmits into the file, receives nothing; never a radio */
extern const sdr_ops_t sdr_file;
extern const char     *sdr_file_path;

/* A device with these USB IDs (hex, "1d50", "6089") is plugged in */
bool sdr_usb_has(const char *vid, const char *pid);

/* Signed 8-bit I/Q to the RTL's unsigned, in place, its DC (the receiver's own offset, a spike in the middle of the
 * picture) taken out: the average of each component followed over the transfers, and taken off with the rounding
 * error carried on, so no fraction of it stays behind. state: the backend's own, zeroed when it opens */
typedef struct {
    int32_t dc_i, dc_q;             /* 1/256 of a step */
    int32_t err_i, err_q;
    bool    primed;
} sdr_dc_t;
void sdr_s8_to_u8(sdr_dc_t *st, uint8_t *buf, uint32_t n);
