/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - An RTL-SDR stick's spectrum for the Sentry's F VHF/UHF and G LOG screens (rtl_spectrum.c):
 *  /usr/bin/x6x00-rtlspec (tools/rtlspec) runs while it is wanted and its rows are kept here, the newest one. The
 *  Sentry wants it from its first start for as long as the GUI runs, so screens and apps come and go without the
 *  stick being stopped and started again (the radio's USB host suffers from that); AIRSPACE's screen has it while it
 *  is open (rtl_spectrum_yield: AIRSPACE's dump1090 lent it, adsb_rx_lend) and gives it back when it closes.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    RTL_SPEC_OFF,           /* not wanted */
    RTL_SPEC_NO_HELPER,     /* no /usr/bin/x6x00-rtlspec on this image */
    RTL_SPEC_NO_STICK,      /* no RTL-SDR on USB */
    RTL_SPEC_BUSY,          /* the stick, held by another program for now */
    RTL_SPEC_OPENING,       /* started, its first row not here yet */
    RTL_SPEC_ON,            /* rows coming */
    RTL_SPEC_STALLED,       /* the stick opened but sent nothing, twice: the radio's USB has to have it again */
} rtl_spec_state_t;

#define RTL_SPEC_MIN_HZ  24000000LL
#define RTL_SPEC_MAX_HZ  1766000000LL

/* Wanted (on, at hz with gain tenths of dB, -1 the tuner's AGC) or not: the helper starts or ends in the background */
void rtl_spectrum_want(bool on, int64_t hz, int gain);

/* While on: another frequency (kept within RTL_SPEC_MIN_HZ..MAX_HZ), another gain */
void rtl_spectrum_tune(int64_t hz);
void rtl_spectrum_gain(int tenths);

rtl_spec_state_t rtl_spectrum_state(void);

/* AIRSPACE's screen open (on): the helper stops and AIRSPACE's dump1090 may have the stick; closed: the helper again */
void rtl_spectrum_yield(bool on);

/* Listen to hz (mode 1 NFM, 2 AM, 3 WFM) on the speaker in place of the X6200's receiver; mode 0 or hz 0: back to
 * the X6200. Within the helper's span only: outside it is silent until the stick is retuned there */
void rtl_spectrum_listen(int64_t hz, int mode);
bool rtl_spectrum_listening(void);

/* While listening: the X6200's receiver mixed in with the stick (on: BOTH), or the stick alone */
void rtl_spectrum_mux(bool on);

/* OFF: the speaker taken from the X6200's receiver and given nothing (on), or let go */
void rtl_spectrum_mute(bool on);

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
