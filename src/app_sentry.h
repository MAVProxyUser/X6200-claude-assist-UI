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
/* The SDR pool's panel (sdr_pool.h), the Sentry opened on F for it when it is not open (APP held on H) */
void app_sentry_open_pool(void);

/* The release and the commit the GUI is built from, "v0.3.12.19 f739c40": in every Sentry title, AIRSPACE's too */
const char *app_sentry_release(void);

#ifdef __cplusplus
}
#endif
