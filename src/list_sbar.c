/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - a list's scroll bar (list_sbar.h)
 */
#include "list_sbar.h"

#include <stdint.h>
#include <stdlib.h>

struct list_sbar_s {
    lv_obj_t *track, *thumb;
    int       total, shown, top;
    void    (*pick)(int i);
};

static void drag_cb(lv_event_t *e) {
    list_sbar_t *b  = lv_event_get_user_data(e);
    lv_indev_t  *in = lv_indev_get_act();
    lv_point_t   pt;
    lv_area_t    a;

    if (!b || !in || b->total <= b->shown || !b->pick) return;
    lv_indev_get_point(in, &pt);
    lv_obj_get_coords(b->track, &a);
    int h = lv_area_get_height(&a), i = h > 1 ? (int)((int64_t)(pt.y - a.y1) * b->total / h) : 0;

    if (i < 0) i = 0;
    if (i >= b->total) i = b->total - 1;
    b->pick(i);
}

list_sbar_t *list_sbar_new(lv_obj_t *parent, int x, int y, int h, lv_color_t c, void (*pick)(int i)) {
    list_sbar_t *b = calloc(1, sizeof(*b));

    if (!b) return NULL;
    b->track = lv_obj_create(parent);
    lv_obj_remove_style_all(b->track);
    lv_obj_set_pos(b->track, x, y);
    lv_obj_set_size(b->track, LIST_SBAR_W, h);
    lv_obj_set_style_bg_color(b->track, c, 0);
    lv_obj_set_style_bg_opa(b->track, LV_OPA_20, 0);
    lv_obj_set_style_radius(b->track, 2, 0);
    lv_obj_set_ext_click_area(b->track, 6);         /* a mouse or a finger finds it */
    lv_obj_add_flag(b->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b->track, drag_cb, LV_EVENT_PRESSED, b);
    lv_obj_add_event_cb(b->track, drag_cb, LV_EVENT_PRESSING, b);
    b->thumb = lv_obj_create(b->track);
    lv_obj_remove_style_all(b->thumb);
    lv_obj_set_style_bg_color(b->thumb, c, 0);
    lv_obj_set_style_bg_opa(b->thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b->thumb, 2, 0);
    lv_obj_clear_flag(b->thumb, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    b->pick = pick;
    lv_obj_add_flag(b->track, LV_OBJ_FLAG_HIDDEN);
    return b;
}

void list_sbar_set(list_sbar_t *b, int total, int shown, int top) {
    if (!b || !b->track) return;
    b->total = total, b->shown = shown, b->top = top;
    if (total <= shown || shown <= 0) {
        lv_obj_add_flag(b->track, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    int h = lv_obj_get_height(b->track), th = h * shown / total;

    if (th < 10) th = 10;
    if (top > total - shown) top = total - shown;
    if (top < 0) top = 0;
    lv_obj_set_size(b->thumb, LIST_SBAR_W, th);
    lv_obj_set_pos(b->thumb, 0, (h - th) * top / (total - shown));
    lv_obj_clear_flag(b->track, LV_OBJ_FLAG_HIDDEN);
}
