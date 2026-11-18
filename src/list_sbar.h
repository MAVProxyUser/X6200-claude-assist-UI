/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - a list's scroll bar (the owner, 2026-11-17: "a slight scroll bar indicator ... to help know the
 *  position in the overall list like a web browser normally does ... our mouse when active can click and drag here.
 *  any list box with too many items should handle the same way"): a thin track at the list's right, its thumb as long
 *  as the share of the list in sight and where that part is, shown only while the list holds more than it shows. A
 *  press or a drag on it (the mouse, a finger) calls pick with the item at that height, which the list chooses as its
 *  MFK would. Used by the Sentry's lists (D, C, E) and AIRSPACE's.
 */
#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIST_SBAR_W 5

typedef struct list_sbar_s list_sbar_t;

/* at x, y on parent, h high (the rows' height), in colour c; pick(i): the item at the height pressed or dragged to */
list_sbar_t *list_sbar_new(lv_obj_t *parent, int x, int y, int h, lv_color_t c, void (*pick)(int i));
/* total items, shown at once, the first shown; hidden when all are shown */
void         list_sbar_set(list_sbar_t *b, int total, int shown, int top);

#ifdef __cplusplus
}
#endif
