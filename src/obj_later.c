/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - an object deleted later, safely (obj_later.h)
 *
 *  The objects wait in a list. Each gets a DELETE event handler that takes it off the list, so an object deleted
 *  some other way first (with its window) is forgotten at that moment and the deferred delete never sees its freed
 *  memory. The deferred delete itself is our own async call, which only reads the list.
 */

#include "obj_later.h"

#define OBJ_LATER_MAX 32 /* waiting at once: a window has a dozen popups at most */

static lv_obj_t *later[OBJ_LATER_MAX];
static bool      queued; /* later_run() waits in LVGL's timers */

static void forget(lv_obj_t *obj) {
    for (int i = 0; i < OBJ_LATER_MAX; i++)
        if (later[i] == obj) later[i] = NULL;
}

/* DELETE of a waiting object, whoever deletes it */
static void deleted_cb(lv_event_t *e) {
    forget(lv_event_get_target(e));
}

static void later_run(void *unused) {
    (void)unused;
    queued = false;
    for (int i = 0; i < OBJ_LATER_MAX; i++) {
        lv_obj_t *obj = later[i];
        if (obj) lv_obj_del(obj); /* deleted_cb takes it, and any child also waiting, off the list */
    }
}

void obj_later_del(lv_obj_t *obj) {
    int free_at = -1;

    if (obj == NULL) return;
    for (int i = 0; i < OBJ_LATER_MAX; i++) {
        if (later[i] == obj) return; /* waiting already */
        if (later[i] == NULL && free_at < 0) free_at = i;
    }
    if (free_at < 0) { /* not seen in practice: LVGL's way, unsafe across a window's close as it always was */
        static bool said;
        if (!said) LV_LOG_WARN("obj_later: over %d waiting, the rest deleted LVGL's way", OBJ_LATER_MAX);
        said = true;
        lv_obj_del_async(obj);
        return;
    }
    later[free_at] = obj;
    lv_obj_add_event_cb(obj, deleted_cb, LV_EVENT_DELETE, NULL);
    if (!queued) queued = lv_async_call(later_run, NULL) == LV_RES_OK;
}

unsigned obj_later_count(void) {
    unsigned n = 0;
    for (int i = 0; i < OBJ_LATER_MAX; i++)
        if (later[i]) n++;
    return n;
}
