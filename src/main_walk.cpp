/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the MFK walking the main screen's values (main_walk.h, #113).
 *
 *  Every theme lays the values out its own way (the box at the top, the frequency strip, Black Metal's and Total
 *  Black's two rows, VISION's), so nothing here knows where they are: each value is asked of the box and the strip
 *  (info_bar_value, freq_strip_value), the first of them on the screen now is the one framed, and a value no layout
 *  shows is passed over. The frame is an object of the top layer laid over the label's place, followed every 300 ms
 *  (a value that changes width, a theme change). The change itself is the knob controls' (controls_encoder_update):
 *  what the MFK does when one of them is bound to it. The VFO and the mode have none: a turn on the VFO switches
 *  A and B, on the mode it steps through the modes.
 */

#include "main_walk.h"

#include "app_window.h"
#include "cfg/cfg_api.h"
#include "cfg/settings_manager.h"
#include "freq_strip.h"
#include "info_bar.h"
#include "radio_controls.h"
#include "theme.h"

#include <string>

extern "C" {
#include "msg_box.h"
#include "radio.h"
#include "screen_main.h"
}

#define IDLE_MS 6000

static const struct {
    const char *name;
    cfg_ctrl_t  ctrl;           /* CTRL_LAST (none): VFO and mode, their own */
} VALUES[IV_NUM] = {
    {"VFO", CTRL_LAST},   {"Mode", CTRL_LAST}, {"AGC", CTRL_AGC}, {"PRE", CTRL_PRE}, {"ATT", CTRL_ATT},
    {"ATU", CTRL_ATU},    {"NR", CTRL_NR},     {"NB", CTRL_NB},   {"DNF", CTRL_DNF}, {"RIT", CTRL_RIT},
    {"XIT", CTRL_XIT},    {"Step", CTRL_STEP},
};

static bool        walking, picked;
static int         at;                  /* the value framed */
static lv_obj_t   *frame;
static lv_timer_t *follow_timer;
static uint32_t    last_ms;
static uint32_t    done_ms;             /* the press that ended a walk: the long press that may follow is its own */

/* The label of value v on the screen now, NULL when no layout shows it now */
static lv_obj_t *shown(int v) {
    lv_obj_t *o = info_bar_value((info_value_t)v);

    if (!o || !lv_obj_is_visible(o)) o = freq_strip_value((info_value_t)v);
    return o && lv_obj_is_visible(o) ? o : NULL;
}

static void frame_place(void) {
    lv_obj_t *o = shown(at);
    lv_area_t a;

    if (!frame || !o) {
        if (frame) lv_obj_add_flag(frame, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_get_coords(o, &a);
    lv_obj_set_pos(frame, a.x1 - 3, a.y1 - 3);
    lv_obj_set_size(frame, lv_area_get_width(&a) + 6, lv_area_get_height(&a) + 6);
    lv_obj_set_style_border_width(frame, picked ? 3 : 2, 0);
    lv_obj_set_style_border_color(frame, picked ? TC(accent) : TC(text), 0);
    lv_obj_set_style_bg_color(frame, TC(accent), 0);
    lv_obj_set_style_bg_opa(frame, picked ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(frame);
}

#define SAY_MS 4000                     /* the hint stays a while: the walk is new to most */

static void say(void) {
    if (!picked) {
        msg_update_text_ms(SAY_MS, "#%06x %s#: press to change it", theme_hex(TC_btn_mark), VALUES[at].name);
        return;
    }
    if (VALUES[at].ctrl != CTRL_LAST) {
        std::string m(64, '\0');

        controls_encoder_update(VALUES[at].ctrl, 0, m);
        msg_update_text_fmt("%s", m.c_str());
    } else {
        msg_update_text_ms(SAY_MS, "%s: %s", VALUES[at].name,
                            at == IV_VFO ? info_params_vfo_label_get() : info_params_mode_label_get());
    }
}

static void follow_cb(lv_timer_t *t) {
    (void)t;
    if (dialog_is_run() || lv_tick_elaps(last_ms) > IDLE_MS) {     /* an app opened, or left untouched */
        main_walk_stop();
        return;
    }
    frame_place();
}

bool main_walk_on(void) {
    return walking;
}

bool main_walk_just_done(void) {
    return !walking && done_ms && lv_tick_elaps(done_ms) < 1500;
}

void main_walk_stop(void) {
    if (!walking) return;
    walking = picked = false;
    if (follow_timer) lv_timer_del(follow_timer);
    follow_timer = NULL;
    if (frame) lv_obj_add_flag(frame, LV_OBJ_FLAG_HIDDEN);
    freq_strip_walk(false);
    msg_update_text_fmt("MFK: its own control again");
}

void main_walk_toggle(void) {
    if (walking) {
        main_walk_stop();
        return;
    }
    if (dialog_is_run()) return;
    walking = true;
    picked = false;
    freq_strip_walk(true);              /* RIT and XIT where they can be framed */
    if (!frame) {
        frame = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(frame);
        lv_obj_set_style_radius(frame, 4, 0);
        lv_obj_clear_flag(frame, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    }
    at = 0;
    while (at < IV_NUM - 1 && !shown(at)) at++;
    last_ms = lv_tick_get();
    follow_timer = lv_timer_create(follow_cb, 300, NULL);
    frame_place();
    say();
}

bool main_walk_mfk(int dir) {
    if (!walking) return false;
    last_ms = lv_tick_get();
    if (dir == 0) {                     /* the press */
        if (picked) {                   /* done: the MFK back */
            done_ms = lv_tick_get();
            main_walk_stop();
        } else {
            picked = true;
            frame_place();
            say();
        }
        return true;
    }
    if (picked) {                       /* the value framed, changed */
        if (VALUES[at].ctrl != CTRL_LAST) {
            std::string m(64, '\0');

            controls_encoder_update(VALUES[at].ctrl, dir, m);
            msg_update_text_fmt("%s", m.c_str());
        } else {
            if (at == IV_VFO) radio_toggle_vfo();
            else main_screen_mode_step(dir);
            say();
        }
        frame_place();
        return true;
    }
    return main_walk_ring(dir);
}

bool main_walk_ring(int dir) {
    if (!walking) return false;
    last_ms = lv_tick_get();
    picked = false;
    /* to the next value shown, either way round */
    for (int k = 1; k <= IV_NUM; k++) {
        int v = ((at + dir * k) % IV_NUM + IV_NUM) % IV_NUM;

        if (shown(v)) {
            at = v;
            break;
        }
    }
    frame_place();
    say();
    return true;
}
