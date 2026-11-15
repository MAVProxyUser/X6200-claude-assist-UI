/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Time Signal (dialog_timesig.h): WWV and WWVH on 2.5 to 25 MHz in AM, their second ticks and their
 *  100 Hz time code decoded (wwv.c), and the radio's clock set from them once two minutes in a row agree, unless NTP
 *  (WiFi) or the GPS keeps it (#261, #275). Frequency and mode come back when it closes.
 *
 *  The time: a minute's :00 tick came in at minute_t (CLOCK_MONOTONIC, the audio's arrival); it left WWV at the
 *  decoded minute's start, TIMESIG_RX_DELAY_S earlier (the path through the air, the base, the codec and PulseAudio).
 *  With NTP keeping the clock the app logs how far the radio's clock is from WWV's that way, a line a minute
 *  ("timesig: ..."): that is the delay to set, once measured on the air.
 */

#include "dialog_timesig.h"

#include "buttons.h"
#include "btaudio.h"
#include "cfg/cfg_api.h"
#include "events.h"
#include "fonts/font_select.h"
#include "gps.h"
#include "keyboard.h"
#include "msg_box.h"
#include "radio.h"
#include "screen_main.h"
#include "styles.h"
#include "theme.h"
#include "wwv.h"

#include <errno.h>
#include <sys/timex.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define RATE               11025    /* the apps' audio (AUDIO_CAPTURE_RATE / AUDIO_DECIM) */
#define TIMESIG_RX_DELAY_S 0.030    /* WWV's tick to the decoder here: a guess until measured (the log line above) */
#define SET_MIN_S          0.050    /* the clock moved only when this far off */
#define FAR_S              60.0     /* further off than this: three minutes in a row to agree, not two */
#define GPS_AGE_S          30

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void audio_cb(unsigned int n, float *samples);
static void freq_cb(button_data_t *b);
static void freq_hold_cb(button_data_t *b);
static void auto_cb(button_data_t *b);
static const char *freq_label(void);
static const char *auto_label(void);

static button_data_t btn_freq = {.type = BTN_TEXT_FN, .label_fn = freq_label, .press = freq_cb, .hold = freq_hold_cb};
static button_data_t btn_auto = {.type = BTN_TEXT_FN, .label_fn = auto_label, .press = auto_cb};
static buttons_page_t page = {{&btn_freq, &btn_auto}};

static dialog_t dialog = {
    .run           = false,
    .construct_cb  = construct_cb,
    .destruct_cb   = destruct_cb,
    .audio_cb      = audio_cb,
    .btn_page      = &page,
    .key_cb        = key_cb,
    .restore_radio = true,
};

dialog_t *dialog_timesig = &dialog;

static const int32_t FREQS[] = {2500000, 5000000, 10000000, 15000000, 20000000, 25000000};
#define FREQS_N ((int)(sizeof(FREQS) / sizeof(FREQS[0])))

static int             freq_i = 2;          /* 10 MHz: day and night, both stations */
static bool            auto_set = true;
static pthread_mutex_t mux = PTHREAD_MUTEX_INITIALIZER;
static wwv_t          *wwv;                 /* under mux: fed on the audio thread, read on the LVGL one */
static lv_timer_t     *timer;
static lv_obj_t       *status_l, *code_l, *time_l, *clock_l;
static uint32_t        frames_seen;
static char            clock_text[160];

static double mono_now(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Audio thread: the newest sample came now, x[0] n samples before it */
static void audio_cb(unsigned int n, float *samples) {
    double t0 = mono_now() - (double)n / RATE;

    pthread_mutex_lock(&mux);
    if (wwv) wwv_feed(wwv, samples, n, t0);
    pthread_mutex_unlock(&mux);
}

static void tune(void) {
    main_screen_set_freq(FREQS[freq_i]);
    cparam_i_set(cfg_cur_mode, radio_mode_am);
    pthread_mutex_lock(&mux);
    if (wwv) wwv_reset(wwv);
    pthread_mutex_unlock(&mux);
    frames_seen = 0;
    clock_text[0] = 0;
}

static const char *freq_label(void) {
    static char s[24];

    snprintf(s, sizeof(s), "WWV\n%g MHz", FREQS[freq_i] / 1e6);
    return s;
}

static const char *auto_label(void) {
    return auto_set ? "Set clock:\nAuto" : "Set clock:\nOff";
}

static void freq_cb(button_data_t *b) {
    freq_i = (freq_i + 1) % FREQS_N;
    tune();
    buttons_refresh(b);
}

static void freq_hold_cb(button_data_t *b) {
    freq_i = (freq_i + FREQS_N - 1) % FREQS_N;
    tune();
    buttons_refresh(b);
}

static void auto_cb(button_data_t *b) {
    auto_set = !auto_set;
    buttons_refresh(b);
}

/* Who keeps the clock: NTP (WiFi, or the GPS through ntpd), the GPS itself, or nothing. NTP keeps it while the
 * kernel's clock is not marked unsynchronised: its error bound alone says too little here (ntpd polls every 1024 s,
 * and it was 1.8 s with the clock 16 ms off, ftx_clock_synced's 1 s refused it) */
static const char *clock_keeper(void) {
    struct timex tx = {0};      /* modes 0: only read */
    double       lat, lon;
    int          age, state = ntp_adjtime(&tx);

    if (state >= 0 && state != TIME_ERROR && !(tx.status & STA_UNSYNC)) return "NTP";
    if (gps_last_fix(&lat, &lon, &age) && age <= GPS_AGE_S) return "the GPS";
    return NULL;
}

/* A frame decoded: how far the radio's clock is from WWV's, and the clock set when it may be */
static void frame_done(const wwv_info_t *in) {
    struct timespec now;
    double          mono = mono_now();
    int64_t         utc = wwv_minute_utc(in);

    clock_gettime(CLOCK_REALTIME, &now);
    double      wwv_now = utc + TIMESIG_RX_DELAY_S + (mono - in->minute_t);
    double      off = now.tv_sec + now.tv_nsec / 1e9 - wwv_now;      /* + the radio's clock ahead */
    const char *keeper = clock_keeper();

    if (keeper) {
        snprintf(clock_text, sizeof(clock_text), "The radio's clock is %+.3f s from WWV's. %s keeps it: left alone.",
                 off, keeper);
        LV_LOG_USER("timesig: radio clock %+.3f s from WWV's (kept by %s; the receive delay taken as %.3f s)", off,
                    keeper, TIMESIG_RX_DELAY_S);
        return;
    }
    /* Two minutes in a row; three for a move of more than a minute (a recording played back, a misread) */
    int need = fabs(off) > FAR_S ? 3 : 2;

    if (in->frames_ok < need) {
        snprintf(clock_text, sizeof(clock_text), "The radio's clock is %+.3f s from WWV's. %d more minute%s to agree.",
                 off, need - in->frames_ok, need - in->frames_ok == 1 ? "" : "s");
        return;
    }
    if (!auto_set || fabs(off) < SET_MIN_S) {
        snprintf(clock_text, sizeof(clock_text), "The radio's clock is %+.3f s from WWV's%s.", off,
                 auto_set ? ": right" : " (Set clock is Off)");
        return;
    }
    /* now - off, taken on the time as it is now: nothing lost to the work above */
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t         t = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec - (int64_t)llround(off * 1e9);
    struct timespec ts = {.tv_sec = t / 1000000000LL, .tv_nsec = t % 1000000000LL};

    if (clock_settime(CLOCK_REALTIME, &ts)) {
        snprintf(clock_text, sizeof(clock_text), "The clock can't be set: %s", strerror(errno));
        LV_LOG_ERROR("timesig: clock_settime: %s", strerror(errno));
        return;
    }
    /* into the clock chip too, kept across a power-off (x6100-rtc: its second's phase) */
    sh_run("/usr/sbin/x6100-rtc save >/dev/null 2>&1 || hwclock -w -u -f /dev/rtc1", NULL, NULL);
    snprintf(clock_text, sizeof(clock_text), "Clock set from WWV: moved %+.3f s.", -off);
    msg_update_text_fmt("Time Signal: clock moved %+.2f s by WWV", -off);
    LV_LOG_USER("timesig: clock moved %+.3f s by WWV (%d minutes agreeing)", -off, in->frames_ok);
}

static void timer_cb(lv_timer_t *t) {
    wwv_info_t in;
    char       s[200], sym[80];
    int        k = 0;

    (void)t;
    pthread_mutex_lock(&mux);
    if (!wwv) {
        pthread_mutex_unlock(&mux);
        return;
    }
    wwv_info(wwv, &in);
    pthread_mutex_unlock(&mux);

    if (!in.locked)
        snprintf(s, sizeof(s), "Listening on %g MHz AM: no second ticks yet (%.0f dB).", FREQS[freq_i] / 1e6,
                 in.tick_db);
    else
        snprintf(s, sizeof(s), "%s's second ticks, %.0f dB over the rest; the time code %.0f dB over its gaps.",
                 in.station == 2 ? "WWVH (1200 Hz)" : "WWV (1000 Hz)", in.tick_db, in.code_db);
    lv_label_set_text(status_l, s);

    /* This minute so far, in tens */
    for (int i = 0; i < 60 && k < (int)sizeof(sym) - 2; i++) {
        if (i && i % 10 == 0) sym[k++] = ' ';
        sym[k++] = i <= in.second && in.symbols[i] ? in.symbols[i] : '.';
    }
    sym[k] = 0;
    if (in.second >= 0) snprintf(s, sizeof(s), "Second %02d   %s", in.second, sym);
    else snprintf(s, sizeof(s), "Waiting for a minute's start (a marker, then a second without 100 Hz).");
    lv_label_set_text(code_l, s);

    if (in.have_time) {
        time_t    utc = (time_t)wwv_minute_utc(&in);
        struct tm tm;

        gmtime_r(&utc, &tm);
        snprintf(s, sizeof(s), "%04d-%02d-%02d %02d:%02d UTC (day %d), DUT1 %+.1f s%s; %d minute%s in a row",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, in.hour, in.minute, in.yday, in.dut1,
                 in.leap_warning ? ", a leap second this month" : "", in.frames_ok, in.frames_ok == 1 ? "" : "s");
        lv_label_set_text(time_l, s);
        if (in.frames != frames_seen && in.frames_ok > 0) frame_done(&in);
    } else {
        lv_label_set_text(time_l, in.frames ? "No whole minute read yet: a weak or fading signal." : "");
    }
    frames_seen = in.frames;
    lv_label_set_text(clock_l, clock_text);
}

static void key_cb(lv_event_t *e) {
    switch (*((uint32_t *)lv_event_get_param(e))) {
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            radio_change_vol(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            radio_change_vol(1);
            break;
        default:
            break;
    }
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t y, const char *s) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, 740);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, 20, y);
    lv_label_set_text(l, s);
    return l;
}

static void construct_cb(lv_obj_t *parent) {
    dialog.obj = dialog_init(parent);
    buttons_load_page(&page);

    text(dialog.obj, &sony_28, TC(text_head), 12, "TIME SIGNAL");
    text(dialog.obj, &sony_20, TC(text_dim), 52,
         "WWV and WWVH send the time on 2.5 to 25 MHz in AM. Their second ticks give the second, the 100 Hz code "
         "the minute; once two minutes in a row agree the clock is set, unless WiFi (NTP) or a GPS keeps it.");
    status_l = text(dialog.obj, &sony_22, TC(text), 122, "");
    code_l   = text(dialog.obj, &sony_22, TC(text_value), 176, "");
    time_l   = text(dialog.obj, &sony_24, TC(text), 214, "");
    clock_l  = text(dialog.obj, &sony_22, TC(accent), 256, "");

    pthread_mutex_lock(&mux);
    if (!wwv) wwv = wwv_new(RATE);
    pthread_mutex_unlock(&mux);
    tune();
    timer = lv_timer_create(timer_cb, 500, NULL);
    timer_cb(NULL);

    lv_group_add_obj(keyboard_group, dialog.obj);
    lv_obj_add_event_cb(dialog.obj, key_cb, LV_EVENT_KEY, NULL);
    lv_group_focus_obj(dialog.obj);
    lv_group_set_editing(keyboard_group, true);
}

static void destruct_cb(void) {
    if (timer) lv_timer_del(timer);
    timer = NULL;
    pthread_mutex_lock(&mux);
    wwv_free(wwv);
    wwv = NULL;
    pthread_mutex_unlock(&mux);
    status_l = code_l = time_l = clock_l = NULL;
    lv_group_remove_obj(dialog.obj);
}
