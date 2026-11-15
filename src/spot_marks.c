/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "spot_marks.h"
#include "birdie.h"

#include "channel_list.h"
#include "freq_strip.h"

#include "board.h"
#include "cluster/client.h"
#include "cluster_column.h"
#include "dxcc.h"
#include "flags.h"
#include "cfg/cfg_api.h"
#include "styles.h"
#include "theme.h"
#include "fonts/font_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIDTH    BOARD_DISP_W
#define ROWS     2
#define ROW_H    (SPOT_MARKS_H / ROWS)
#define POOL     24
#define GAP      6      /* between labels of a row */
#define LINE_W   4      /* the line from a call to its frequency */
#define SPOT_COLOR theme_hex(TC_spot)
/* line height 17: fits ROW_H; VISION: the size of the other themes, its bigger one ran the two rows into each other */
#define SPOT_FONT  (*(theme_shape.vision ? font_down(&sony_22, VISION_FONT_STEPS) : &sony_22))

typedef struct {
    lv_obj_t *label;
    lv_obj_t *line;
} mark_t;

#define BIRDIE_POOL 24      /* the birdies' ticks at the top of the waterfall */
#define BIRDIE_H    9
static lv_obj_t      *bticks[BIRDIE_POOL];
static mark_t         marks[POOL];
static int32_t        shown_hz[POOL];   /* the spots drawn now, for the tuning snap */
static int            shown_n;
static bool           ready;
static lv_coord_t     line_y0, tag_y0;
static int32_t        span_center, span_half;
static cluster_spot_t spots[CLUSTER_MAX_SPOTS];
static int            spots_n;
static uint32_t       spots_gen;
static uint16_t       last_min = 0xFFFF;
static uint32_t       last_fgen;

static uint16_t utc_now_min(void) {
    time_t    now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);
    return (uint16_t)(tm.tm_hour * 60 + tm.tm_min);
}

/* The tags placed so far in this drawing: their rows' spans, the cluster column */
static lv_coord_t pl_end[ROWS][POOL], pl_start[ROWS][POOL];
static int        pl_cnt[ROWS];
static lv_area_t  pl_col;
static bool       pl_has_col;

/* One tag, text at hz (x on the screen) with its line, in the first row with room; marks[n] if it gets one. The
 * new n */
static int place(int n, lv_coord_t x, int32_t hz, const char *text, uint32_t color, bool inverted) {
    lv_coord_t w  = lv_txt_get_width(text, strlen(text), &SPOT_FONT, 0, LV_TEXT_FLAG_NONE) + 4;
    lv_coord_t lx = (x + w > WIDTH) ? x - w + 1 : x;  /* right-aligned at the right edge */
    int        row;

    if (pl_has_col && lx + w > pl_col.x1) {
        if (x < pl_col.x1) {
            lx = x - w + 1;     /* ends at its frequency, left of the column */
        } else {
            /* Under the column (which lists it): only the tick down to the column top */
            mark_t *m = &marks[n++];

            lv_obj_add_flag(m->label, LV_OBJ_FLAG_HIDDEN);
            shown_hz[n - 1] = hz;
            lv_obj_set_pos(m->line, x - LINE_W / 2, line_y0);
            lv_obj_set_height(m->line, LV_MAX(pl_col.y1 - line_y0, 1));
            lv_obj_set_style_bg_color(m->line, lv_color_hex(color), 0);
            lv_obj_clear_flag(m->line, LV_OBJ_FLAG_HIDDEN);
            return n;
        }
    }
    for (row = 0; row < ROWS; row++) {
        int k;
        for (k = 0; k < pl_cnt[row]; k++) {
            if (lx < pl_end[row][k] + GAP && lx + w + GAP > pl_start[row][k]) {
                break;
            }
        }
        if (k == pl_cnt[row]) {
            break;
        }
    }
    if (row == ROWS) {
        return n;
    }
    pl_start[row][pl_cnt[row]] = lx;
    pl_end[row][pl_cnt[row]++] = lx + w;

    mark_t *m = &marks[n++];

    lv_label_set_text(m->label, text);
    lv_obj_set_pos(m->label, lx, tag_y0 + row * ROW_H);
    lv_obj_set_style_text_color(m->label, lv_color_hex(color), 0);
    if (theme_shape.mono) {     /* 1bit: an alert call inverted, not in its own colour */
        lv_obj_set_style_bg_color(m->label, TC(text), 0);
        lv_obj_set_style_bg_opa(m->label, inverted ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    lv_obj_clear_flag(m->label, LV_OBJ_FLAG_HIDDEN);

    shown_hz[n - 1] = hz;
    /* From the frequency on the top bar down to its tag, either row */
    lv_obj_set_pos(m->line, x - LINE_W / 2, line_y0);
    lv_obj_set_height(m->line, tag_y0 + row * ROW_H - line_y0 + 2);
    lv_obj_set_style_bg_color(m->line, lv_color_hex(color), 0);
    lv_obj_clear_flag(m->line, LV_OBJ_FLAG_HIDDEN);
    return n;
}

static void redraw(void) {
    int      n = 0;
    int32_t  left = span_center - span_half;
    uint16_t now  = utc_now_min();
    const cluster_filter_t *flt = cluster_filter();

    /* The cluster column covers the calls' rows on the right: calls keep out of it */
    memset(pl_cnt, 0, sizeof(pl_cnt));
    pl_has_col = cluster_column_area(&pl_col) && tag_y0 + ROWS * ROW_H > pl_col.y1;
    last_fgen = flt->gen;

    /* The memory channels in the span, by name, in the OK colour (#203): first, so they keep their places */
    if (span_half > 0 && param_i_get(cfg_channel_marks)) {
        for (uint16_t i = 0; i < channels_count() && n < POOL; i++) {
            const channel_t *c  = channels_get(i);
            int64_t          dx = ((int64_t)c->freq - left) * WIDTH / (2 * span_half);

            if (dx < 0 || dx >= WIDTH) continue;
            n = place(n, (lv_coord_t)dx, (int32_t)c->freq, c->name[0] ? c->name : "CH", theme_hex(TC_ok), false);
        }
    }
    if (span_half > 0 && param_i_get(cfg_cluster_spots_wf)) {
        /* Newest first: they win the free places. Cyan on the waterfall; an alert call stays pink */
        for (int i = spots_n - 1; i >= 0 && n < POOL; i--) {
            const cluster_spot_t *s = &spots[i];
            int64_t   dx = (int64_t)(s->freq_hz - left) * WIDTH / (2 * span_half);

            if (dx < 0 || dx >= WIDTH || !cluster_filter_pass(flt, s, now)) {
                continue;
            }
            bool alert = cluster_alert_match(flt, s) >= 0;

            n = place(n, (lv_coord_t)dx, s->freq_hz, s->dx, alert ? theme_hex(TC_alert) : SPOT_COLOR, alert);
        }
    }
    shown_n = n;
    for (; n < POOL; n++) {
        lv_obj_add_flag(marks[n].label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(marks[n].line, LV_OBJ_FLAG_HIDDEN);
    }

    /* The birdies in the span: short ticks in the error colour, no tag */
    int b = 0;
    if (span_half > 0 && param_i_get(cfg_birdie_marks) > 0) {
        for (int i = 0; i < birdie_count() && b < BIRDIE_POOL; i++) {
            int64_t dx = (int64_t)(birdie_at(i) - left) * WIDTH / (2 * span_half);
            if (dx < 0 || dx >= WIDTH) continue;
            lv_obj_set_pos(bticks[b], (lv_coord_t)dx - LINE_W / 2, line_y0);
            lv_obj_set_style_bg_color(bticks[b], TC(err), 0);
            lv_obj_clear_flag(bticks[b], LV_OBJ_FLAG_HIDDEN);
            b++;
        }
    }
    for (; b < BIRDIE_POOL; b++) lv_obj_add_flag(bticks[b], LV_OBJ_FLAG_HIDDEN);
}

/* The channels as they are now, in one number: their file written on the web page shows at once */
static uint32_t channels_sum(void) {
    uint32_t h = 2166136261u;

    for (uint16_t i = 0; i < channels_count(); i++) {
        const channel_t *c = channels_get(i);

        h = (h ^ c->freq) * 16777619u;
        h = (h ^ (uint32_t)(c->mode | c->agc << 8 | c->pre << 16 | c->att << 17)) * 16777619u;
        for (const char *p = c->name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h ^ channels_count();
}

/* 1 Hz: new spots or a new minute (the colours age), or the channels changed */
static void timer_cb(lv_timer_t *t) {
    static uint32_t ch_sum;
    int      n   = cluster_client_snapshot(spots, CLUSTER_MAX_SPOTS, &spots_gen);
    uint16_t min = utc_now_min();
    bool     changed = n >= 0 || min != last_min || cluster_filter()->gen != last_fgen;

    channels_reload_if_changed();
    uint32_t sum = channels_sum();

    if (sum != ch_sum) {
        ch_sum  = sum;
        changed = true;
        freq_strip_channels_changed();      /* the tuned channel's name */
    }

    if (n >= 0) {
        spots_n = n;
    }
    if (changed) {
        last_min = min;
        redraw();
    }
}

/* The call drawn 1 px around in the panel colour (black, white in Outdoor) before the label
 * draws it in colour */
static void outline_cb(lv_event_t *e) {
    static const int8_t off[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    lv_obj_t           *l = lv_event_get_target(e);
    lv_draw_label_dsc_t dsc;
    lv_area_t           a;

    if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, 1);
        return;
    }
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_BEGIN) {
        return;
    }
    lv_draw_label_dsc_init(&dsc);
    lv_obj_init_draw_label_dsc(l, LV_PART_MAIN, &dsc);
    dsc.color = TC(panel);
    for (int i = 0; i < 8; i++) {
        lv_obj_get_content_coords(l, &a);
        lv_area_move(&a, off[i][0], off[i][1]);
        lv_draw_label(lv_event_get_draw_ctx(e), &dsc, &a, lv_label_get_text(l), NULL);
    }
}

void spot_marks_layout(lv_coord_t line_y, lv_coord_t tag_y) {
    line_y0 = line_y;
    tag_y0  = tag_y;
    if (ready) {
        redraw();
    }
}

void spot_marks_init(lv_obj_t *parent, lv_coord_t line_y, lv_coord_t tag_y) {
    line_y0 = line_y;
    tag_y0  = tag_y;

    /* All the lines first: every call is drawn over the lines that cross its row */
    for (int i = 0; i < POOL; i++) {
        lv_obj_t *l = lv_obj_create(parent);
        lv_obj_remove_style_all(l);
        lv_obj_set_width(l, LINE_W);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        marks[i].line = l;
    }
    for (int i = 0; i < BIRDIE_POOL; i++) {
        lv_obj_t *t = lv_obj_create(parent);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, LINE_W, BIRDIE_H);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
        bticks[i] = t;
    }
    for (int i = 0; i < POOL; i++) {
        lv_obj_t *l = lv_label_create(parent);
        lv_obj_set_style_text_font(l, &SPOT_FONT, 0);
        lv_obj_set_style_pad_hor(l, 2, 0);
        /* Readable on the waterfall in every theme: an outline */
        lv_obj_add_event_cb(l, outline_cb, LV_EVENT_ALL, NULL);
        lv_obj_refresh_ext_draw_size(l);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        marks[i].label = l;
    }
    /* Again on a theme change: drawn by spot_marks_layout(), once the cluster column is there too */
    if (!ready) {
        lv_timer_create(timer_cb, 1000, NULL);
        ready = true;
        timer_cb(NULL);
    }
}

bool spot_marks_crossed(int32_t from, int32_t to, int32_t *spot_hz) {
    bool found = false;

    for (int i = 0; i < shown_n; i++) {
        int32_t f = shown_hz[i];
        bool    past = to > from ? (f > from && f <= to) : (f < from && f >= to);

        /* The first one on the way */
        if (past && (!found || abs(f - from) < abs(*spot_hz - from))) {
            *spot_hz = f;
            found    = true;
        }
    }
    return found;
}

void spot_marks_update(int32_t center, int32_t half_width) {
    if (!ready || (center == span_center && half_width == span_half)) {
        return;
    }
    span_center = center;
    span_half   = half_width;
    redraw();
}

void spot_marks_refresh(void) {
    if (ready) redraw();
}
