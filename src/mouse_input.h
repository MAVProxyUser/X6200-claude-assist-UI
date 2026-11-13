/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - A USB or Bluetooth mouse or touchpad on the screen (mouse_input.c), with Settings > Keys > KEYBOARD
 *  AND MOUSE > Mouse on (off by default): an arrow follows it and a left click is a tap where it points, on every
 *  button, tile, list and option the screen has. The wheel turns as the MFK does and the middle button is its
 *  press; the right button is ESC (back, close). A paddle the keyer has (paddle_keyer.c) is never the pointer.
 */

#pragma once

#include <stdbool.h>

/* The pointer's LVGL inputs, after keyboard_init() (they share its group) */
void mouse_init(void);

/* The setting is read from now on: after the settings are loaded */
void mouse_start(void);

/* A mouse there and not open: open it; gone: let it go. key_input.c calls it at every device scan */
void mouse_check(void);
