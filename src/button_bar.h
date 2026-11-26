/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 *
 *  The five buttons over F1-F5 and their pages. A page is five button_data_t; the
 *  apps make their own pages and load them, the front keys load the groups below.
 */

#pragma once

#include "mfk_control.h"
#include "vol_control.h"
#include "cfg/cfg_api.h"
#include "gui_styles.h"
#include "theme.h"

#define BUTTONS     F_BAR_BTNS
#define BTN_HEIGHT  BUTTONS_H
#define BTN_WIDTH   F_BAR_BTN_W
#define BTN_INSET   (theme_amber ? 0 : 1 + theme_shape.btn_gap)   /* space on every side of a drawn button; Amber Trey: none */

#ifdef __cplusplus
#include "params/params.h"
extern "C" {
#endif

#include "lvgl/lvgl.h"
#include "screen_main.h"

typedef enum {
    BTN_EMPTY,
    BTN_TEXT,       /* label: fixed text */
    BTN_TEXT_FN,    /* label_fn: text made at each show, again when subj changes */
} btn_type_t;

typedef struct _disp_button_t disp_btn_t;

/* "Name:\nvalue" texts show a small name over the value, "(PAGE 1:2)" a page button */
typedef struct button_data_t {
    btn_type_t type;
    union {
        const char *label;
        const char *(*label_fn)();
    };
    void (*press)(struct button_data_t *);
    void (*hold)(struct button_data_t *);
    struct buttons_page_t *next;        /* page buttons: the pages F1 and held F1 go to */
    struct buttons_page_t *prev;
    int32_t                ctrl;        /* knob control, app action or memory, by the press */
    bool                   encoder_allowed;  /* ctrl is a knob control: show its knob mark */
    disp_btn_t            *disp_btn;    /* where it is shown now, or NULL */
    Subject               *subj;
    Observer              *observer;
    bool                   mark;
    bool                   disabled;
} button_data_t;

typedef struct buttons_page_t {
    button_data_t *items[BUTTONS];
} buttons_page_t;

typedef buttons_page_t *buttons_group_t[];

extern buttons_page_t buttons_page_vol_1;
extern buttons_page_t buttons_page_rtty;
extern button_data_t  btn_dig_pwr;   /* TX power steps for the digital apps */
extern button_data_t  btn_ext_tune;  /* TUNE for an external tuner, on and off (ext_tune.h) */
extern button_data_t  btn_tx_pwr;    /* TX power on the knob (walkie.c too) */

/* GEN APP KEY DFN DFL VM: the key again shows the next page */
extern buttons_group_t buttons_group_gen;
extern buttons_group_t buttons_group_app;
extern buttons_group_t buttons_group_key;
extern buttons_group_t buttons_group_dfn;
extern buttons_group_t buttons_group_dfl;
extern buttons_group_t buttons_group_vm;

void            buttons_init(lv_obj_t *parent);
/* The five buttons to parent in a column, w x h each, F1 on top (the walkie screen) */
void            buttons_vertical(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);
/* The same, the first key starting top px down (what the parent shows across the top over it) */
void            buttons_vertical_at(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_coord_t top);
/* The same, each key its own width (ws[i], from the left edge): a key of little use takes less of the screen, one
 * of width 0 no box at all (its key still works) */
void            buttons_vertical_w(lv_obj_t *parent, const lv_coord_t *ws, lv_coord_t h, lv_coord_t top);
/* Then key i's box alone at y, w x h (from the left edge): one as small as its name, where the screen has room */
void            buttons_vertical_place(uint8_t i, lv_coord_t y, lv_coord_t w, lv_coord_t h);
/* Back in a row at the bottom of parent, where they were (the main screen) */
void            buttons_horizontal(lv_obj_t *parent);
/* The five and the knob lines over them hidden, or shown again (Settings > Display > Hide buttons, screen_main.c) */
void            buttons_hide(bool hide);
/* What F1-F5 show now (the remote's read-only "fkeys?", any thread): each one's text as the code gives it, its
 * line break a space ("Set clock: Auto", "(BANDS 1:1)"; "" for none), marked (checked), greyed; hid: Hide
 * buttons has them hidden */
void            buttons_shown(char text[][64], bool mark[], bool off[], bool *hid);
bool            buttons_hidden();
/* Show the button again (label, mark, disabled) if it is on screen */
void            buttons_refresh(button_data_t *data);
void            buttons_refresh_marks();
void            buttons_screen_rec_update();
void            buttons_mark(button_data_t *data, bool val);
void            buttons_disabled(button_data_t *data, bool val);
void            buttons_load(uint8_t n, button_data_t *data);
void            buttons_load_page(buttons_page_t *page);
void            buttons_unload_page();
/* press and hold of a page button */
void            button_next_page_cb(button_data_t *data);
void            button_prev_page_cb(button_data_t *data);
/* The page before data's (a page button's): its prev, else the page whose page button leads to data's page, found
 * along the next links, which is all most apps give theirs; NULL when they never come back (button_page.c, #317) */
buttons_page_t *buttons_page_before(const button_data_t *data);
/* What a hold of data does: its own hold, else, for a page button (its press button_next_page_cb), one page back
 * (button_prev_page_cb, #317); NULL: nothing */
void          (*buttons_hold_fn(const button_data_t *data))(button_data_t *);
/* The press of F1-F5, or its hold */
void            buttons_press(uint8_t n, bool hold);
/* The bench's remote ("btn NAME [hold]", remote_server.c): a main-screen button pressed or held by its name, as its F-key
 * does while its page is shown (cw_zap, cw_log, cw_peak, cw_peak_q, cw_decoder, cw_tuner, cw_snr, cw_parse, key_train,
 * key_mode). buttons_named: the name is one (not CW peak or its Q where the base has no such filter);
 * buttons_press_named on the LVGL thread, false when it does nothing */
bool            buttons_named(const char *name);
bool            buttons_press_named(const char *name, bool hold);
/* Every page of the main screen's groups (the remote's read-only "buttons?", any thread): pages split by '|', each its
 * buttons' names ("(MFK 2:4); AGC; IF shift; VOX"), split by "; ": none of a setting the base lacks */
void            buttons_layout_text(char *out, size_t size);
void            buttons_load_page_group(buttons_group_t group);
/* One of the group's pages is the one loaded (also while Hide buttons hides it) */
bool            buttons_group_shown(buttons_group_t group);
buttons_page_t *buttons_get_cur_page();
/* The USER page: F1-F5 of the user's own (#111) */
void            buttons_load_user_page(void);

/* The F-keys of the page loaded, as said: "F1 Run, F2 Scale, Log, F3 Span, 200 kHz"; "" with none */
void            buttons_page_text(char *out, size_t size);

#ifdef __cplusplus
}
#endif
