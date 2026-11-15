/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - QSO state for the keyboard digital modes
 */

#include "digi_qso.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "msg.h"
#include "dialog_field.h"

#include <time.h>

static char    qso_dx[16];
static uint8_t rst_s = 9;          /* the strength digit, 1..9 */
static uint8_t rst_r = 9;
static char    rst_s_str[8] = "599";
static char    rst_r_str[8] = "599";
static bool    autograb = true;

const char *digi_qso_dx(void) { return qso_dx; }

void digi_qso_set_dx(const char *call) {
    size_t o = 0;

    if (!call) {
        qso_dx[0] = '\0';
        return;
    }
    for (const char *p = call; *p && o + 1 < sizeof(qso_dx); p++) {
        qso_dx[o++] = (char)toupper((unsigned char)*p);
    }
    qso_dx[o] = '\0';
}

void digi_qso_clear(void) {
    qso_dx[0] = '\0';
    rst_s = rst_r = 9;
    snprintf(rst_s_str, sizeof(rst_s_str), "59%u", rst_s);
    snprintf(rst_r_str, sizeof(rst_r_str), "59%u", rst_r);
}

const char *digi_qso_rst_sent(void) { return rst_s_str; }
const char *digi_qso_rst_rcvd(void) { return rst_r_str; }

static uint8_t rst_change(uint8_t *v, char *str, size_t sz, int16_t df) {
    if (df) {
        int n = (int)*v + (df > 0 ? 1 : -1);

        if (n < 1) n = 9;
        if (n > 9) n = 1;
        *v = (uint8_t)n;
        snprintf(str, sz, "59%u", *v);
    }
    return *v;
}

uint8_t digi_qso_change_rst_sent(int16_t df) {
    return rst_change(&rst_s, rst_s_str, sizeof(rst_s_str), df);
}

uint8_t digi_qso_change_rst_rcvd(int16_t df) {
    return rst_change(&rst_r, rst_r_str, sizeof(rst_r_str), df);
}

bool digi_qso_autograb(void) { return autograb; }

bool digi_qso_toggle_autograb(void) {
    autograb = !autograb;
    return autograb;
}

/* Does this word look like an amateur callsign? Requires a digit and at
 * least one letter after it - enough to reject ordinary words without
 * rejecting real calls, including the /P and /QRP forms. */
static bool looks_like_call(const char *w) {
    size_t len = strlen(w);
    bool   has_digit = false, letter_after_digit = false;

    if (len < 3 || len > 12) return false;

    for (size_t i = 0; i < len; i++) {
        char c = w[i];

        if (c >= '0' && c <= '9') {
            has_digit = true;
        } else if (c >= 'A' && c <= 'Z') {
            if (has_digit) letter_after_digit = true;
        } else if (c != '/') {
            return false;                 /* anything else disqualifies */
        }
    }
    return has_digit && letter_after_digit;
}

/* Assemble words from the decoded stream and capture the one that
 * follows "DE". Calls are almost always sent as "MYCALL DE HISCALL", so
 * this catches the correspondent without the operator typing. Called
 * from a decoder, i.e. on the DSP audio thread. */
const char *digi_qso_feed_rx(char c) {
    static char   word[16];
    static size_t wl = 0;
    static bool   after_de = false;
    bool          is_word;

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

    is_word = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/';
    if (is_word) {
        if (wl + 1 < sizeof(word)) word[wl++] = c;
        return NULL;
    }

    if (wl == 0) return NULL;             /* separator run */
    word[wl] = '\0';
    wl = 0;

    if (strcmp(word, "DE") == 0) {
        after_de = true;
        return NULL;
    }
    if (after_de) {
        after_de = false;
        if (looks_like_call(word)) {
            if (autograb) {
                digi_qso_set_dx(word);
                /* msg_update_text_fmt() only queues an event, so it is safe
                 * from the audio thread. */
                msg_update_text_fmt("DX: %s", qso_dx);
            }
            return word;
        }
    }
    return NULL;
}

/* ---- The shared keys (#55) -------------------------------------------- */

static char   logged_call[16];
static time_t logged_at;

static void refresh(button_data_t *b) {
    if (b->disp_btn) buttons_refresh(b);
}

void digi_qso_refresh_keys(void) {
    refresh(&btn_digi_log);
}

static const char *rsts_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "RST snt\n%s", rst_s_str);
    return buf;
}

static const char *rstr_label(void) {
    static char buf[24];

    snprintf(buf, sizeof(buf), "RST rcvd\n%s", rst_r_str);
    return buf;
}

static const char *log_label(void) {
    static char buf[32];

    snprintf(buf, sizeof(buf), "Log QSO\n%s", qso_dx[0] ? qso_dx : "---");
    return buf;
}

static void rsts_press(button_data_t *b) {
    digi_qso_change_rst_sent(1);
    refresh(b);
}

static void rsts_hold(button_data_t *b) {
    while (digi_qso_change_rst_sent(0) != 9) digi_qso_change_rst_sent(1);
    refresh(b);
}

static void rstr_press(button_data_t *b) {
    digi_qso_change_rst_rcvd(1);
    refresh(b);
}

static void rstr_hold(button_data_t *b) {
    while (digi_qso_change_rst_rcvd(0) != 9) digi_qso_change_rst_rcvd(1);
    refresh(b);
}

static void log_qso(bool again) {
    char   text[48];
    time_t now = time(NULL);

    if (!qso_dx[0]) {
        msg_update_text_fmt("Nothing to log: no DX call yet");
        return;
    }
    if (!again && !strcmp(logged_call, qso_dx) && now - logged_at < 600) {
        msg_update_text_fmt("%s logged %d min ago: hold Log QSO to log it again", qso_dx, (int)(now - logged_at) / 60);
        return;
    }
    snprintf(text, sizeof(text), "%s|%s|%s", qso_dx, rst_s_str, rst_r_str);
    if (dialog_field_qso_log(text)) {    /* it says "QSO logged: ..." itself */
        snprintf(logged_call, sizeof(logged_call), "%s", qso_dx);
        logged_at = now;
    }
}

static void log_press(button_data_t *b) {
    (void)b;
    log_qso(false);
}

static void log_hold(button_data_t *b) {
    (void)b;
    log_qso(true);
}

button_data_t btn_digi_rsts = {.type = BTN_TEXT_FN, .label_fn = rsts_label, .press = rsts_press, .hold = rsts_hold};
button_data_t btn_digi_rstr = {.type = BTN_TEXT_FN, .label_fn = rstr_label, .press = rstr_press, .hold = rstr_hold};
button_data_t btn_digi_log  = {.type = BTN_TEXT_FN, .label_fn = log_label, .press = log_press, .hold = log_hold};
