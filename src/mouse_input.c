/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - A USB or Bluetooth mouse or touchpad on the screen (mouse_input.h). Every input device with a
 *  left button and X and Y movement that no paddle has is opened (up to four: a touchpad and a mouse both point), at each device scan (key_input.c's), while
 *  Settings > Keys > KEYBOARD AND MOUSE > Mouse is on. One reader takes its events for three LVGL inputs: the
 *  pointer (left button, with an arrow on the top layer that goes after a few seconds still), an encoder (the
 *  wheel and the middle button, as the MFK) and a key (the right button, ESC). A press that lights a dark screen
 *  or ends the screensaver only does that. A click shorter than LVGL's read period still reaches it: a button
 *  pressed and let go between two reads counts as pressed at the next one.
 */

#include "mouse_input.h"

#include "cfg/cfg_api.h"
#include "input_caps.h"
#include "key_input.h"
#include "paddle_keyer.h"
#include "screen_light.h"

#include "lvgl/lvgl.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BITS(max)     ((max) / (8 * sizeof(long)) + 1)
#define HAS(bits, n)  ((bits)[(n) / (8 * sizeof(long))] & (1UL << ((n) % (8 * sizeof(long)))))
#define HIDE_MS       5000      /* the arrow goes after this long without a move or a press */

enum { B_LEFT, B_MIDDLE, B_RIGHT, B_N };

typedef struct {
    bool held;      /* down now */
    bool latched;   /* went down since its input last read it: counts as pressed once, however short */
    bool ignored;   /* its press only woke the screen: up until it is let go */
} button_t;

#define MICE 4                  /* open at once: a touchpad and a mouse both point */

static int            fds[MICE] = {-1, -1, -1, -1};
static dev_t          dev_rdev[MICE];
static ino_t          dev_ino[MICE];
static bool           started;          /* the settings are there */
static button_t       btn[B_N];
static int            wheel;            /* detents not read yet: + is toward the user, the MFK's next */
static lv_coord_t     px = 400, py = 240;
static lv_coord_t     press_x, press_y; /* where the left button went down */
static uint32_t       active;           /* lv_tick of the last move or press */
static lv_indev_drv_t ptr_drv, wheel_drv, back_drv;
static lv_obj_t      *arrow;

/* The arrow: X outline, O fill, the tip at the top left (the point) */
#define ARROW_W 11
#define ARROW_H 17
static const char *const shape[ARROW_H] = {
    "X          ", "XX         ", "XOX        ", "XOOX       ", "XOOOX      ", "XOOOOX     ",
    "XOOOOOX    ", "XOOOOOOX   ", "XOOOOOOOX  ", "XOOOOOOOOX ", "XOOOOOXXXXX", "XOOXOOX    ",
    "XOX XOOX   ", "XX  XOOX   ", "X    XOOX  ", "     XOOX  ", "      XX   ",
};
static uint8_t      arrow_px[ARROW_W * ARROW_H * LV_IMG_PX_SIZE_ALPHA_BYTE];
static lv_img_dsc_t arrow_img;

static bool mouse_on(void) {
    return started && cfg_mouse && param_i_get(cfg_mouse);
}

static void arrow_hide(void) {
    if (arrow) {
        lv_obj_add_flag(arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

static void arrow_show(void) {
    active = lv_tick_get();
    if (arrow && lv_obj_has_flag(arrow, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool any_open(void) {
    for (int i = 0; i < MICE; i++)
        if (fds[i] >= 0) return true;
    return false;
}

/* One let go (gone, or no longer a mouse); the buttons up when it was the last */
static void close_one(int i) {
    if (fds[i] < 0) return;
    close(fds[i]);
    fds[i] = -1;
    LV_LOG_USER("Mouse %d closed", i);
    if (!any_open()) {
        wheel = 0;
        for (int b = 0; b < B_N; b++) btn[b] = (button_t){0};
        arrow_hide();
    }
}

static void close_mouse(void) {
    for (int i = 0; i < MICE; i++) close_one(i);
}

/* Faster moves go further: one count a pixel when slow, up to three */
static lv_coord_t speed(int d) {
    int a = d < 0 ? -d : d;

    return (lv_coord_t)(a < 4 ? d : a < 12 ? d * 2 : d * 3);
}

static void take(const struct input_event *e) {
    int b;

    if (e->type == EV_REL) {
        if (e->code == REL_X || e->code == REL_Y) {
            lv_coord_t w = lv_disp_get_hor_res(NULL), h = lv_disp_get_ver_res(NULL);

            if (e->code == REL_X) {
                px = LV_CLAMP(0, px + speed(e->value), w - 1);
            } else {
                py = LV_CLAMP(0, py + speed(e->value), h - 1);
            }
            backlight_tick();
            arrow_show();
        } else if (e->code == REL_WHEEL) {
            if (!backlight_tick()) {
                wheel -= e->value;      /* away from the user: the MFK's previous */
            }
        }
        return;
    }
    if (e->type != EV_KEY) {
        return;
    }
    switch (e->code) {
        case BTN_LEFT:   b = B_LEFT; break;
        case BTN_MIDDLE: b = B_MIDDLE; break;
        case BTN_RIGHT:  b = B_RIGHT; break;
        default: return;
    }
    if (e->value == 1) {
        btn[b].held = true;
        if (backlight_tick()) {
            btn[b].ignored = true;      /* it lit the screen: nothing more */
        } else if (!btn[b].ignored) {
            btn[b].latched = true;
            if (b == B_LEFT) {
                press_x = px;
                press_y = py;
            }
        }
        arrow_show();
    } else if (e->value == 0) {
        btn[b].held    = false;
        btn[b].ignored = false;
    }
}

/* Every event waiting on every mouse, by whichever input reads first */
static void drain(void) {
    struct input_event ev[32];
    ssize_t            n;

    for (int m = 0; m < MICE; m++) {
        if (fds[m] < 0) continue;
        while ((n = read(fds[m], ev, sizeof(ev))) > 0) {
            for (size_t i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
                take(&ev[i]);
            }
        }
        if (n < 0 && errno == ENODEV) {     /* unplugged, or its Bluetooth link gone */
            close_one(m);
        }
    }
}

/* Button b as its input sees it now, its latch taken */
static bool pressed(int b) {
    bool p = (btn[b].held && !btn[b].ignored) || btn[b].latched;

    btn[b].latched = false;
    return p;
}

static void ptr_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    drain();
    /* Pressed and let go since the last read: pressed where it went down, let go at the next read */
    bool quick = btn[B_LEFT].latched && !btn[B_LEFT].held;

    data->state   = pressed(B_LEFT) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point.x = quick ? press_x : px;
    data->point.y = quick ? press_y : py;
}

static void wheel_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    drain();
    data->enc_diff = (int16_t)LV_CLAMP(INT16_MIN, wheel, INT16_MAX);
    wheel          = 0;
    data->state    = pressed(B_MIDDLE) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void back_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    drain();
    data->key   = LV_KEY_ESC;
    data->state = pressed(B_RIGHT) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* A pointer: a left button and X and Y movement, not a paddle the keyer has, and opens */
static bool is_mouse(const char *path, struct stat *st) {
    unsigned long keys[BITS(KEY_MAX)] = {0}, rel[BITS(REL_MAX)] = {0};

    return stat(path, st) == 0 && input_caps_bits(st->st_rdev, "key", keys, BITS(KEY_MAX)) &&
           input_caps_bits(st->st_rdev, "rel", rel, BITS(REL_MAX)) && HAS(keys, BTN_LEFT) && HAS(rel, REL_X) &&
           HAS(rel, REL_Y) && !paddle_keyer_holds(st);
}

/* Every mouse there opened (up to MICE), the ones gone let go */
void mouse_check(void) {
    glob_t      g;
    struct stat st;
    bool        seen[MICE] = {false};

    if (!mouse_on() || glob("/dev/input/event*", 0, NULL, &g) != 0) {
        close_mouse();
        return;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        int m, free_m = -1;

        if (!is_mouse(g.gl_pathv[i], &st)) continue;
        for (m = 0; m < MICE; m++) {
            if (fds[m] >= 0 && st.st_rdev == dev_rdev[m] && st.st_ino == dev_ino[m]) break;
            if (fds[m] < 0 && free_m < 0) free_m = m;
        }
        if (m < MICE) {         /* open already */
            seen[m] = true;
            continue;
        }
        if (free_m < 0) continue;
        int n = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);

        if (n >= 0) {
            fds[free_m]      = n;
            dev_rdev[free_m] = st.st_rdev;
            dev_ino[free_m]  = st.st_ino;
            seen[free_m]     = true;
            LV_LOG_USER("Mouse %d on %s", free_m, g.gl_pathv[i]);
        }
    }
    globfree(&g);
    for (int m = 0; m < MICE; m++)
        if (!seen[m]) close_one(m);
}

static void hide_cb(lv_timer_t *t) {
    if (arrow && !lv_obj_has_flag(arrow, LV_OBJ_FLAG_HIDDEN) &&
        (!any_open() || lv_tick_elaps(active) > HIDE_MS)) {
        arrow_hide();
    }
}

static void setting_cb(Subject *subj, void *user) {
    (void)subj;
    (void)user;
    mouse_check();
}

static void arrow_make(void) {
    for (int y = 0; y < ARROW_H; y++) {
        for (int x = 0; x < ARROW_W; x++) {
            char     c = shape[y][x];
            uint8_t *p = &arrow_px[(y * ARROW_W + x) * LV_IMG_PX_SIZE_ALPHA_BYTE];
            lv_color_t col = c == 'O' ? lv_color_white() : lv_color_black();

            memcpy(p, &col, sizeof(col));
            p[LV_IMG_PX_SIZE_ALPHA_BYTE - 1] = (c == 'X' || c == 'O') ? LV_OPA_COVER : LV_OPA_TRANSP;
        }
    }
    arrow_img.header.cf          = LV_IMG_CF_TRUE_COLOR_ALPHA;
    arrow_img.header.w           = ARROW_W;
    arrow_img.header.h           = ARROW_H;
    arrow_img.header.always_zero = 0;
    arrow_img.data_size          = sizeof(arrow_px);
    arrow_img.data               = arrow_px;
}

void mouse_init(void) {
    lv_indev_t *ptr;

    arrow_make();

    lv_indev_drv_init(&ptr_drv);
    ptr_drv.type    = LV_INDEV_TYPE_POINTER;
    ptr_drv.read_cb = ptr_read;
    ptr             = lv_indev_drv_register(&ptr_drv);

    arrow = lv_img_create(lv_layer_sys());
    lv_img_set_src(arrow, &arrow_img);
    lv_indev_set_cursor(ptr, arrow);
    arrow_hide();

    /* After the MFK's: Settings' factory reset looks at the first encoder for the MFK held */
    lv_indev_drv_init(&wheel_drv);
    wheel_drv.type    = LV_INDEV_TYPE_ENCODER;
    wheel_drv.read_cb = wheel_read;
    lv_indev_set_group(lv_indev_drv_register(&wheel_drv), keyboard_group);

    lv_indev_drv_init(&back_drv);
    back_drv.type    = LV_INDEV_TYPE_KEYPAD;
    back_drv.read_cb = back_read;
    lv_indev_set_group(lv_indev_drv_register(&back_drv), keyboard_group);

    lv_timer_create(hide_cb, 500, NULL);
}

void mouse_start(void) {
    started = true;
    subject_subscribe((Subject *)cfg_mouse, setting_cb, NULL);
    mouse_check();
}
