/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An RTL-SDR stick's spectrum for the Sentry's spectrum, LOG and WALKIE screens (rtl_spectrum.c). The
 *  stick is held by /usr/bin/x6x00-rtlspec (tools/rtlspec), a daemon that opens it once and keeps it streaming for as
 *  long as the radio runs; this side connects to it, asks for rows while they are wanted and keeps the newest one.
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
} rtl_spec_state_t;

#define RTL_SPEC_MIN_HZ  24000000LL
#define RTL_SPEC_MAX_HZ  1766000000LL

/* Wanted (on, at hz with gain tenths of dB, -1 the tuner's AGC) or not: rows asked of the daemon or not, which is
 * started (in the background, for good) the first time */
void rtl_spectrum_want(bool on, int64_t hz, int gain);

/* While on: another frequency (kept within RTL_SPEC_MIN_HZ..MAX_HZ), another gain */
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

/* While listening: the X6200's receiver mixed in with the stick (on: BOTH), or the stick alone */
void rtl_spectrum_mux(bool on);

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

/* The tuner the helper found ("R820T"), "" before */
void rtl_spectrum_tuner(char *s, size_t n);

/* The newest row: up to cap dB values from the lowest frequency, its sample rate (the span) and centre. False when
 * there is none, or none newer than *seq (which it updates). fresh: the first since a retune */
bool rtl_spectrum_frame(float *db, uint16_t cap, uint16_t *n, uint32_t *rate, int64_t *centre, uint32_t *seq,
                        bool *fresh);
