/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#if HAVE_LIBGPS
#include <gps.h>          /* gpsd client library; a PC without it builds without GPS */
#endif
#include <stdbool.h>      /* for the C files that include only this */
#include <stddef.h>

typedef enum {
    GPS_STATUS_WAITING=0,
    GPS_STATUS_WORKING,
    GPS_STATUS_RESTARTING,
    GPS_STATUS_EXITED,
} gps_status_t;

void gps_init();

gps_status_t gps_status();

/* gpsd's last 2D or 3D position, any thread; age in seconds. False before the first fix */
bool gps_last_fix(double *lat, double *lon, int *age);

/* The phone's position, sent by the Panel page ("pos"): kept apart from gpsd's, any thread; acc the phone's own
 * accuracy in metres. False before the first */
void gps_phone_set(double lat, double lon, int acc);
bool gps_phone_fix(double *lat, double *lon, int *age, int *acc);

/* How the GPS is doing, as gpsd last said (the Sentry's GPS panel): any thread. NAN where gpsd gave nothing */
#define GPS_SATS 32
typedef struct {
    short prn, ss;              /* ss: signal, dB-Hz (0: not heard) */
    short el, az;               /* elevation, azimuth (degrees true); el -99 where gpsd gives none */
    bool  used;
    char  sys;                  /* G GPS, R GLONASS, E Galileo, B BeiDou, Q QZSS, S SBAS, I NavIC, ? */
} gps_sat_t;
typedef struct {
    bool   reporting;           /* gpsd reported in the last 5 s */
    int    report_age_ms;       /* since its last report, -1 none yet */
    char   path[64];            /* the device it came from ("/dev/pts/1": the Bluetooth GPS's bridge) */
    char   driver[40];          /* "NMEA0183", "u-blox"... */
    int    mode;                /* 0 not known, 1 no fix, 2 2D, 3 3D */
    int    seen, used;          /* satellites in view, used in the fix */
    int    nsat;
    gps_sat_t sat[GPS_SATS];
    double hdop, pdop;
    double eph, epv;            /* gpsd's estimated error, horizontal and vertical, m */
    double lat, lon, alt;       /* the last fix's; alt above sea level, m */
    double speed, track;        /* m/s, degrees true */
    int    fix_age_ms;          /* since the last 2D or 3D fix, -1 none yet */
    double rate;                /* new positions a second, over the last few seconds */
    double reports;             /* gpsd's reports a second (positions and satellites), the same seconds */
    long   utc;                 /* the GPS's own time of its last report (s since 1970), 0 none */
} gps_health_t;
void gps_health(gps_health_t *h);

/* The Bluetooth GPS gpsd is given (btgps-ctl): its address and name ("" for none chosen) and its link as
 * /tmp/btgps.state says ("connected", "trying", "off"). Reads files: the LVGL thread, a few times a second at most */
void gps_bt_link(char *mac, size_t mac_n, char *name, size_t name_n, char *state, size_t state_n);
