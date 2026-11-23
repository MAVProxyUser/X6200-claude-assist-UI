/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts at night, the setting (page 4 of the app: Night Auto / On / Off) and the answer for the
 *  drawing now (wefax/night.h). From OlivLoze's fork (F4OLI).
 */

#pragma once

#include "wefax/night.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dim the charts now: the setting, in Auto the sun at the station's locator, asked again once a minute */
bool wefax_night(void);

/* GoldCorpHF: a chart's grey (0 black, 255 white) as the screen shows it, 0xRRGGBB: the theme's text colour for the
 * chart's black, its background for the paper (the Sentry's amber on black), dimmed at night, as the live view draws
 * it (wefax_window.c shades_init). The owner, 2026-11-17: a white chart was the one bright thing on a dark screen.
 * Only the screen: the saved PNGs stay black on white. Any thread */
uint32_t wefax_screen_grey(uint8_t g, bool night);

/* The button's label ("Night" over "Auto (on)"...) and the next setting */
const char *wefax_night_label(void);
void        wefax_night_next(void);

#ifdef __cplusplus
}
#endif
