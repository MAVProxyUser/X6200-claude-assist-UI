/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - audio a client sends, played as it comes, through the digital apps' drive loop (tx_level.c): on a
 *  base whose ALC does not hold the power setting (HW_CAP_TX_NO_ALC: the X6200), and on the X6100's, whose ALC does
 *
 *  The X6200: there the audio level is the power. TCI audio went out as the client sent it: 1.1 W at a 0.1 W setting
 *  from -20 dBFS (2026-11-21, the bench's Watch stopped it), where the GUI's own digital apps go through tx_player.c's
 *  loop (tx_level.c, power_hold). This is that loop for a stream. What it learns is the peak the audio is played at,
 *  in dBFS: the client's audio is brought to it (its loudest frame so far sets the gain, which only falls within a
 *  transmission, so no pumping), whatever the client's own slider, so that a client at full scale does not go out
 *  20 dB hotter than one at -20 dBFS. Never over the client's own level: the audio is only taken down. A tx_level
 *  block for each TX_HOLD_BLOCK samples played, from the readings that came meanwhile: up to the setting, held there,
 *  down when the power reads over it; the level moved by a ramp. The level found is kept for the next transmission
 *  (the caller's setting), as tx_player keeps the apps'.
 *
 *  The X6100 (a base with ALC): TCI audio went out at -20 dBFS with the codec at its setting (0 dB), 36 dB over the
 *  level the GUI's own apps send at (tx_player_level_1w: -55.8 dBFS for 1 W on the bench X6100's factory V1.1.9 base,
 *  2026-11-21). The base's ALC held the power at the setting, reading 9.9 of 10, and its loop, opened up through the
 *  pause before the key, let the start of every over out hot: 0.4 W, then 0.9-1.0 W in the first second at a 0.1 W
 *  setting, before it came down to 0.0-0.1 W. The apps' audio, starting at their learned level and climbing, did not
 *  (0.10 W the same day). Now a TCI over starts there too: at the level TCI found last (tci_tx_level), or the apps' own
 *  while it has none (TX_HOLD_UNSET), the codec taking that level and the samples peaking TX_HOLD_HEAD_DB under full
 *  scale, at -20 dBFS as TCI's audio has since 2026-11-20 (the 2nd harmonic of the processor-to-base hop falls with
 *  the analogue level, 1 dB for 1 dB: 44 dB under at -20 dBFS with the codec at 0 dB; now the codec is 36 dB lower). Why the level matters: Xiegu's loop (tools/base_update/README.md) is an AGC
 *  on the audio, its gain climbing to its top in a pause (half a dB a 1.28 ms block) and coming down with the sound
 *  (0.01 of the error a block: 0.13 s), so the first sound after a pause goes out over the setting by as much as the
 *  loop had come down, what its ALC reads: 36 dB at -20 dBFS (the 1 W), a few dB at the apps' level. And the first
 *  TX_HOLD_FADE_S of sound fade in from TX_HOLD_FADE_DB under, slowly enough for the loop to follow (0.5 dB behind it
 *  in a model, tools/txlevel), so even those few dB do not go out: tx_level judges nothing before 1.4 s anyway
 *  (TX_LEVEL_SETTLE_BLOCKS). The client's audio is brought to the level, raised up to TX_HOLD_RAISE_DB (a quiet
 *  slider still drives the base), and the apps' loop runs on it with the ALC rule (tx_level.c): up while the ALC
 *  reads under 1, held once it shows, down a little when it reads over 3. TCI keeps its own level, not the apps': a
 *  voice from the browser's microphone ramps on what it reads, and it is not to move FT8's. The codec back to the
 *  playback volume of the settings at the end. Any other base: the audio as it came, nothing learned.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tx_level.h"

#define TX_HOLD_BLOCK    2048       /* tx_level's block (tx_player.c BLOCK_SAMPLES), at the play rate: 46 ms */
#define TX_HOLD_FLOOR    (-60.0f)   /* the lowest peak of the samples, dBFS */
#define TX_HOLD_FIRST    (-23.0f)   /* the X6200's level for 1 W the first time: -20 dBFS gave 1.1 W, 3 dB under it */
#define TX_HOLD_MIN_W    0.1f       /* a setting under this scales the level as this */
#define TX_HOLD_UNSET    1.0f       /* a base with ALC: no level of TCI's own yet (over full scale: none is), the apps' */
#define TX_HOLD_HEAD_DB  20.0f      /* a base with ALC: the samples' peak this far under full scale at the start, the
                                     * codec taking the rest: room for the loop's climb (TX_LEVEL_UP_MAX_DB) and for a
                                     * quiet client's raise (a slider at -32 dBFS still gets there) */
#define TX_HOLD_RAISE_DB 12.0f      /* a base with ALC: the most a quiet client is raised */
#define TX_HOLD_FADE_DB  6.0f       /* a base with ALC: the first sound this far under the level, then up to it in */
#define TX_HOLD_FADE_S   1.5f       /* this long, in dB a straight line (over the ALC's 3: about 4.7 dB, the most room
                                     * the apps' level leaves Xiegu's loop) */

/* The bases with ALC it runs on: the X6100's */
#if !defined(BOARD_X6200) && !(defined(BOARD_GSOC) && BOARD_GSOC)
#define TX_HOLD_ALC_BASE 1
#else
#define TX_HOLD_ALC_BASE 0
#endif

typedef struct {
    bool       on;              /* this base needs it: the rest is used only then */
    bool       alc;             /* a base with ALC (the X6100): the ALC rule, the codec set, the client raised */
    tx_level_t level;
    float      peak_dbfs;       /* the level: the peak to play at, dBFS at the codec's output (its gain in) */
    float      start_dbfs;      /* where it started this transmission */
    float      scale_db;        /* 10 log10 of the setting: the level for 1 W to this transmission's */
    float      codec_db;        /* the codec's gain this transmission (0: left as it is, the X6200) */
    float      raise_db;        /* the most the client's audio is raised (0: only taken down, the X6200) */
    int        client_pk;       /* the client's loudest sample this transmission */
    uint64_t   sounded;         /* samples since its first sound (the fade in, a base with ALC) */
    bool       started;         /* a frame of sound played: applied is its gain */
    float      applied;         /* the gain on the last frame, dB */
    uint8_t    seen;            /* the last tx_info reading taken */
    float      pwr, alc_read;   /* the last reading */
    bool       have;            /* not yet given to tx_level */
    bool       quiet;           /* only zeros since tx_level's last block (no readings then, tx_level.c) */
    uint64_t   played;          /* samples this transmission */
} tx_hold_t;

/* A transmission at target_w starting, before its key, from the level the last one kept. The level is kept as the peak
 * for 1 W: the power goes with the square of the audio on this path (SSB, no ALC), so the level for target_w is that
 * plus 10 log10(target_w), and a setting turned down from 5 W to 1 W does not go out at 5 W until the loop comes down
 * (0.75 dB a window: 6 s). On a base with ALC the same, but on one that sets the power itself (HW_CAP_TX_LEVEL: at
 * any power the same level); TX_HOLD_UNSET there starts from the apps' level. The codec is set here */
void tx_hold_begin(tx_hold_t *h, float target_w, float level_1w);

/* A frame of audio at the play rate, before it is played: its gain set in place */
void tx_hold_audio(tx_hold_t *h, int16_t *s, size_t n);

/* The transmission over (or refused after its tx_hold_begin): the codec back to the playback volume of the settings,
 * and the level for 1 W to keep for the next (level_1w as it was when nothing is to change) */
float tx_hold_end(tx_hold_t *h, float level_1w);
