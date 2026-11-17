/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Page I, the Sentry's direction finder (app_df.c): a KrakenSDR's five coherent receivers through
 *  x6x00-df (df_rx.c), the bearing to a signal drawn round the array as it is set out
 */

#pragma once

#include "app_window.h"

#include <stdbool.h>
#include <stddef.h>

extern dialog_t *dialog_df;

#ifdef __cplusplus
extern "C" {
#endif

/* The web configurator's Direction finder page (cfg_socket.cpp: df_get, df_set, df_run). app_df_web_run and
 * app_df_web_set on the LVGL thread; app_df_web_json on any. The web page holds the Kraken as page I does, while its
 * df_get keep coming (20 s) */
void app_df_web_run(bool on);
int  app_df_web_set(const char *key, double val);       /* 0 done, -1 not a setting or out of range */
int  app_df_web_json(char *out, size_t n);              /* the length, -1 when it did not fit */

#ifdef __cplusplus
}
#endif
