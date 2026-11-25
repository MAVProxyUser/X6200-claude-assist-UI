/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Stand-ins for what src/cat.cpp and src/cfg call outside them (tools/cat_modes): LVGL's log and async calls, the GUI
 *  thread's check (the test is it), and the radio's side cat.cpp reads (in receive, no meters, the X6200's caps).
 */

#include "lvgl.h"
#include "scheduler.h"

extern "C" {
#include "radio.h"
#include "smeter.h"
#include "tx_meters.h"

void _lv_log_add(lv_log_level_t level, const char *file, int line, const char *func, const char *format, ...) {
    (void)level, (void)file, (void)line, (void)func, (void)format;
}

lv_res_t lv_async_call(lv_async_cb_t async_xcb, void *user_data) {
    (void)async_xcb, (void)user_data;
    return LV_RES_OK;
}

lv_res_t lv_async_call_cancel(lv_async_cb_t async_xcb, void *user_data) {
    (void)async_xcb, (void)user_data;
    return LV_RES_OK;
}

bool scheduler_on_gui_thread() {
    return true;
}

/* The CAT thread's requests run on the GUI thread there: here at once */
bool scheduler_call(scheduler_fn_t fn, void *arg) {
    fn(arg);
    return true;
}

uint32_t radio_caps(void) {
    return HW_CAP_SAM_WFM;
}

radio_state_t radio_get_state() {
    return RADIO_RX;
}

bool radio_set_ptt(bool tx) {
    (void)tx;
    return false;
}

void radio_usb_to_cat(void) {
}

int16_t meter_get_raw_db(void) {
    return -120;
}

void tx_info_get(float *pwr, float *swr) {
    if (pwr) *pwr = 0.0f;
    if (swr) *swr = 1.0f;
}

float tx_info_get_alc(bool *greyed) {
    if (greyed) *greyed = true;
    return 0.0f;
}
}
