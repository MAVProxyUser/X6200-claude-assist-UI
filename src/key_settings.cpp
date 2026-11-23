/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - what the keys do (key_settings.h, #111)
 */

#include "key_settings.h"

#include "cfg/settings_manager.h"

const key_action_t key_front_actions[] = {
    {" None ", ACTION_NONE},
    {" Screenshot ", ACTION_SCREENSHOT},
    {" RX recorder on/off ", ACTION_RECORDER},
    {" Screen rec on/off ", ACTION_SCREEN_REC},
    {" Mute ", ACTION_MUTE},
    {" Frequency lock on/off ", ACTION_FREQ_LOCK},
    {" Step up ", ACTION_STEP_UP},
    {" Step down ", ACTION_STEP_DOWN},     /* FST's long press until it is changed */
    {" WF mode ", ACTION_WF_MODE},
    {" WF 3D on/off ", ACTION_WF_3D},
    {" WF full screen on/off ", ACTION_WF_FULL},    /* upstream's (#302), merged 2026-11-23 */
    {" Speech mode ", ACTION_VOICE_MODE},           /* upstream's speech for blind and low-vision operators */
    {" CW decoder on/off ", ACTION_CW_DECODER},
    {" CW ZAP ", ACTION_CW_ZAP},
    {" SWL panel ", ACTION_SWL_PANEL},
    {" SWL: save channel ", ACTION_SWL_SAVE},
    {" ATT on/off ", ACTION_ATT_TOGGLE},
    {" PRE on/off ", ACTION_PRE_TOGGLE},
    {" Tuner on/off ", ACTION_ATU_TOGGLE},
    {" Tuner: tune ", ACTION_ATU_TUNE},
    {" TUNE ext. tuner on/off ", ACTION_EXT_TUNE},
    /* The radio's own keys' jobs (#111) */
    {" Mode: next ", ACTION_MODE_NEXT},
    {" Mode: back ", ACTION_MODE_PREV},
    {" VFO A/B ", ACTION_VFO_AB},
    {" Band up ", ACTION_BAND_UP},
    {" Band down ", ACTION_BAND_DOWN},
    {" AGC ", ACTION_AGC_NEXT},
    {" Split on/off ", ACTION_SPLIT},
    {" USER page ", ACTION_USER_PAGE},
    {" Charger on/off ", ACTION_CHARGER},
    /* Digital modes, in the order of the Modes menu */
    {" APP RTTY ", ACTION_APP_RTTY},
    {" APP FT8 ", ACTION_APP_FT8},
    {" APP WSPR ", ACTION_APP_WSPR},
    {" APP JS8 ", ACTION_APP_JS8},
    {" APP SSTV ", ACTION_APP_SSTV},
    {" APP WeFax ", ACTION_APP_WEFAX},
    {" APP NavTex ", ACTION_APP_NAVTEX},
    {" APP RADE ", ACTION_APP_FREEDV},
    {" APP PSK ", ACTION_APP_PSK},
    {" APP Olivia ", ACTION_APP_OLIVIA},
    {" APP JTTY ", ACTION_APP_JTTY},
    {" APP Modes menu ", ACTION_APP_MODES},
    /* Tools */
    {" APP Cluster ", ACTION_APP_CLUSTER},
    {" APP CW Trainer ", ACTION_APP_CWTRAINER},
    {" APP CW Keyboard ", ACTION_APP_CW_KBD},
    {" APP Field Ops ", ACTION_APP_FIELD},
    {" APP Bands ", ACTION_APP_BANDS},
    {" APP Propagation ", ACTION_APP_PROPAGATION},
    {" APP Channels ", ACTION_APP_CHANNELS},
    {" APP Scanner ", ACTION_APP_SCAN},
    {" APP Internet radio ", ACTION_APP_IRADIO},
    {" APP Zello ", ACTION_APP_ZELLO},
    {" APP SVXConnect ", ACTION_APP_SVX},
    {" APP Flights ", ACTION_APP_FLIGHTS},
    {" APP HF Air Band ", ACTION_APP_AERO},
    {" APP CB ", ACTION_APP_CB},
    {" APP Weather ", ACTION_APP_METEO},
    {" APP Mail ", ACTION_APP_MAIL},
    {" APP DMR ", ACTION_APP_DMR},
    {" APP RX recorder ", ACTION_APP_RECORDER},
    {" APP SWR Scan ", ACTION_APP_SWRSCAN},
    {" APP GPS ", ACTION_APP_GPS},
    {" APP QTH grid ", ACTION_APP_QTH},
    {" APP Settings ", ACTION_APP_SETTINGS},
#ifndef BOARD_GSOC
    {" APP Sentry ", ACTION_APP_SENTRY},
#endif
    {" APP Airspace ", ACTION_APP_ADSB},
    {" APP Birdie Cal ", ACTION_APP_BIRDIE},
    {" APP Time Signal ", ACTION_APP_TIMESIG},
    {" APP On the Air ", ACTION_APP_SWL},
};
const size_t key_front_n = sizeof(key_front_actions) / sizeof(key_front_actions[0]);

const key_action_t key_mic_actions[] = {
    {" None ", ACTION_NONE},
    {" RX recorder on/off ", ACTION_RECORDER},
    {" Screen rec on/off ", ACTION_SCREEN_REC},
    {" Mute ", ACTION_MUTE},
    {" Step up ", ACTION_STEP_UP},
    {" Step down ", ACTION_STEP_DOWN},
    {" Frequency lock on/off ", ACTION_FREQ_LOCK},
    {" NR toggle ", ACTION_NR_TOGGLE},
    {" NB toggle ", ACTION_NB_TOGGLE},
    {" WF mode ", ACTION_WF_MODE},
    {" WF 3D on/off ", ACTION_WF_3D},
    {" WF full screen on/off ", ACTION_WF_FULL},
    {" Speech mode ", ACTION_VOICE_MODE},
    {" CW decoder on/off ", ACTION_CW_DECODER},
    {" CW ZAP ", ACTION_CW_ZAP},
    {" SWL panel ", ACTION_SWL_PANEL},
    {" SWL: save channel ", ACTION_SWL_SAVE},
    {" ATT on/off ", ACTION_ATT_TOGGLE},
    {" PRE on/off ", ACTION_PRE_TOGGLE},
    {" Tuner on/off ", ACTION_ATU_TOGGLE},
    {" Tuner: tune ", ACTION_ATU_TUNE},
    {" TUNE ext. tuner on/off ", ACTION_EXT_TUNE},
    {" Mode: next ", ACTION_MODE_NEXT},
    {" Mode: back ", ACTION_MODE_PREV},
    {" VFO A/B ", ACTION_VFO_AB},
    {" Band up ", ACTION_BAND_UP},
    {" Band down ", ACTION_BAND_DOWN},
    {" AGC ", ACTION_AGC_NEXT},
    {" Split on/off ", ACTION_SPLIT},
    {" USER page ", ACTION_USER_PAGE},
    {" Charger on/off ", ACTION_CHARGER},
};
const size_t key_mic_n = sizeof(key_mic_actions) / sizeof(key_mic_actions[0]);

bool key_action_ok(const key_action_t *list, size_t n, int32_t action) {
    for (size_t i = 0; i < n; i++)
        if (list[i].action == action) return true;
    return false;
}

const key_row_t key_rows[] = {
    {"long_gen", "GEN", &params.long_gen, &params.dirty.long_gen},
    {"long_app", "APP", &params.long_app, &params.dirty.long_app},
    {"long_key", "KEY", &params.long_key, &params.dirty.long_key},
    {"long_msg", "MSG", &params.long_msg, &params.dirty.long_msg},
    {"long_dfn", "DFN", &params.long_dfn, &params.dirty.long_dfn},
    {"long_dfl", "DFL", &params.long_dfl, &params.dirty.long_dfl},
    {"mic_f1", "F1", &params.press_f1, &params.dirty.press_f1},
    {"mic_f2", "F2", &params.press_f2, &params.dirty.press_f2},
#ifdef BOARD_X6200
    {"mic_f3", "F3", &params.press_f3, &params.dirty.press_f3},       /* the H-22 has an F3, the X6100's mic not */
#endif
    {"mic_f1_long", "F1 long", &params.long_f1, &params.dirty.long_f1},
    {"mic_f2_long", "F2 long", &params.long_f2, &params.dirty.long_f2},
#ifdef BOARD_X6200
    {"mic_f3_long", "F3 long", &params.long_f3, &params.dirty.long_f3},
#endif
};
const size_t key_rows_n = sizeof(key_rows) / sizeof(key_rows[0]);

const key_action_t *key_row_actions(size_t row, size_t *n) {
    if (row < KEY_ROWS_FRONT) {
        *n = key_front_n;
        return key_front_actions;
    }
    *n = key_mic_n;
    return key_mic_actions;
}

void key_row_set(size_t row, int32_t action) {
    params_lock();
    *key_rows[row].val = action;
    params_unlock(key_rows[row].dirty);
}

const key_top_t key_top[] = {
    {KA_FST, "fst", "FST", "step up", "step down"},
    {KA_ATU, "atu", "ATU", "tuner on/off", "tune (transmits)"},
    {KA_AB, "ab", "A/B", "VFO A/B", "copy VFO"},
    {KA_BAND_DOWN, "band_down", "Band down", "band down", "nothing"},
    {KA_BAND_UP, "band_up", "Band up", "band up", "nothing"},
    {KA_AM, "am", "AM", "AM modes", "AM modes"},
    {KA_CW, "cw", "CW", "CW / CW-R", "CW / CW-R"},
    {KA_SSB, "ssb", "SSB", "LSB / USB", "data on/off"},
#ifndef BOARD_X6200
    {KA_PRE, "pre", "PRE", "PRE on/off", "ATT on/off"},
    {KA_AGC, "agc", "AGC", "AGC", "split on/off"},
#endif
};
const size_t key_top_n = sizeof(key_top) / sizeof(key_top[0]);

/* FST's hold is params.long_fst, as before: its own job, the step down, is KEY_OWN here */
int32_t key_top_get(size_t i, bool held) {
    ParamInt *p = held ? cfg_key_hold[key_top[i].k] : cfg_key_press[key_top[i].k];

    if (p) return p->get();
    return params.long_fst == ACTION_STEP_DOWN ? KEY_OWN : params.long_fst;
}

void key_top_set(size_t i, bool held, int32_t action) {
    ParamInt *p = held ? cfg_key_hold[key_top[i].k] : cfg_key_press[key_top[i].k];

    if (p) {
        p->set(action);
        return;
    }
    params_lock();
    params.long_fst = action == KEY_OWN ? ACTION_STEP_DOWN : action;
    params_unlock(&params.dirty.long_fst);
}

void key_top_reset(void) {
    for (size_t i = 0; i < key_top_n; i++) {
        key_top_set(i, false, KEY_OWN);
        key_top_set(i, true, KEY_OWN);
    }
}
