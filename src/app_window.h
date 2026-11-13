/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

/* App windows: one at a time over the main screen, with their own button page */

#ifndef X6100_DIALOG_H
#define X6100_DIALOG_H

#include "lvgl/lvgl.h"
#include <liquid/liquid.h>
#include "helpers.h"
#include "button_bar.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*dialog_construct_cb_t)(lv_obj_t *parent);    /* builds the window in parent */
typedef void (*dialog_destruct_cb_t)(void);                 /* before its object is deleted */
typedef void (*dialog_audio_cb_t)(unsigned int count, float *samples);
typedef void (*dialog_rotary_cb_t)(int32_t steps);          /* the main knob */

typedef struct {
    lv_obj_t              *obj;             /* set by construct_cb, deleted on close */
    dialog_construct_cb_t  construct_cb;
    dialog_destruct_cb_t   destruct_cb;
    dialog_audio_cb_t      audio_cb;        /* wants the receiver's audio */
    dialog_rotary_cb_t     rotary_cb;
    buttons_page_t        *btn_page;        /* shown while open */
    buttons_page_t        *prev_page;       /* given back on close */
    lv_event_cb_t          key_cb;          /* keys of its controls (dialog_item()) */
    bool                   restore_radio;   /* frequency and mode saved on open, back on close */
    /* The front panel's keys (keypad_key_t, keypad_state_t) before the main screen acts on them: true when taken.
     * Only an app over the whole screen wants them (the Sentry): the others leave it NULL */
    bool                 (*front_key_cb)(int key, int state);
    /* True while the main knob is the window's alone (the Sentry listening to an RTL-SDR): the radio is not tuned by
     * it, rotary_cb still gets the turns. NULL: the radio is tuned as ever */
    bool                 (*rotary_takes_cb)(void);
    bool                   run;             /* open now */
} dialog_t;

/* The open window has the main knob to itself (rotary_takes_cb) */
bool dialog_rotary_taken(void);

/* A window that has no APP action (the Sentry's UNIX SHELL), opened as the APP grid is (screen_main.c) */
void main_screen_open_dialog(dialog_t *d);

/* Open dialog over parent (nothing if it is open already); close the open one */
void dialog_construct(dialog_t *dialog, lv_obj_t *parent);
void dialog_destruct();

/* The open app changed the mode from mode to forced: back to mode when it closes */
void dialog_restore_mode_on_close(int32_t mode, int32_t forced);
/* The app just opened (one without restore_radio) gives back frequency and mode when it closes */
void dialog_restore_radio_on_close(void);

/* Each digital app opens where it was last used (frequency and mode, /mnt/app_freqs):
 * the next dialog_construct() tunes to name's after saving the radio, its close keeps
 * the app's last ones for the next time. The two calls for an app without a dialog (RTTY) */
void dialog_app_memory(const char *name);
bool app_mem_load(const char *name, int32_t *freq, int32_t *mode);
void app_mem_save(const char *name);

/* An event to the open window (queued, from any thread) */
void dialog_send(lv_event_code_t code, void *param);
bool dialog_is_run();
bool dialog_type_is_run(dialog_t *which);

/* The window's object, and each of its controls */
lv_obj_t *dialog_init(lv_obj_t *parent);
void dialog_item(dialog_t *dialog, lv_obj_t *item);

/* The receiver's audio and the main knob, passed to the open window */
bool dialog_need_audio();
void dialog_audio_samples(unsigned int count, float *samples);
void dialog_rotary(int32_t steps);
/* A front panel key to the open window first: true when it took it */
bool dialog_front_key(int key, int state);

#ifdef __cplusplus
}
#endif

#endif
