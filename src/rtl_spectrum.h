/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An SDR's spectrum for the Sentry's spectrum, LOG and WALKIE screens (rtl_spectrum.c): an RTL-SDR stick,
 *  a HackRF or a bladeRF on the HOST port, "the stick" below whichever it is. It is held by /usr/bin/x6x00-rtlspec
 *  (tools/rtlspec), a daemon that opens it once and keeps it streaming for as long as the radio runs; this side
 *  connects to it, asks for rows while they are wanted and keeps the newest one. A HackRF or a bladeRF also
 *  transmits, through the same daemon (rtl_spectrum_tx), only where it allows: the amateur bands from 2 m up.
 *  A GUI started again finds the same daemon and the same open stick: nothing closes it but pulling it out. While
 *  AIRSPACE's screen is open (rtl_spectrum_yield) no rows are asked for and its dump1090 is given the samples
 *  (rtl_spectrum_feed).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    RTL_SPEC_OFF,           /* not wanted */
    RTL_SPEC_NO_HELPER,     /* no /usr/bin/x6x00-rtlspec on this image, or it does not start */
    RTL_SPEC_NO_STICK,      /* no RTL-SDR on USB */
    RTL_SPEC_BUSY,          /* the stick, held by another program for now */
    RTL_SPEC_OPENING,       /* the stick opening, or its first row not here yet */
    RTL_SPEC_ON,            /* rows coming */
    RTL_SPEC_STALLED,       /* the stick open and sending nothing: it has to be pulled out and plugged in again */
    RTL_SPEC_TX,            /* transmitting (a HackRF, a bladeRF): no rows meanwhile */
    RTL_SPEC_NEEDS,         /* there and not usable as it is: a bladeRF with no FPGA image, or no libbladeRF
                             * (rtl_spectrum_tuner says which) */
} rtl_spec_state_t;

/* An RTL-SDR's tuning; the device's own: rtl_spectrum_min_hz, rtl_spectrum_max_hz */
#define RTL_SPEC_MIN_HZ  24000000LL
#define RTL_SPEC_MAX_HZ  1766000000LL
/* What any of them tunes, the daemon's own bounds: a frequency asked of it within these is kept there (each device
 * takes what it can of it) */
#define RTL_SPEC_ANY_MIN_HZ  1000000LL
#define RTL_SPEC_ANY_MAX_HZ  6000000000LL

/* The kind of SDR: the daemon's word, or before it (or with no daemon) the one found on USB */
typedef enum { RTL_DEV_NONE, RTL_DEV_RTL, RTL_DEV_HACKRF, RTL_DEV_BLADERF } rtl_dev_t;

rtl_dev_t rtl_spectrum_dev(void);

/* The SDR the daemon is to hold when it is plugged in (RTL_DEV_NONE: a HackRF, else a bladeRF, else an RTL-SDR): the
 * one open closed for it, said again to a daemon started anew. One that will not open leaves the one before */
void rtl_spectrum_use(rtl_dev_t d);
rtl_dev_t rtl_spectrum_used(void);

/* What a kind of SDR tunes, Hz (an RTL-SDR's R820T 24 MHz-1.766 GHz, a HackRF 1 MHz-6 GHz, a bladeRF 2 70 MHz-6 GHz):
 * for one not open, whether it could take a band */
void rtl_spectrum_kind_range(rtl_dev_t d, int64_t *lo, int64_t *hi);
/* Its name in a title bar's device tags and other short places: "RTL", "HACKRF", "BLADERF" ("RTL" for none) */
const char *rtl_spectrum_dev_tag(void);
/* Its name where the stick is named to the user: "RTL-SDR", "HACKRF", "BLADERF" ("RTL-SDR" for none) */
const char *rtl_spectrum_dev_name(void);
/* What it tunes, Hz: the device's own once the daemon has said (a HackRF 1 MHz-6 GHz, a bladeRF 2 70 MHz-6 GHz), an
 * RTL-SDR's before (RTL_SPEC_MIN_HZ, RTL_SPEC_MAX_HZ) */
int64_t rtl_spectrum_min_hz(void);
int64_t rtl_spectrum_max_hz(void);
/* Open and able to transmit (a HackRF, a bladeRF), transmitting or not */
bool rtl_spectrum_can_tx(void);

/* Wanted (on, at hz with gain tenths of dB, -1 the tuner's AGC) or not: rows asked of the daemon or not, which is
 * started (in the background, for good) the first time */
void rtl_spectrum_want(bool on, int64_t hz, int gain);

/* While on: another frequency (kept within what the device tunes), another gain */
void rtl_spectrum_tune(int64_t hz);
void rtl_spectrum_gain(int tenths);

rtl_spec_state_t rtl_spectrum_state(void);

/* AIRSPACE's screen open (on): no rows asked for, and AIRSPACE's dump1090 runs on the stick's samples; closed: rows
 * again. The stick stays open and streaming either way */
void rtl_spectrum_yield(bool on);

/* Connected to the daemon: the stick's samples can be had through it */
bool rtl_spectrum_daemon(void);

/* The stick's raw samples (unsigned 8-bit I/Q, 1090 MHz, 2.4 MS/s) written by the daemon into fd, a pipe's end it
 * takes over (the caller closes its own copy), for dump1090 reading the other end; -1: no more, the stick back to the
 * spectrum. False when the daemon is not connected */
bool rtl_spectrum_feed(int fd);

/* Listen to hz (mode 1 NFM, 2 AM, 3 WFM) on the speaker in place of the X6200's receiver; mode 0 or hz 0: back to
 * the X6200. Within the helper's span only: outside it is silent until the stick is retuned there */
void rtl_spectrum_listen(int64_t hz, int mode);
bool rtl_spectrum_listening(void);

/* While listening: the X6200's receiver mixed in with the stick (on: BOTH), or the stick alone. Mixed only on the
 * Bluetooth headphones: on the speaker the base, playing the stick, sends none of its receiver (rtl_spectrum.c) */
void rtl_spectrum_mux(bool on);
/* The stick's audio on the Bluetooth headphones now, not the speaker: BOTH mixes the X6200 in */
bool rtl_spectrum_on_headphones(void);
/* BOTH's monitor test now (/tmp/both_moni): the base in normal mode with its receiver, the stick to its line in */
bool rtl_spectrum_on_monitor(void);

/* OFF: the speaker taken from the X6200's receiver and given nothing (on), or let go */
void rtl_spectrum_mute(bool on);

/* The stick's audio as it is played, kept while on for a picture of it (a voiceprint); read up to n samples */
void rtl_spectrum_scope(bool on);
size_t rtl_spectrum_scope_read(int16_t *buf, size_t n);

/* The channel listened to over the noise of the stick's picture, dB (as the squelch measures it); -1 none */
float rtl_spectrum_level(void);

/* BOTH: each receiver's share of the mix (0-1), the louder at 1 and the base's speaker at its volume */
void rtl_spectrum_levels(float stick, float x6200);

/* The squelch while listening: open at db over the noise floor of the stick's picture (the listened channel's
 * strongest bin over the median), 0 always open. Open says whether it lets the audio through now */
void rtl_spectrum_squelch(int db);
bool rtl_spectrum_squelch_open(void);

/* The channel watched for SAME alerts (NOAA Weather Radio, narrow FM), 0 none: within the stick's span only */
void rtl_spectrum_watch(int64_t hz);

/* The last SAME header the watch decided ("ZCZC-WXR-TOR-039049+0030-3170815-KILN/NWS-", "NNNN" an end), when, and
 * a count that tells a new one: false when none newer than *seq. Each is also appended to /mnt/wx_alerts.log */
bool rtl_spectrum_alert(char *text, size_t n, time_t *at, uint32_t *seq);

/* RDS (the daemon's rds.c): that FM broadcast channel's station decoded, 0 none; said again to a daemon started anew */
void rtl_spectrum_rds_watch(int64_t hz);

typedef struct {
    int64_t  hz;                /* the channel it came from */
    uint16_t pi;                /* the station's code */
    int      pty;               /* its programme type, -1 none yet */
    bool     tp, ta;
    char     ps[9];             /* its name, "" until whole */
    char     rt[65];            /* its RadioText, "" until whole */
    uint32_t seq;               /* one more at each news */
} rtl_rds_t;

/* What came for the channel watched; false: nothing yet */
bool rtl_spectrum_rds(rtl_rds_t *out);

/* ACARS (the daemon's acars.c): the device at 130.8 MHz, the ACARS channels there decoded (AIRSPACE's ACARS view;
 * never with AIRSPACE's dump1090 on the same device: adsb_rx_lend first). Said again to a daemon started anew */
void rtl_spectrum_acars(bool on);
bool rtl_spectrum_acars_on(void);

typedef struct {
    int64_t hz;                 /* the channel */
    time_t  at;                 /* when it came */
    char    mode, ack, block;
    bool    more;               /* ETB: more blocks follow */
    char    reg[8], label[3], msn[5], flight[7];
    int     level;              /* dB, the audio's */
    char    text[221];
} rtl_acars_t;

#define RTL_ACARS_KEEP 64
/* The messages kept, newest first: i from 0, false past the last. rtl_spectrum_acars_seq: one more at each new one */
bool     rtl_spectrum_acars_msg(int i, rtl_acars_t *out);
uint32_t rtl_spectrum_acars_seq(void);

/* The tuner the helper found ("R820T", "HACKRF-ONE", "BLADERF2"; for RTL_SPEC_NEEDS what is missing: "LIBBLADERF",
 * "hostedxA4.rbf"), "" before */
void rtl_spectrum_tuner(char *s, size_t n);

/* ---- Transmitting: a HackRF or a bladeRF, narrow FM, the hand mic's voice ----
 *
 * The daemon refuses all but the amateur bands from 2 m to 23 cm whatever is asked here; the caller narrows that to
 * the TX ranges first (tx_ranges_sdr_check). On: the base in record mode (AUDIO_PLAY_VOICE_REC), so the hand mic's
 * audio comes into Linux as the voice messages record it, and sent to the daemon as it comes; off: the base's audio
 * back as it was. The daemon ends it by itself when no voice came for 2 s, after 3 minutes in one go, and when this GUI
 * goes. hz 0 ends it */
void rtl_spectrum_tx(int64_t hz, int gain_db, bool amp);

typedef enum { RTL_TX_OFF, RTL_TX_ASKED, RTL_TX_ON } rtl_tx_state_t;
rtl_tx_state_t rtl_spectrum_tx_state(void);
/* The daemon's last word on it ("ON", "END TIMEOUT", "NO NOT-AMATEUR"), and a count that tells a new one */
uint32_t rtl_spectrum_tx_said(char *s, size_t n);
/* The hand mic's level going out, dBFS (its loudest of the last 0.1 s), -99 none */
float rtl_spectrum_tx_mic_db(void);

/* The newest row: up to cap dB values from the lowest frequency, its sample rate (the span) and centre. False when
 * there is none, or none newer than *seq (which it updates). fresh: the first since a retune */
bool rtl_spectrum_frame(float *db, uint16_t cap, uint16_t *n, uint32_t *rate, int64_t *centre, uint32_t *seq,
                        bool *fresh);
