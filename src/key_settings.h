/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - what the keys do (#111): the lists of actions and keys of Settings > Keys and of the web
 *  configurator's Keys page (cfg_socket.cpp), one list for both. C++ only.
 *
 *  - The page keys held (GEN APP KEY MSG DFN DFL) and the hand mic keys: an action each, in params.
 *  - The top and side keys (FST, ATU, A/B, the band and mode keys; the X6100's PRE and AGC too): a press and a hold
 *    each, KEY_OWN the key's own job (screen_main.c key_assigned). FST's hold is params.long_fst, its own the step
 *    down, as before.
 *  - The USER page's F1-F5 (button_bar.cpp) and the lock per band and mode: cfg_user_key, cfg_lock_per_band.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "cfg/cfg_api.h"

extern "C" {
#include "params/params.h"
}

struct key_action_t {
    const char     *label;      /* as the dropdowns show it, a space each side */
    press_action_t  action;
};

/* The front keys' list (page keys, top and side keys, the USER page); the hand mic's, shorter */
extern const key_action_t key_front_actions[];
extern const size_t       key_front_n;
extern const key_action_t key_mic_actions[];
extern const size_t       key_mic_n;

/* True when the list offers it */
bool key_action_ok(const key_action_t *list, size_t n, int32_t action);

/* The page keys held, then the hand mic keys */
struct key_row_t {
    const char *id;             /* the web's name: "long_gen", "mic_f1" ... */
    const char *label;
    uint8_t    *val;
    bool       *dirty;
};

#define KEY_ROWS_FRONT 6        /* GEN to DFL; then the hand mic's */

extern const key_row_t key_rows[];
extern const size_t    key_rows_n;

const key_action_t *key_row_actions(size_t row, size_t *n);
void                key_row_set(size_t row, int32_t action);

/* The top and side keys */
struct key_top_t {
    key_assign_t k;
    const char  *id;            /* the web's name: "fst", "band_up" ... */
    const char  *name;
    const char  *own_press, *own_hold;
};

extern const key_top_t key_top[];
extern const size_t    key_top_n;

int32_t key_top_get(size_t i, bool held);           /* KEY_OWN or an action */
void    key_top_set(size_t i, bool held, int32_t action);
void    key_top_reset(void);                        /* every key its own job */
