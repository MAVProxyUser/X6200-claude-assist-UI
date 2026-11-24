/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

/* Settings: eleven tabs on three button pages and a rail at the left */

#include "app_window.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_settings;

/* Before the settings are saved at power off: a Measure under way (TX Cal > S-METER) puts PRE, ATT and the RF gain back */
void settings_window_poweroff(void);

/* TX Cal > S-METER's Measure from the remote (remote_server.c "smc"): started or stopped as its button, the page open
 * or not; the three figures put (PRE, ATT the band's for the power the radio is on, RF gain in hundredths a step);
 * running or not, the figures in use and the last note (any thread) */
void settings_smeter_measure(bool start);
void settings_smeter_put(int32_t pre, int32_t att, int32_t rfg);
bool settings_smeter_status(int32_t fig[3], char *note, size_t n);

/* The radio's network name (Settings > Wireless > Network name): the web configurator is at http://<name>.local */
void settings_network_name(char *out, size_t size);

#ifdef __cplusplus
}
#endif
