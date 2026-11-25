/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - CW keyer messages
 *
 *  Copyright (c) 2026 Giovanni Ricciardi IU1WXY
 */

#pragma once

#include "app_window.h"

#include <stdbool.h>

extern dialog_t *dialog_msg_cw;

/* params_msg_cw_load() hands over the stored texts one by one */
void dialog_msg_cw_append(uint32_t id, const char *val);

/* MSG > CW's switches that live only while the GUI runs (the bench's remote reads them, "cfg? msg_cw_beacon" and
 * "cfg? msg_cw_qrss"): the beacon's pause in s, 0 off; QRSS3 on */
int  dialog_msg_cw_beacon_s(void);
bool dialog_msg_cw_qrss(void);
