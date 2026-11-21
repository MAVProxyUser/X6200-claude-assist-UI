/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - audio a client sends, played as it comes, through the drive loop on a base whose ALC does not hold the
 *  power setting (HW_CAP_TX_NO_ALC: the X6200)
 *
 *  There the audio level is the power. TCI audio went out as the client sent it: 1.1 W at a 0.1 W setting from
 *  -20 dBFS (2026-11-21, the bench's Watch stopped it), where the GUI's own digital apps go through tx_player.c's loop
 *  (tx_level.c, power_hold). This is that loop for a stream. What it learns is the peak the audio is played at, in
 *  dBFS: the client's audio is brought to it (its loudest frame so far sets the gain, which only falls within a
 *  transmission, so no pumping), whatever the client's own slider, so that a client at full scale does not go out
 *  20 dB hotter than one at -20 dBFS. Never over the client's own level: the audio is only taken down. A tx_level
 *  block for each TX_HOLD_BLOCK samples played, from the readings that came meanwhile: up to the setting, held there,
 *  down when the power reads over it; the level moved by a ramp. The level found is kept for the next transmission
 *  (the caller's setting), as tx_player keeps the apps'. Any other base: the audio as it came, nothing learned.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tx_level.h"

#define TX_HOLD_BLOCK   2048        /* tx_level's block (tx_player.c BLOCK_SAMPLES), at the play rate: 46 ms */
#define TX_HOLD_FLOOR   (-60.0f)    /* the lowest peak, dBFS */
#define TX_HOLD_FIRST   (-23.0f)    /* the level for 1 W the first time: -20 dBFS gave 1.1 W, 3 dB under it */
#define TX_HOLD_MIN_W   0.1f        /* a setting under this scales the level as this */

typedef struct {
    bool       on;              /* this base needs it: the rest is used only then */
    tx_level_t level;
    float      peak_dbfs;       /* the level: the peak to play at, dBFS */
    float      start_dbfs;      /* where it started this transmission */
    float      scale_db;        /* 10 log10 of the setting: the level for 1 W to this transmission's */
    int        client_pk;       /* the client's loudest sample this transmission */
    float      applied;         /* the gain on the last frame, dB; > 0 none yet */
    uint8_t    seen;            /* the last tx_info reading taken */
    float      pwr, alc;        /* the last reading */
    bool       have;            /* not yet given to tx_level */
    bool       quiet;           /* only zeros since tx_level's last block (no readings then, tx_level.c) */
    uint64_t   played;          /* samples this transmission */
} tx_hold_t;

/* A transmission at target_w starting, before its key, from the level the last one kept. The level is kept as the peak
 * for 1 W: the power goes with the square of the audio on this path (SSB, no ALC), so the level for target_w is that
 * plus 10 log10(target_w), and a setting turned down from 5 W to 1 W does not go out at 5 W until the loop comes down
 * (0.75 dB a window: 6 s) */
void tx_hold_begin(tx_hold_t *h, float target_w, float level_1w);

/* A frame of audio at the play rate, before it is played: its gain set in place */
void tx_hold_audio(tx_hold_t *h, int16_t *s, size_t n);

/* The transmission over: the level for 1 W to keep for the next (level_1w as it was when nothing is to change) */
float tx_hold_end(tx_hold_t *h, float level_1w);
