/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "gui_events.h"
#include "lvgl/lvgl.h"

#include <stdint.h>

/* The MFK knob */
typedef struct {
    int            fd;
    bool           pressed;         /* set by the buttons' device */
    bool           paged;           /* held while the main dial turned pages (no inner ring: screen_main.c), since
                                     * it was pressed: its let-go is no push (mfk_paged_take) */
    lv_indev_drv_t indev_drv;
    lv_indev_t    *indev;
} encoder_t;

/* Open the MFK's /dev/input/eventN; NULL if it cannot */
encoder_t *encoder_init(char *dev_name);

/* An app's MFK push (its LV_EVENT_CLICKED from the encoder): true, once, when the MFK held turned pages with the main
 * dial (the X6100's stand-in for the X6200's inner ring, screen_main.c): the push is then not the app's */
bool mfk_paged_take(void);
