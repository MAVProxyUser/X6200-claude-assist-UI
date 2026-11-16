/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The SDR pool (sdr_pool.c): every SDR on USB (each RTL-SDR by its own number, a HackRF, a bladeRF), who
 *  has which, and whose audio is heard. One place decides it, so no user of an SDR takes one another has:
 *
 *  - B (the Sentry's VHF/UHF screen) through the SDR daemon x6x00-rtlspec: the one chosen on B, else the daemon's own
 *    order (a HackRF, a bladeRF, RTL0). G's SDR channels and C's stick entries are B's SDR, retuned.
 *  - Watch channels (rtl_watch.c): an RTL each, from the last down.
 *  - AIRSPACE (adsb_rx.c): its spares, an RTL each, from the first up; B's SDR itself only with no spare at all (one
 *    SDR), and only while AIRSPACE's screen is open.
 *  - What the user stopped in the pool's panel stays stopped (kept by serial in BOARD_DATA "/sdr_pool").
 *
 *  Power: an SDR on the radio's own HOST power streams alone; those behind a hub of their own power (a KrakenSDR's
 *  RTLs) stream beside each other and beside the one on the radio's power (a HackRF and an RTL-SDR on an unpowered
 *  hub browned the port out, 2026-11-15; five RTLs on the Kraken streamed together, 2026-11-16).
 */

#pragma once

#include "rtl_spectrum.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SDR_POOL_UNITS 10                       /* RTL0-RTL7, a HackRF, a bladeRF */

/* Who has one: B, AIRSPACE's spares 0-6 (every RTL but B's), the watch channels 0-3 */
#define SDR_POOL_ADSB  7
#define SDR_POOL_WATCH 4
enum { SDR_OWN_NONE = -1, SDR_OWN_B = 0, SDR_OWN_ADSB = 1, SDR_OWN_WATCH = SDR_OWN_ADSB + SDR_POOL_ADSB,
       SDR_OWNERS = SDR_OWN_WATCH + SDR_POOL_WATCH };

typedef struct {
    rtl_dev_t kind;
    int       rtl;                              /* RTL<n> for an RTL-SDR, -1 else */
    char      name[12];                         /* "RTL2", "HACKRF", "BLADERF" */
    char      sn[24], loc[24];                  /* serial, USB location ("3-1.3") */
    bool      own_power;                        /* behind a hub of its own power */
    bool      stopped;                          /* stopped by the user in the panel: nobody's */
    int       owner;                            /* SDR_OWN_*, SDR_OWN_NONE free */
} sdr_unit_t;

/* Started once: its look at USB each second, the stopped ones read back */
void sdr_pool_init(void);

/* The units now, RTL0... then a HackRF, a bladeRF; how many */
int  sdr_pool_units(sdr_unit_t *out, int max);
int  sdr_pool_rtl_count(void);

/* What an owner should hold now: the unit's index in sdr_pool_units (-1 none), its serial and location for an RTL */
int  sdr_pool_held(int owner, sdr_unit_t *u);

/* An owner wanting one or not: AIRSPACE's spares as AIRSPACE wants them, the watch channels as they are set */
void sdr_pool_want(int owner, bool want);

/* B's SDR as the daemon has it, and as asked for (rtl_spectrum.c), said to the pool */
void sdr_pool_b(rtl_dev_t held, int held_rtl, rtl_dev_t want, int want_rtl);

/* The panel's: a unit stopped (nobody's) or free again; whether one may stream beside the others (power) */
void sdr_pool_stop(int unit, bool stop);
/* One at a change of who has what (an owner's thread compares it) */
unsigned sdr_pool_gen(void);

/* Whose audio is heard (the player in rtl_spectrum.c): B's by its LISTEN, a watch channel's unless muted here */
void sdr_pool_hear(int owner, bool on);
bool sdr_pool_heard(int owner);

/* An owner's name for the panel: "B", "AIRSPACE", "WATCH 162.550" */
void sdr_pool_owner_name(int owner, char *s, size_t n);

#ifdef __cplusplus
}
#endif
