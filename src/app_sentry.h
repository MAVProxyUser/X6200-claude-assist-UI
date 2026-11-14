/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Sentry: a listener's receiver on the base's whole spectrum (app_sentry.c)
 */

#pragma once

#include "app_window.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_sentry;

/* The Sentry opened on a screen of its ring by its letter, "A" to "G"; "H" opens AIRSPACE (app_adsb.c), its eighth */
void app_sentry_open_at(char letter);

#ifdef __cplusplus
}
#endif
