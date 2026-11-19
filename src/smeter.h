/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include "lvgl/lvgl.h"

/* The S scale in dB: 6 dB an S unit, S9 at -73 dBm, then dB over S9 */
#define S_MIN   (-127)
#define S1      (-121)
#define S2      (S1 + 6)
#define S3      (S1 + 12)
#define S4      (S1 + 18)
#define S5      (S1 + 24)
#define S6      (S1 + 30)
#define S7      (S1 + 36)
#define S8      (S1 + 42)
#define S9      (S1 + 48)
#define S9_20   (S9 + 20)
#define S9_40   (S9 + 40)

/* The S-meter box: bars, needle or the deck scales, by theme and Settings */
lv_obj_t *meter_init(lv_obj_t *parent);

/* From the DSP: a new reading in dB and its smoothing (0 none, towards 1 slower) */
void meter_update(float db, float beta);

/* From the DSP: the noise floor in the filter, in dB; the bars under it stay grey */
void meter_set_noise(float db);

/* New TX readings (tx_meters.c, LVGL thread): the box shows them while transmitting with no TX panel */
void meter_tx_redraw(void);

/* The needle dial on any box (the walkie screen), at the current reading */
void meter_needle_draw(lv_obj_t *obj, lv_draw_ctx_t *draw_ctx);

#ifdef BOARD_X6200
/* The supply as the TX power cap follows it (radio.c, LVGL thread): true on the battery alone. The X6200's attenuator
 * takes off more there, and the reading takes the band's figure for it back with ATT on (#125) */
void meter_set_battery(bool battery);

/* Any thread: the radio is on the battery alone, as the meter takes it */
bool meter_on_battery(void);
#endif

/* The last reading in dB, S1 to S9+40 */
int16_t meter_get_raw_db(void);

/* The same, unrounded (the front end measurement in Settings) */
float meter_get_db(void);

/* The meter as its box draws it, every figure, for the meters of other screens (the Sentry's A, B and G: the user,
 * "is the S meter you added showing the same information the stock meter shows? all data points because it should"):
 * dB S1 to S9+40. Any thread, as the box reads them */
typedef struct {
    float raw;          /* the reading as read: the box's value */
    float level;        /* smoothed (Settings' spectrum beta): the bars */
    float peak;         /* the peak bar: held 1.5 s, then falling faster and faster */
    float noise;        /* the noise floor in the filter: the bars under it grey */
    bool  tx;           /* transmitting: the receiver gives no reading, the box says TX */
} meter_state_t;
void meter_state(meter_state_t *s);

/* The box's bars: METER_BARS, bar i at meter_bar_db(i) dB (3 dB each up to S9, bar 16, then 5 dB each to S9+40), lit
 * from 1.5 dB under it; the scale's numbers at every 4th bar (meter_scale_text) */
#define METER_BARS      25
#define METER_LIT_SLACK 1.5f
int16_t meter_bar_db(int i);
extern const char *const meter_scale_text[7];

/* The value in S units as the box says it (VISION): S1 to S9, S9+n */
void meter_s_text(float db, char *s, size_t n);

/* Counts the readings: a new one when it changed (the scanner and the beacons take each reading
 * once, and none from before a retune) */
uint32_t meter_seq(void);

/* x of db on a bar w wide with S9 at 70% (Codec's SIGNAL bar, Ops Room's hexagons): S1 at 0, S9+40 at w */
lv_coord_t meter_x70(float db, lv_coord_t w);
