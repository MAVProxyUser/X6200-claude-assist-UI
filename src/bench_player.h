/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the bench's file player: a /tmp/bench_*.wav on this radio's speaker (bench_player.c)
 */

#pragma once

#include <stdbool.h>

/* Played on the speaker through the base's play mode, on a thread of its own: false for any other path, a file not
 * yet ended, or the radio transmitting. Any thread */
bool bench_player_start(const char *path);
/* Ended at once (the queued audio dropped) */
void bench_player_stop(void);
/* A file is playing */
bool bench_player_busy(void);
