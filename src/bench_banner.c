/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The bench suite's line across the top of the screen (bench_banner.h). The owner, 2026-11-19: "when the suite is
 *  running there should be a screen that says which device is TXing and which is RXing and exactly which test is
 *  being performed". The suite's text says the test and both radios' parts; the box in front is this radio's own
 *  word, from its transmit state twice a second: TX on red while it is on the air, RX otherwise. On the top layer,
 *  over any screen and app, and it takes no touches.
 */

#include "bench_banner.h"

#include "gui_styles.h"
#include "radio.h"
#include "theme.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <string.h>

#define BANNER_H 40

static lv_obj_t   *box, *state, *label;
static lv_timer_t *timer;

static void state_cb(lv_timer_t *t) {
    bool tx = radio_get_state() == RADIO_TX;

    lv_label_set_text(state, tx ? "TX" : "RX");
    lv_obj_set_style_bg_color(state, tx ? TC(tx_bg) : TC(bg), 0);
    lv_obj_set_style_text_color(state, tx ? TC(text) : TC(accent), 0);
}

static void make(void) {
    box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), BANNER_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(box, TC(bg), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_border_color(box, TC(accent), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    state = lv_label_create(box);
    lv_obj_set_size(state, 56, BANNER_H - 8);
    lv_obj_align(state, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_font(state, &sony_26, 0);
    lv_obj_set_style_text_align(state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(state, 3, 0);
    lv_obj_set_style_bg_opa(state, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(state, TC(accent), 0);
    lv_obj_set_style_border_width(state, 1, 0);

    label = lv_label_create(box);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_pad_left(label, 68, 0);
    lv_obj_set_style_pad_right(label, 6, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(label, &sony_20, 0);
    lv_obj_set_style_text_color(label, TC(accent), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
}

void bench_banner_set(const char *text) {
    if (!text || !text[0]) {
        if (timer) {
            lv_timer_del(timer);
            timer = NULL;
        }
        if (box) {
            lv_obj_del(box);
            box = state = label = NULL;
        }
        return;
    }
    if (!box) {
        make();
        timer = lv_timer_create(state_cb, 500, NULL);
    }
    lv_label_set_text(label, text);
    lv_obj_move_foreground(box);
    state_cb(NULL);
}
