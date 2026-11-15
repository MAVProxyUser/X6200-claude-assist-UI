/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the MFK walking the main screen's values (#113): a long press of the MFK, and the MFK moves a frame
 *  over the values the screen shows (VFO, mode, AGC, PRE, ATT, ATU, NR, NB, DNF, RIT, XIT, the step), a press picks
 *  the one framed and a turn changes it where it is, a press again (or 6 s untouched) gives the MFK back. The X6200's
 *  inner ring moves the frame too. The main knob tunes all along. With a keyboard the arrows and Enter do the same,
 *  Esc leaves.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool main_walk_on(void);
void main_walk_toggle(void);        /* the MFK's long press */
void main_walk_stop(void);

/* The MFK on the main screen while walking: dir -1/+1 a turn, 0 the press. False: not walking, the MFK as ever */
bool main_walk_mfk(int dir);

/* The X6200's inner ring while walking: the frame to the next value or the one before. False: not walking */
bool main_walk_ring(int dir);

/* A press just ended the walk (picked value done): the long press it may grow into is not a new walk */
bool main_walk_just_done(void);

#ifdef __cplusplus
}
#endif
