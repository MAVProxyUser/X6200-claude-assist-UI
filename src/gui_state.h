/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - The GUI's state across its own restart (gui_state.c): where it was and what was going on there
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the GUI was: the Sentry (its own state under its keys), an app ("app": its action), the main screen */
#define GUI_PLACE_SENTRY 1
#define GUI_PLACE_APP    2
#define GUI_PLACE_MAIN   3

/* At the start: what the GUI before this one kept, read; whether this start comes back to it (not after a start
 * that went down again within its first minute twice: a state that crashes it is not restored a third time) */
void gui_state_init(void);
bool gui_state_restoring(void);
/* Restoring: the place the GUI before this one was at, as it was read at the start (GUI_PLACE_...); 0 otherwise */
int  gui_state_start_place(void);

/* Kept under a key, written out within 2 s of a change (DATA's copy a minute at most behind); read back at a restart
 * and at the next switch-on */
void    gui_state_set(const char *key, int32_t val);
int32_t gui_state_get(const char *key, int32_t def);
/* Written out now, DATA's copy too: the power-off */
void    gui_state_flush(void);

#ifdef __cplusplus
}
#endif
