/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - On the Air (dialog_swl.h): what the broadcast list (EiBi, Settings > Info > Broadcast list) has on the
 *  air now, as a list to move through with the MFK and tune with its press, on both radios (#155).
 *
 *  EiBi has no category column; what an entry is comes from what it holds: a language code starting with '-' (-CW,
 *  -TY teletype, -TS time signals, -HF) or a name of the navies, air forces, VOLMETs and the like is Utility; a name
 *  that starts with an ENIGMA designation ("E11 Polish Spy Numbers") or says Numbers, and the Russian markers ("The
 *  Buzzer", "The Squeaky Wheel", "The Pip"), are Numbers;
 *  the rest is Broadcast. A Broadcast entry is tuned in AM, Morse in CW, the others in USB (the list gives their dial
 *  frequency). The list is read again each minute, the selection staying on its entry while it is on the air.
 */

#define _GNU_SOURCE     /* strcasestr */
#include "dialog_swl.h"

#include "broadcast_db.h"
#include "buttons.h"
#include "cfg/cfg_api.h"
#include "cfg/settings_manager_api.h"
#include "channel_list.h"
#include "events.h"
#include "fonts/font_select.h"
#include "keyboard.h"
#include "msg_box.h"
#include "radio.h"
#include "screen_main.h"
#include "styles.h"
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_ROWS   2048     /* on the air at once: about 1500 at the busiest hours */
#define SHOW       7        /* rows on the screen */
#define ROW_H      29
#define ROWS_Y     38
#define NEAR_HZ    250000   /* Near: this far either side of the dial */

typedef enum { CAT_ALL, CAT_BC, CAT_UTIL, CAT_NUM, CAT_N } cat_t;

static const char *const CAT_NAMES[CAT_N] = {"All", "Broadcast", "Utility", "Numbers"};

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void show_cb(button_data_t *b);
static void show_hold_cb(button_data_t *b);
static void range_cb(button_data_t *b);
static void follow_cb(button_data_t *b);
static void tune_cb(button_data_t *b);
static void save_cb(button_data_t *b);
static const char *show_label(void);
static const char *range_label(void);
static const char *follow_label(void);

static button_data_t btn_show   = {.type = BTN_TEXT_FN, .label_fn = show_label, .press = show_cb, .hold = show_hold_cb};
static button_data_t btn_range  = {.type = BTN_TEXT_FN, .label_fn = range_label, .press = range_cb};
static button_data_t btn_follow = {.type = BTN_TEXT_FN, .label_fn = follow_label, .press = follow_cb};
static button_data_t btn_tune   = {.type = BTN_TEXT, .label = "Tune", .press = tune_cb};
static button_data_t btn_save   = {.type = BTN_TEXT, .label = "Save as\nchannel", .press = save_cb};
static buttons_page_t page = {{&btn_show, &btn_range, &btn_follow, &btn_tune, &btn_save}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
};

dialog_t *dialog_swl = &dialog;

static cat_t      show = CAT_ALL;
static bool       near_dial;
static bool       follow;           /* tune as the selection moves */
static uint32_t   rows[MAX_ROWS];
static int        rows_n, sel, top;
static lv_timer_t *minute_timer, *follow_timer;
static lv_obj_t   *head_l, *detail_l, *row_obj[SHOW], *row_l[SHOW][4];

/* ---- What an entry is ---- */

static bool name_has(const char *name, const char *const *words) {
    for (; *words; words++)
        if (strcasestr(name, *words)) return true;
    return false;
}

static cat_t category(const broadcast_match_t *m) {
    static const char *const util[] = {"navy", "naval", "air force", "usaf", "volmet", "coast guard", "military",
                                       "hfgcs", "meteo", "weather", "noaa", "marine", "maritime", "aero", "ldoc",
                                       "hfdl", "stanag", "fax", "army", "embassy", "diplomatic", NULL};
    const char *n = m->station;

    /* E11, S28, V07, M12a, XPA...: a letter, one to three digits (a letter after them sometimes), a space */
    if (isupper((unsigned char)n[0]) && isdigit((unsigned char)n[1])) {
        int k = 2;

        while (k < 4 && isdigit((unsigned char)n[k])) k++;
        if (isalpha((unsigned char)n[k]) && n[k + 1] == ' ') k++;
        if (n[k] == ' ' || n[k] == 0) return CAT_NUM;
    }
    /* and the Russian channel markers listeners count with them (4625 kHz all day, 3364, 3756) */
    static const char *const marker[] = {"numbers", "spy ", "the buzzer", "squeaky wheel", "the pip", NULL};

    if (name_has(n, marker)) return CAT_NUM;
    if (m->language[0] == '-' || name_has(n, util)) return CAT_UTIL;
    return CAT_BC;
}

static radio_mode_t mode_of(const broadcast_match_t *m) {
    cat_t c = category(m);

    if (c == CAT_BC) return radio_mode_am;
    return strcmp(m->language, "-CW") == 0 ? radio_mode_cw : radio_mode_usb;
}

/* ---- The list ---- */

static void build(void) {
    static uint32_t all[MAX_ROWS];
    int32_t         f = cparam_i_get(cfg_fg_freq);
    int32_t         lo = near_dial ? f - NEAR_HZ : 100000, hi = near_dial ? f + NEAR_HZ : 30000000;
    uint32_t        keep = sel >= 0 && sel < rows_n ? rows[sel] : UINT32_MAX;
    size_t          n = broadcast_db_on_air(lo, hi, all, MAX_ROWS);

    rows_n = 0;
    for (size_t i = 0; i < n; i++) {
        const broadcast_match_t *m = broadcast_db_entry(all[i]);
        int32_t                  hz = (int32_t)(m->freq_khz * 1000 + 0.5);

        if (!cfg_is_valid_hw_freq(hz)) continue;        /* below or above what the radio tunes */
        if (show != CAT_ALL && category(m) != show) continue;
        rows[rows_n++] = all[i];
    }
    /* The selection: the same entry if it is still there, else the one nearest the dial */
    sel = -1;
    for (int i = 0; i < rows_n && keep != UINT32_MAX; i++)
        if (rows[i] == keep) sel = i;
    if (sel < 0) {
        sel = 0;
        for (int i = 0; i < rows_n; i++)
            if (broadcast_db_entry(rows[i])->freq_khz * 1000 <= f) sel = i;
    }
}

static void draw(void) {
    char      s[300];
    time_t    now = time(NULL);
    struct tm utc;

    gmtime_r(&now, &utc);
    snprintf(s, sizeof(s), "ON THE AIR  %02d:%02d UTC   %d station%s, %s, %s", utc.tm_hour, utc.tm_min, rows_n,
             rows_n == 1 ? "" : "s", CAT_NAMES[show], near_dial ? "near the dial" : "all of HF and MW");
    lv_label_set_text(head_l, s);

    if (sel < top) top = sel;
    if (sel >= top + SHOW) top = sel - SHOW + 1;
    if (top > rows_n - SHOW) top = rows_n - SHOW;
    if (top < 0) top = 0;

    for (int r = 0; r < SHOW; r++) {
        int i = top + r;

        if (i >= rows_n) {
            lv_obj_add_flag(row_obj[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const broadcast_match_t *m = broadcast_db_entry(rows[i]);
        bool                     on = i == sel;
        lv_color_t               c = on ? TC(panel) : TC(text);

        lv_obj_clear_flag(row_obj[r], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(row_obj[r], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        snprintf(s, sizeof(s), "%g", m->freq_khz);
        lv_label_set_text(row_l[r][0], s);
        lv_label_set_text(row_l[r][1], m->station);
        lv_label_set_text(row_l[r][2], m->language[0] ? m->language : "-");
        lv_label_set_text(row_l[r][3], m->target[0] ? m->target : "");
        for (int k = 0; k < 4; k++) lv_obj_set_style_text_color(row_l[r][k], k == 2 || k == 3 ? (on ? c : TC(text_dim)) : c, 0);
    }

    if (rows_n == 0) {
        lv_label_set_text(detail_l, near_dial ? "Nothing on the air near the dial by the list. Range: All shows the rest."
                                              : "Nothing on the air by the list (Settings > Info > Broadcast list).");
        return;
    }
    const broadcast_match_t *m = broadcast_db_entry(rows[sel]);
    static const char *const kind[CAT_N] = {"", "Broadcast", "Utility", "Numbers"};

    snprintf(s, sizeof(s), "%s, %s. %04d-%04d UTC%s%s. %s%s%s%s\nMFK: move, press: tune (in %s).", m->station,
             kind[category(m)], m->start_minute / 60 * 100 + m->start_minute % 60,
             m->stop_minute / 60 * 100 + m->stop_minute % 60, m->days[0] ? ", " : "", m->days,
             m->itu[0] ? "From " : "", m->itu, m->target[0] ? " to " : "", m->target,
             mode_of(m) == radio_mode_am ? "AM" : mode_of(m) == radio_mode_cw ? "CW" : "USB");
    lv_label_set_text(detail_l, s);
}

static void tune_sel(void) {
    if (sel < 0 || sel >= rows_n) return;
    const broadcast_match_t *m = broadcast_db_entry(rows[sel]);
    int32_t                  hz = (int32_t)(m->freq_khz * 1000 + 0.5);

    if (radio_get_state() != RADIO_RX) {
        msg_update_text_fmt("Not while transmitting");
        return;
    }
    main_screen_set_freq(hz);                   /* first: another band brings its own mode */
    cparam_i_set(cfg_cur_mode, mode_of(m));
}

static void follow_timer_cb(lv_timer_t *t) {
    (void)t;
    follow_timer = NULL;
    tune_sel();
}

static void move(int d) {
    if (!rows_n) return;
    sel += d;
    if (sel < 0) sel = 0;
    if (sel >= rows_n) sel = rows_n - 1;
    draw();
    if (follow) {           /* tuned once the knob rests a moment */
        if (follow_timer) lv_timer_reset(follow_timer);
        else {
            follow_timer = lv_timer_create(follow_timer_cb, 350, NULL);
            lv_timer_set_repeat_count(follow_timer, 1);
        }
    }
}

static void minute_cb(lv_timer_t *t) {
    (void)t;
    build();
    draw();
}

/* ---- The buttons ---- */

static const char *show_label(void) {
    static char s[24];

    snprintf(s, sizeof(s), "Show:\n%s", CAT_NAMES[show]);
    return s;
}

static const char *range_label(void) {
    return near_dial ? "Range:\nNear dial" : "Range:\nAll";
}

static const char *follow_label(void) {
    return follow ? "Tune as\nyou go: On" : "Tune as\nyou go: Off";
}

static void show_cb(button_data_t *b) {
    show = (show + 1) % CAT_N;
    build();
    draw();
    buttons_refresh(b);
}

static void show_hold_cb(button_data_t *b) {
    show = (show + CAT_N - 1) % CAT_N;
    build();
    draw();
    buttons_refresh(b);
}

static void range_cb(button_data_t *b) {
    near_dial = !near_dial;
    build();
    draw();
    buttons_refresh(b);
}

static void follow_cb(button_data_t *b) {
    follow = !follow;
    buttons_refresh(b);
    if (follow) tune_sel();
}

static void tune_cb(button_data_t *b) {
    (void)b;
    tune_sel();
}

/* Tuned, then the radio's frequency and mode kept as a channel under the station's name */
static void save_cb(button_data_t *b) {
    (void)b;
    if (sel < 0 || sel >= rows_n) return;
    const broadcast_match_t *m = broadcast_db_entry(rows[sel]);
    char                     name[CHANNEL_NAME_MAX];

    tune_sel();
    snprintf(name, sizeof(name), "%s", m->station);
    if (!channels_add_current() || !channels_set_name(channels_count() - 1, name)) {
        msg_update_text_fmt("Not saved: the channel list is full or can't be written");
        return;
    }
    msg_update_text_fmt("Saved as a channel: %s", name);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case LV_KEY_LEFT:
            move(-1);
            break;
        case LV_KEY_RIGHT:
            move(1);
            break;
        case LV_KEY_ENTER:
            tune_sel();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                       lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, TC(text), 0);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

/* MFK press: the encoder sends a click, never ENTER (mfk_device.c: the keypad's press sets the encoder's pressed),
 * taken here as ENTER, a keyboard's Enter (orch onair, 2026-11-28: On the Air's press tuned nothing) */
static void click_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();
    uint32_t    key   = LV_KEY_ENTER;

    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER)
        lv_event_send(lv_event_get_target(e), LV_EVENT_KEY, &key);
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    buttons_load_page(&page);

    head_l = label(dialog.obj, &sony_22, 16, 6, 760, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_color(head_l, TC(text_head), 0);
    for (int r = 0; r < SHOW; r++) {
        lv_obj_t *o = lv_obj_create(dialog.obj);

        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, 768, ROW_H - 2);
        lv_obj_set_pos(o, 8, ROWS_Y + r * ROW_H);
        lv_obj_set_style_bg_color(o, TC(accent), 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        row_obj[r] = o;
        row_l[r][0] = label(o, &sony_22, 4, 1, 86, LV_TEXT_ALIGN_RIGHT);
        row_l[r][1] = label(o, &sony_22, 104, 1, 430, LV_TEXT_ALIGN_LEFT);
        row_l[r][2] = label(o, &sony_20, 544, 2, 110, LV_TEXT_ALIGN_LEFT);
        row_l[r][3] = label(o, &sony_20, 660, 2, 104, LV_TEXT_ALIGN_LEFT);
    }
    detail_l = label(dialog.obj, &sony_20, 16, ROWS_Y + SHOW * ROW_H + 4, 760, LV_TEXT_ALIGN_LEFT);
    lv_label_set_long_mode(detail_l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(detail_l, TC(text_dim), 0);

    sel = -1;
    top = 0;
    build();
    draw();
    minute_timer = lv_timer_create(minute_cb, 60000, NULL);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(dialog.obj, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static void destruct_cb(void) {
    if (minute_timer) lv_timer_del(minute_timer);
    if (follow_timer) lv_timer_del(follow_timer);
    minute_timer = follow_timer = NULL;
    head_l = detail_l = NULL;
    lv_group_remove_obj(dialog.obj);
}
