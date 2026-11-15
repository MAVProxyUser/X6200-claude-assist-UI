/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  This fork's releases, as the daily check found them (update_watch.h).
 *
 *  A newer one is fetched in the background (x6100-update download) and then asked about once, in a box over
 *  whatever is on the screen (as tci_ask.c asks): its version, the commit it was built from and the bundle's MD5,
 *  "Apply it?" Yes or No. No is chosen when it opens; the MFK turns the choice and its press confirms, the keyboard's
 *  arrows and Enter do the same, Esc is No, a touch takes a choice. Yes installs it (x6100-update install: the radio's
 *  software restarts), what it is doing shown meanwhile in a box with a bar; No leaves it for Settings > Info > Update.
 *  The boxes fit a screen turned to portrait (the Sentry's G). A release that wants a newer card image is only
 *  said, as before: the bundle alone cannot bring it.
 */
#include "update_watch.h"

#include "board.h"
#include "btaudio.h"        /* sh_run */
#include "fonts/font_select.h"
#include "gui_events.h"
#include "gui_styles.h"
#include "key_input.h"
#include "msg.h"
#include "radio.h"
#include "screen_light.h"
#include "theme.h"

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_JSON   BOARD_DATA "/update/check.json"     /* x6100-update check, from S99updatecheck */
#define ANNOUNCED    BOARD_DATA "/update/announced"      /* the version it was said for */
#define REPO_FILE    BOARD_SHARE "/update-repo"
#define APP_VERSION  BOARD_SHARE "/app-version"
#define DEFAULT_REPO "MAVProxyUser/X6200-claude-assist-UI" /* x6100-update's own default */
#define JSON_MAX     32768                                /* the release notes are in it too, up to 2000 chars */
#define UPD_CTL      "/usr/sbin/x6100-update"
#define BUNDLE       BOARD_DATA "/update/bundle.tar.gz"   /* where x6100-update download puts it */
#define PROGRESS     BOARD_DATA "/update/.progress"       /* x6100-update's step: "<step> <done> <of>" */
#define VF(n)        (theme_shape.vision ? font_down(&sony_##n, VISION_FONT_STEPS - 1) : &sony_##n)

const char *update_repo(void) {
    static char repo[128];
    FILE       *f;

    if (repo[0]) return repo;
    if ((f = fopen(REPO_FILE, "r"))) {
        if (fgets(repo, sizeof(repo), f)) repo[strcspn(repo, " \t\r\n")] = '\0';
        fclose(f);
    }
    if (!repo[0] || !strchr(repo, '/')) snprintf(repo, sizeof(repo), "%s", DEFAULT_REPO);
    return repo;
}

/* x6100-update prints its JSON with Python's default separators: "key": "value", "key": true */
static void json_str(const char *json, const char *key, char *out, size_t n) {
    char        pat[64];
    const char *p;

    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    if ((p = strstr(json, pat))) {
        p += strlen(pat);
        size_t l = strcspn(p, "\"");
        snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), p);
    }
}

static bool json_true(const char *json, const char *key) {
    char        pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\": ", key);
    return (p = strstr(json, pat)) && !strncmp(p + strlen(pat), "true", 4);
}

/* The newer release the last check found, for the software running now: its version, bundle URL and commit (either
 * may be NULL); false when none (or the check was made for another version: one installed since) */
static bool newer_release_ex(char *latest, size_t n, bool *image, char *url, size_t un, char *commit, size_t cn) {
    char  *json = malloc(JSON_MAX), running[32], now[32] = "";
    FILE  *f;
    size_t r = 0;
    bool   newer = false;

    latest[0] = '\0';
    *image    = false;
    if (url) url[0] = '\0';
    if (commit) commit[0] = '\0';
    if (!json) return false;
    if ((f = fopen(CHECK_JSON, "r"))) {
        r = fread(json, 1, JSON_MAX - 1, f);
        fclose(f);
    }
    json[r] = '\0';
    if ((f = fopen(APP_VERSION, "r"))) {
        if (fgets(now, sizeof(now), f)) now[strcspn(now, " \t\r\n")] = '\0';
        fclose(f);
    }
    json_str(json, "running", running, sizeof(running));
    if (r && json_true(json, "newer") && !strcmp(running, now)) {
        json_str(json, "latest", latest, n);
        *image = json_true(json, "image_needed");
        if (url) json_str(json, "url", url, un);
        if (commit) json_str(json, "commit", commit, cn);
        newer = latest[0] != '\0';
    }
    free(json);
    return newer;
}

static bool newer_release(char *latest, size_t n, bool *image) {
    return newer_release_ex(latest, n, image, NULL, 0, NULL, 0);
}

void update_watch_status(char *buf, size_t n) {
    char latest[24];
    bool image;

    buf[0] = '\0';
    if (newer_release(latest, sizeof(latest), &image))
        snprintf(buf, n, "%s out%s", latest, image ? ", new card image" : "");
}

/* The version answered (or said, for one that wants a card image): not asked about again */
static void announced_set(const char *v) {
    FILE *f = fopen(ANNOUNCED, "w");

    if (f) {
        fprintf(f, "%s\n", v);
        fclose(f);
    }
}

/* ---- The question ---- */

enum { UW_IDLE, UW_FETCHING, UW_ASKING, UW_INSTALLING };
enum { PICK_YES, PICK_NO, PICKS };

static int         uw = UW_IDLE;
static char        uw_ver[24], uw_commit[48], uw_md5[40];
static lv_obj_t   *box, *btn[PICKS];
static lv_obj_t   *back;            /* focused before the box took the keys */
static bool        back_editing, picked;
static int         sel = PICK_NO;

/* A box's width: 640 (700 VISION), narrower on a screen turned to portrait (the Sentry's G: 480 wide) */
static lv_coord_t box_w(void) {
    lv_coord_t w = theme_shape.vision ? 700 : 640, room = lv_disp_get_hor_res(NULL) - 24;

    return w < room ? w : room;
}

static void show_sel(void) {
    for (int i = 0; i < PICKS; i++) {
        bool on = i == sel;

        lv_obj_set_style_bg_color(btn[i], on ? TC(sel_bg) : TC(chip), 0);
        lv_obj_set_style_border_color(btn[i], on ? TC(sel_border) : TC(popup_border), 0);
        lv_obj_set_style_border_width(btn[i], on ? 2 : 1, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(btn[i], 0), on ? TC(sel_text) : TC(text), 0);
    }
}

/* The box gone, the keys back where they were if it still had them */
static void box_close(void) {
    if (!box) return;
    if (lv_group_get_focused(keyboard_group) == box && back && lv_obj_is_valid(back) &&
        lv_obj_get_group(back) == keyboard_group) {
        lv_group_focus_obj(back);
        lv_group_set_editing(keyboard_group, back_editing);
    }
    lv_obj_del(box);
    box = NULL;
    for (int i = 0; i < PICKS; i++) btn[i] = NULL;
    back = NULL;
}

/* ---- The install, after Yes: what it is doing, in a box with a bar (x6100-update's .progress, read 4 times a
 * second): the files checked against the release's checksums, then put in place one by one (the old ones kept), then
 * the restart. A failure is said there (nothing changed) and the box goes after 10 s or a key ---- */

static lv_obj_t   *pbox, *p_step, *p_bar, *p_note;
static lv_timer_t *p_timer;
static uint32_t    p_end;           /* a failure said: the box goes 10 s on */

static void prog_close(void) {
    if (p_timer) lv_timer_del(p_timer);
    p_timer = NULL;
    if (!pbox) return;
    if (lv_group_get_focused(keyboard_group) == pbox && back && lv_obj_is_valid(back) &&
        lv_obj_get_group(back) == keyboard_group) {
        lv_group_focus_obj(back);
        lv_group_set_editing(keyboard_group, back_editing);
    }
    lv_obj_del(pbox);
    pbox = NULL;
}

static void prog_close_async(void *arg) {
    (void)arg;
    prog_close();
}

static void prog_key_cb(lv_event_t *e) {
    (void)e;
    if (uw != UW_INSTALLING) lv_async_call(prog_close_async, NULL);     /* installing: the keys wait */
}

static void prog_timer_cb(lv_timer_t *t) {
    FILE *f;
    char  step[16] = "";
    long  got = 0, total = 0;
    int   pct = 0;
    char  s[96];

    (void)t;
    backlight_tick();               /* the screen stays lit while it works */
    if (uw != UW_INSTALLING) {
        if (p_end && lv_tick_elaps(p_end) > 10000) prog_close();
        return;
    }
    if (!(f = fopen(PROGRESS, "r"))) return;
    if (fscanf(f, "%15s %ld %ld", step, &got, &total) < 1) step[0] = '\0';
    fclose(f);
    if (!strcmp(step, "verify")) {
        pct = total > 0 ? (int)(45 * got / total) : 0;
        snprintf(s, sizeof(s), "Checking each file against the release's checksum: %ld of %ld", got, total);
    } else if (!strcmp(step, "install")) {
        pct = 45 + (total > 0 ? (int)(50 * got / total) : 0);
        snprintf(s, sizeof(s), "Putting the new files in place, the old ones kept: %ld of %ld", got, total);
    } else if (!strcmp(step, "done")) {
        pct = 100;
        snprintf(s, sizeof(s), "Installed. The radio's software restarts now");
    } else {
        snprintf(s, sizeof(s), "Unpacking the bundle...");
    }
    lv_label_set_text(p_step, s);
    lv_bar_set_value(p_bar, pct, LV_ANIM_OFF);
}

static void prog_open(void) {
    const lv_coord_t w = box_w();
    lv_obj_t        *obj;

    prog_close();
    backlight_tick();
    pbox = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pbox);
    lv_obj_add_style(pbox, &popup_style, 0);
    lv_obj_set_size(pbox, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(pbox, 16, 0);
    lv_obj_set_style_pad_row(pbox, 12, 0);
    lv_obj_set_flex_flow(pbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(pbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(pbox);

    obj = lv_label_create(pbox);
    lv_obj_set_style_text_font(obj, VF(18), 0);
    lv_obj_set_style_text_color(obj, TC(text_head), 0);
    lv_label_set_text(obj, "UPDATE FROM GITHUB");

    obj = lv_label_create(pbox);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(obj, VF(26), 0);
    lv_obj_set_style_text_color(obj, TC(text), 0);
    lv_label_set_text_fmt(obj, "Applying OpenX6X00 %s", uw_ver);

    p_step = lv_label_create(pbox);
    lv_obj_set_width(p_step, LV_PCT(100));
    lv_label_set_long_mode(p_step, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(p_step, VF(18), 0);
    lv_obj_set_style_text_color(p_step, TC(text), 0);
    lv_label_set_text(p_step, "Unpacking the bundle...");

    p_bar = lv_bar_create(pbox);
    lv_obj_set_size(p_bar, LV_PCT(100), 22);
    lv_bar_set_range(p_bar, 0, 100);
    lv_bar_set_value(p_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(p_bar, TC(chip), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(p_bar, TC(popup_border), LV_PART_MAIN);
    lv_obj_set_style_border_width(p_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p_bar, TC(sel_bg), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(p_bar, LV_OPA_COVER, LV_PART_INDICATOR);

    p_note = lv_label_create(pbox);
    lv_obj_set_width(p_note, LV_PCT(100));
    lv_label_set_long_mode(p_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(p_note, VF(16), 0);
    lv_obj_set_style_text_color(p_note, TC(text_dim), 0);
    lv_label_set_text(p_note, "Leave the radio on. If the power goes now, the version before comes back by itself at "
                              "the next start.");

    /* the keys wait while it installs (nothing on the screen under it moves) */
    back         = lv_group_get_focused(keyboard_group);
    back_editing = lv_group_get_editing(keyboard_group);
    lv_group_add_obj(keyboard_group, pbox);
    lv_obj_add_event_cb(pbox, prog_key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(pbox);
    lv_group_set_editing(keyboard_group, true);
    p_end   = 0;
    p_timer = lv_timer_create(prog_timer_cb, 250, NULL);
}

static void install_done(const char *out, int status, void *user) {
    (void)status;
    (void)user;
    uw = UW_IDLE;           /* "ok": the software restarts, this GUI with it; anything else is why not */
    if (strncmp(out, "ok", 2) == 0) {
        if (pbox) {
            lv_label_set_text(p_step, "Installed. The radio's software restarts now");
            lv_bar_set_value(p_bar, 100, LV_ANIM_OFF);
        }
        return;
    }
    msg_schedule_long_text_fmt("OpenX6X00 %s not applied: %.80s. Nothing changed.", uw_ver,
                               strncmp(out, "error", 5) == 0 ? out + 6 : out);
    if (pbox) {
        lv_label_set_text_fmt(p_step, "Not applied: %.80s", strncmp(out, "error", 5) == 0 ? out + 6 : out);
        lv_label_set_text(p_note, "Nothing changed: the version running stays. Settings > Info > Update tries again. "
                                  "A key closes this.");
        p_end = lv_tick_get() | 1;
    }
}

static void pick_async(void *arg) {
    (void)arg;
    if (!box) return;
    box_close();
    announced_set(uw_ver);
    if (sel == PICK_YES) {
        char cmd[96];

        uw = UW_INSTALLING;
        prog_open();                    /* what it is doing, step by step, till the restart */
        snprintf(cmd, sizeof(cmd), UPD_CTL " install " BUNDLE);
        sh_run(cmd, install_done, NULL);
    } else {
        uw = UW_IDLE;
        msg_schedule_long_text_fmt("OpenX6X00 %s not applied: Settings > Info > Update installs it", uw_ver);
    }
}

static void pick(int i) {
    lv_indev_t *indev = lv_indev_get_act();

    if (picked) return;
    picked = true;
    sel    = i;
    show_sel();
    if (indev) lv_indev_wait_release(indev);   /* the key that answered is still down (tci_ask.c) */
    lv_async_call(pick_async, NULL);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *(uint32_t *)lv_event_get_param(e);

    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            if (!picked && sel > 0) {
                sel--;
                show_sel();
            }
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            if (!picked && sel < PICKS - 1) {
                sel++;
                show_sel();
            }
            break;
        case LV_KEY_ENTER: pick(sel); break;
        case LV_KEY_ESC: pick(PICK_NO); break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT: radio_change_vol(-1); break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT: radio_change_vol(1); break;
        default: break;
    }
}

/* The MFK's press on the box (an encoder sends a click, not Enter) */
static void box_click_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_get_act();

    (void)e;
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) pick(sel);
}

static void btn_click_cb(lv_event_t *e) {
    pick((int)(intptr_t)lv_event_get_user_data(e));
}

static void ask(void) {
    static const char *const names[PICKS] = {"Yes, apply it", "No"};
    const lv_coord_t         w            = box_w();
    lv_obj_t                *obj, *row;

    box_close();
    backlight_tick();       /* the screen lit: the question is seen */
    uw     = UW_ASKING;
    sel    = PICK_NO;       /* a press meant for something else applies nothing */
    picked = false;

    box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &popup_style, 0);
    lv_obj_set_size(box, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(box);

    obj = lv_label_create(box);
    lv_obj_set_style_text_font(obj, VF(18), 0);
    lv_obj_set_style_text_color(obj, TC(text_head), 0);
    lv_label_set_text(obj, "UPDATE FROM GITHUB");

    obj = lv_label_create(box);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(obj, VF(26), 0);
    lv_obj_set_style_text_color(obj, TC(text), 0);
    lv_label_set_text_fmt(obj, "Apply OpenX6X00 %s?", uw_ver);

    obj = lv_label_create(box);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(obj, VF(18), 0);
    lv_obj_set_style_text_color(obj, TC(text), 0);
    lv_label_set_text_fmt(obj, "Version  %s\nCommit   %s\nMD5      %s\n%s", uw_ver,
                          uw_commit[0] ? uw_commit : "(GitHub did not say)", uw_md5, update_repo());

    row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < PICKS; i++) {
        btn[i] = lv_obj_create(row);
        lv_obj_remove_style_all(btn[i]);
        lv_obj_set_size(btn[i], (w - 32 - 16) / PICKS, 52);
        lv_obj_set_style_bg_opa(btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn[i], theme_shape.radius_popup, 0);
        lv_obj_add_flag(btn[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn[i], btn_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        obj = lv_label_create(btn[i]);
        lv_obj_set_style_text_font(obj, VF(22), 0);
        lv_label_set_text(obj, names[i]);
        lv_obj_center(obj);
    }

    obj = lv_label_create(box);
    lv_obj_set_style_text_font(obj, VF(16), 0);
    lv_obj_set_style_text_color(obj, TC(text_dim), 0);
    lv_label_set_text(obj, "MFK: choose, press: confirm. Yes restarts the radio's software");
    show_sel();

    back         = lv_group_get_focused(keyboard_group);
    back_editing = lv_group_get_editing(keyboard_group);
    lv_group_add_obj(keyboard_group, box);
    lv_obj_add_event_cb(box, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(box, box_click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(box);
    lv_group_set_editing(keyboard_group, true);
}

/* The download's end: "<path>" then md5sum's "<md5>  <path>"; anything else, another try at the next look */
static void fetched(const char *out, int status, void *user) {
    const char *p = out;

    (void)user;
    uw_md5[0] = '\0';
    for (; status == 0 && p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        int k = 0;

        while (k < 32 && ((p[k] >= '0' && p[k] <= '9') || (p[k] >= 'a' && p[k] <= 'f'))) k++;
        if (k == 32 && p[32] == ' ') {
            memcpy(uw_md5, p, 32);
            uw_md5[32] = '\0';
        }
    }
    if (!uw_md5[0]) {
        uw = UW_IDLE;
        return;
    }
    ask();
}

static void watch_cb(lv_timer_t *t) {
    char  latest[24], seen[24] = "", url[512];
    bool  image;
    FILE *f;

    lv_timer_set_period(t, 600000);
    if (uw != UW_IDLE) return;
    if (!newer_release_ex(latest, sizeof(latest), &image, url, sizeof(url), uw_commit, sizeof(uw_commit))) return;
    if ((f = fopen(ANNOUNCED, "r"))) {
        if (fgets(seen, sizeof(seen), f)) seen[strcspn(seen, "\r\n")] = '\0';
        fclose(f);
    }
    if (!strcmp(seen, latest)) return;
    if (image || strncmp(url, "https://", 8) != 0 || strpbrk(url, "'\\\"$` ")) {
        /* a card image wanted too (or no bundle to fetch): said, for Settings > Info > Update */
        msg_schedule_long_text_fmt("OpenX6X00 %s is out: Settings > Info > Update", latest);
        announced_set(latest);
        return;
    }
    char cmd[640];

    snprintf(uw_ver, sizeof(uw_ver), "%s", latest);
    snprintf(cmd, sizeof(cmd), UPD_CTL " download '%s' && md5sum " BUNDLE, url);
    uw = UW_FETCHING;
    sh_run(cmd, fetched, NULL);
}

void update_watch_init(void) {
    lv_timer_create(watch_cb, 60000, NULL);
}
