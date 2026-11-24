/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the JS8 app's crash of 2026-11-23 (docs/security/js8-crash-2026-11-23.md), under
 *  AddressSanitizer, with the GUI's own LVGL 8.3 and lv_conf.h.
 *
 *  The bench sent "js8 stop" and "js8 close" 0.2 ms apart while the "Save to log?" popup that a received 73 opens
 *  was up. Both ran in one scheduler_work() (gui_main.c), before lv_timer_handler():
 *    js8_window_remote() -> popups_close_all() -> log_close() -> p3_popup_shut(): lv_obj_del_async(log_list), NULL
 *    A_JS8_CLOSE -> dialog_destruct() -> close_window() -> destruct_cb() (log_list is NULL: not deleted there)
 *                -> lv_obj_del(dialog.obj): the popup freed with its parent
 *    lv_timer_handler() -> the async delete -> lv_obj_del() on the freed popup
 *
 *    js8_close_uaf_test old    that sequence as the GUI had it: AddressSanitizer must stop it (heap-use-after-free)
 *    js8_close_uaf_test        the same with obj_later_del() (src/obj_later.c), and the helper's other cases
 */

#include "lvgl/lvgl.h"
#include "obj_later.h"

#include <stdio.h>
#include <string.h>

uint32_t lv_tick_mono_now; /* LVGL's clock (lv_tick_mono.h); gui_main.c's on the radio */

static int checks, failed;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failed++;                                                                                                  \
            printf("  FAIL: ");                                                                                        \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- A display for LVGL, drawing nowhere ---- */

static lv_disp_draw_buf_t draw_buf;
static lv_color_t         pixels[800 * 20];
static lv_disp_drv_t      disp_drv;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    (void)area;
    (void)px;
    lv_disp_flush_ready(drv);
}

static void lvgl_up(void) {
    lv_init();
    lv_disp_draw_buf_init(&draw_buf, pixels, NULL, sizeof(pixels) / sizeof(pixels[0]));
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = 800;
    disp_drv.ver_res  = 480;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = flush_cb;
    lv_disp_drv_register(&disp_drv);
}

/* One pass of the GUI loop's LVGL half: its clock on, then its timers (the async deletes among them) */
static void loop_once(void) {
    lv_tick_mono_now += 40;
    lv_timer_handler();
}

/* ---- The JS8 window's pieces, as js8_window.c has them ---- */

static lv_obj_t *dialog_obj; /* dialog.obj: made by construct_cb (dialog_init), deleted by close_window() */
static lv_obj_t *log_list;   /* the "Save to log?" popup */
static int       popup_deleted;

static void count_delete_cb(lv_event_t *e) {
    int *n = lv_event_get_user_data(e);
    (*n)++;
}

static void window_open(void) {
    dialog_obj = lv_obj_create(lv_scr_act());
    lv_obj_clear_flag(dialog_obj, LV_OBJ_FLAG_SCROLLABLE);
}

/* p3_popup_new(): an empty popup list over the waterfall, the window's child */
static lv_obj_t *p3_popup_new(lv_coord_t width) {
    lv_obj_t *p = lv_list_create(dialog_obj);
    lv_obj_set_size(p, width, 300);
    lv_obj_align(p, LV_ALIGN_TOP_RIGHT, -20, 18);
    return p;
}

static void log_item_cb(lv_event_t *e);

/* log_offer() -> log_list_open(): a QSO ended with 73, the popup with its notes and items */
static void log_list_open(void) {
    log_list = p3_popup_new(560);
    lv_list_add_text(log_list, "Log Q1TST  20m  01:35-01:36 UTC");
    lv_list_add_text(log_list, "Sent -05  Rcvd -07  14.079 MHz  0 W");
    const char *items[] = {"Save to log", "Grid: (none)", "Name: (none)", "Comment: (none)", "Cancel"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_list_add_btn(log_list, NULL, items[i]);
        lv_obj_add_event_cb(b, log_item_cb, LV_EVENT_CLICKED, NULL);
    }
    popup_deleted = 0;
    lv_obj_add_event_cb(log_list, count_delete_cb, LV_EVENT_DELETE, &popup_deleted);
}

/* p3_popup_shut() before the fix, and after */
static void p3_popup_shut_old(lv_obj_t **popup) {
    if (*popup == NULL) return;
    lv_obj_del_async(*popup);
    *popup = NULL;
}

static void p3_popup_shut(lv_obj_t **popup) {
    if (*popup == NULL) return;
    obj_later_del(*popup);
    *popup = NULL;
}

/* Cancel in the popup: it closes itself from its own button, still running (why the delete is deferred) */
static void log_item_cb(lv_event_t *e) {
    (void)e;
    p3_popup_shut(&log_list);
}

/* close_window() (app_window.c): destruct_cb() deletes the popups still open, then the window's object goes */
static void close_window(void) {
    lv_obj_t **popups[] = {&log_list};
    for (size_t i = 0; i < sizeof(popups) / sizeof(popups[0]); i++) {
        if (!*popups[i]) continue;
        lv_obj_del(*popups[i]);
        *popups[i] = NULL;
    }
    lv_obj_del(dialog_obj);
    dialog_obj = NULL;
}

/* ---- The cases ---- */

/* The bench's "js8 stop" and "js8 close" in one scheduler pass, as before the fix */
static int bench_sequence_old(void) {
    window_open();
    log_list_open();
    loop_once(); /* drawn */
    printf("old: popup shut with lv_obj_del_async(), the window closed before LVGL's timers run\n");
    fflush(stdout);
    p3_popup_shut_old(&log_list); /* js8 stop */
    close_window();               /* js8 close */
    loop_once();                  /* the async delete: AddressSanitizer stops here */
    printf("old: not stopped: the use after free went unseen\n");
    return 1;
}

static void case_bench_sequence(void) {
    window_open();
    log_list_open();
    loop_once();
    p3_popup_shut(&log_list); /* js8 stop */
    CHECK(obj_later_count() == 1, "the popup waits to be deleted (%u waiting)", obj_later_count());
    close_window(); /* js8 close: the popup goes with the window */
    CHECK(popup_deleted == 1, "the popup was deleted with its window (%d deletes)", popup_deleted);
    CHECK(obj_later_count() == 0, "nothing waits once the window is gone (%u waiting)", obj_later_count());
    loop_once(); /* the deferred delete: nothing left to do */
    CHECK(popup_deleted == 1, "deleted once (%d deletes)", popup_deleted);
    printf("ok: js8 stop + js8 close in one pass, the popup deleted once with its window\n");
}

static void case_deferred(void) {
    window_open();
    log_list_open();
    loop_once();
    lv_obj_t *popup = log_list;
    p3_popup_shut(&log_list);
    CHECK(popup_deleted == 0 && lv_obj_is_valid(popup), "not deleted before LVGL's timers run");
    loop_once();
    CHECK(popup_deleted == 1, "deleted at the next lv_timer_handler (%d deletes)", popup_deleted);
    CHECK(lv_obj_get_child_cnt(dialog_obj) == 0, "the window has no popup left");
    loop_once();
    CHECK(popup_deleted == 1, "deleted once (%d deletes)", popup_deleted);
    close_window();
    printf("ok: a popup shut alone goes at the next timer pass, as with lv_obj_del_async\n");
}

static void case_own_button(void) {
    window_open();
    log_list_open();
    loop_once();
    lv_obj_t *cancel = lv_obj_get_child(log_list, -1);
    lv_event_send(cancel, LV_EVENT_CLICKED, NULL); /* Cancel: log_item_cb shuts the popup under its own button */
    CHECK(log_list == NULL && popup_deleted == 0, "the popup outlives its own button's event");
    close_window(); /* and the window closes in the same pass */
    loop_once();
    CHECK(popup_deleted == 1, "deleted once (%d deletes)", popup_deleted);
    printf("ok: shut from its own button, then the window closed in the same pass\n");
}

static void case_twice(void) {
    window_open();
    log_list_open();
    lv_obj_t *popup = log_list;
    obj_later_del(popup);
    obj_later_del(popup);
    CHECK(obj_later_count() == 1, "asked twice, waiting once (%u)", obj_later_count());
    loop_once();
    CHECK(popup_deleted == 1, "deleted once (%d deletes)", popup_deleted);
    log_list = NULL;
    close_window();
    printf("ok: asked twice, deleted once\n");
}

/* A parent and its child both waiting, either order: the child is deleted once, with its parent or alone */
static void case_nested(void) {
    for (int order = 0; order < 2; order++) {
        int       parent_n = 0, child_n = 0;
        lv_obj_t *parent = lv_obj_create(lv_scr_act());
        lv_obj_t *child  = lv_obj_create(parent);
        lv_obj_add_event_cb(parent, count_delete_cb, LV_EVENT_DELETE, &parent_n);
        lv_obj_add_event_cb(child, count_delete_cb, LV_EVENT_DELETE, &child_n);
        obj_later_del(order ? child : parent);
        obj_later_del(order ? parent : child);
        loop_once();
        CHECK(parent_n == 1 && child_n == 1, "order %d: parent %d, child %d deletes", order, parent_n, child_n);
        CHECK(obj_later_count() == 0, "order %d: nothing left waiting (%u)", order, obj_later_count());
    }
    printf("ok: a parent and its child both waiting\n");
}

/* The window closed with a popup waiting, the next window opened: what LVGL now has at the freed popup's address
 * (the Sentry's page G on the radio) is never deleted by the delete that waited */
static void case_reused_memory(void) {
    window_open();
    log_list_open();
    p3_popup_shut(&log_list);
    close_window();
    int       next_n = 0;
    lv_obj_t *next[16];
    for (int i = 0; i < 16; i++) {
        next[i] = lv_obj_create(lv_scr_act());
        lv_obj_add_event_cb(next[i], count_delete_cb, LV_EVENT_DELETE, &next_n);
    }
    loop_once();
    CHECK(next_n == 0, "objects made after the close left alone (%d deleted)", next_n);
    for (int i = 0; i < 16; i++) lv_obj_del(next[i]);
    printf("ok: the next window's objects are left alone\n");
}

/* More waiting at once than the list holds: each still deleted once */
static void case_many(void) {
    enum { N = 40 };
    int       n = 0;
    lv_obj_t *o[N];
    window_open();
    for (int i = 0; i < N; i++) {
        o[i] = lv_obj_create(dialog_obj);
        lv_obj_add_event_cb(o[i], count_delete_cb, LV_EVENT_DELETE, &n);
        obj_later_del(o[i]);
    }
    loop_once();
    loop_once();
    CHECK(n == N, "%d objects waiting, %d deleted", N, n);
    CHECK(obj_later_count() == 0, "nothing left waiting (%u)", obj_later_count());
    close_window();
    printf("ok: %d waiting at once\n", N);
}

int main(int argc, char **argv) {
    lvgl_up();
    if (argc > 1 && !strcmp(argv[1], "old")) return bench_sequence_old();

    case_bench_sequence();
    case_deferred();
    case_own_button();
    case_twice();
    case_nested();
    case_reused_memory();
    case_many();
    obj_later_del(NULL);
    printf("%d of %d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
