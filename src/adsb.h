/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - ADS-B: Mode-S extended squitters decoded into a table of aircraft, each with its track history.
 *  Plain C, no LVGL: the reader thread (adsb_rx.c) feeds it frames, the AIRSPACE screen (app_adsb.c) draws it, and
 *  tools/adsb/adsb_test.c checks it on a PC. Every call but adsb_now() is made with adsb_lock() held.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADSB_MAX_PLANES 96      /* the oldest ghost makes room for a new aircraft */
#define ADSB_TRAIL      240     /* track points an aircraft keeps */
#define ADSB_LIVE_S     60.0    /* heard this recently: live; after it a ghost of the history */
#define ADSB_RATE_S     120     /* seconds of message rate kept */

typedef struct {
    float   lat, lon;
    int32_t alt;                /* ft, INT32_MIN unknown */
    double  t;                  /* adsb_now() */
} adsb_point_t;

typedef struct {
    uint32_t icao;
    char     call[9];           /* "" not heard yet */
    bool     alt_ok, vel_ok, vr_ok, pos_ok;
    int32_t  alt;               /* ft, barometric */
    float    gs, trk;           /* kt, degrees true */
    int32_t  vr;                /* ft/min, up positive */
    double   lat, lon, pos_t;
    uint32_t cpr_lat[2], cpr_lon[2];
    double   cpr_t[2];          /* 0: none yet; even [0], odd [1] */
    uint32_t msgs;
    double   first, seen;
    adsb_point_t trail[ADSB_TRAIL];
    int      trail_head, trail_n;   /* a ring: the newest at trail_head - 1 */
} adsb_plane_t;

typedef struct {
    uint32_t frames;            /* every frame from the receiver */
    uint32_t shorts;            /* 56-bit Mode-S */
    uint32_t longs;             /* 112-bit */
    uint32_t adsb;              /* DF17/18/19 with a good CRC */
    uint32_t crc_bad, other_df, junk;
} adsb_stats_t;

double adsb_now(void);          /* seconds, monotonic */

void adsb_lock(void);
void adsb_unlock(void);

/* Everything forgotten: the aircraft, their history, the counts */
void adsb_clear(void);

/* The receiver's place, for one-frame positions and the 180 NM sanity check; ok false: unknown */
void adsb_set_ref(bool ok, double lat, double lon);

/* A frame as the receiver sends it between '*' and ';' (after the 12-digit time of an '@' frame): 14 or 28 hex
 * digits. True when it was ADS-B with a good CRC */
bool adsb_frame_hex(const char *hex, size_t n, double now);

/* Aircraft not heard for keep_s go (keep_s < 0: never), history older than trail_s too (trail_s < 0: kept) */
void adsb_expire(double now, double keep_s, double trail_s);

int                 adsb_count(void);
const adsb_plane_t *adsb_plane(int i);
const adsb_stats_t *adsb_stats(void);

/* ADS-B messages a second, out[0] the oldest of the last n seconds (n <= ADSB_RATE_S) */
void adsb_rate(double now, uint16_t *out, int n);

/* Great-circle distance in NM and initial bearing in degrees */
double adsb_range_nm(double lat1, double lon1, double lat2, double lon2, double *bearing);

/* The Mode-S CRC-24 of n bytes: 0 for a good DF17/18 (parity in its last three) */
uint32_t adsb_crc24(const uint8_t *data, size_t n);
