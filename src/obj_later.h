/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - an object deleted later, safely
 *
 *  lv_obj_del_async() in place: for a popup closed from one of its own buttons (still running), or from anything
 *  that may run just before its window closes. LVGL's own lv_obj_del_async() keeps the bare pointer until the next
 *  lv_timer_handler(); when the object went with its parent before that (the window closed in the same GUI loop),
 *  that delete ran on freed memory: the JS8 app's crashes of 2026-11-23 (docs/security/js8-crash-2026-11-23.md).
 */

#pragma once

#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Deletes obj at the next lv_timer_handler(), unless it is deleted before then some other way (its parent deleted,
 * the window closed): then nothing more happens to it. Asked twice, it is deleted once. NULL is ignored */
void obj_later_del(lv_obj_t *obj);

/* Objects waiting to be deleted (the test's) */
unsigned obj_later_count(void);

#ifdef __cplusplus
}
#endif
