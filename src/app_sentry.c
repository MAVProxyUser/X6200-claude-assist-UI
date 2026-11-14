/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Sentry: a listener's receiver on the X6200 base's whole spectrum, in the look of an old amber
 *  terminal, over the whole screen but the buttons. Receive only while it is open (radio_rx_only()).
 *
 *  Four screens, the buttons F1-F5 under them:
 *    A SCAN    384 kHz around the dial (radio_psd_wide(), dsp_psd_raw()), its middle 360 kHz as a trace over a
 *              waterfall, the signals that stand out of the noise around them marked and named (AM broadcast
 *              channel, CB channel, FM station, time signal, shortwave broadcaster from the EiBi list, FT8, ham
 *              band). AUTO-SCAN locks on the strongest target not seen lately and tracks it, then goes on; SEMI-AUTO
 *              marks them for the MFK to choose; MAN-OVERRIDE (the main knob takes it there) only marks. The
 *              routine: BAND, the 360 kHz seen when AUTO began; SWEEP, the bands marked in BANDS, a few seconds each.
 *    B TRACK   the target locked: what it is, how long it has been heard, its signal and the noise. HOLD keeps it,
 *              NEXT goes on, IGNORE never locks it again (until the Sentry closes), REC records the audio.
 *    C BANDS   what each band carries, when, with which antenna, and what the Sentry found there; LISTEN tunes it
 *              and scans it, SWEEP marks it for the sweep.
 *    D DECODE  the decoders and the frequencies they work on; OPEN takes the radio there in the radio's own app.
 *    E SYSTEM  the rest of the radio, in this look: its settings, the other apps, the digital modes menu, the
 *              classic radio screen (with transmit), and where the radio starts. The APP key comes here.
 *
 *  On the X6200 the Sentry is the home screen (cfg_sentry_home): the radio starts in it, and an app or a menu opened
 *  from it comes back to it when it closes (screen_main.c main_screen_dialog_closed). CLASSIC RADIO leaves it till it
 *  is opened again.
 *
 *  The picture's bins: bin n/2 is the dial (the base's FFT shifted, its DC in the middle), checked on the night AM
 *  band where the stations sit on their 10 kHz channels.
 */

#include "app_sentry.h"

#ifdef BOARD_X6200
#include "dialog_base_update.h"     /* the X6200's base firmware: never on another radio */
#endif
#include "git_commit.h"
#include "dialog_shell.h"

#include "band_plan.h"
#include "board.h"
#include "broadcast_db.h"
#include "buttons.h"
#include "clock.h"
#include "cfg/cfg_api.h"
#include "cfg/settings_manager_api.h"
#include "dsp.h"
#include "events.h"
#include "gui_styles.h"
#include "keyboard.h"
#include "main.h"             /* VERSION, the release in the title */
#include "main_screen.h"
#include "msg_box.h"
#include "params/params.h"
#include "qth/qth.h"
#include "radio.h"
#include "rtl_spectrum.h"
#include "recorder.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

extern const lv_font_t pixel_16, pixel_20, pixel_24, pixel_28, pixel_44;

/* The picture */
#define VIEW_HZ     360000      /* the middle of the base's 384 kHz: its filter rolls off in the last 12 kHz each side */
#define IMG_X       8
#define IMG_W       784
#define HZ_PX       ((double)VIEW_HZ / IMG_W)
#define SPEC_H      74
#define SCALE_H     16
#define MAX_BINS    1024
#define FLOOR_HALF  16          /* bins each side for the local floor: +-12 kHz at 750 Hz */
#define TARGET_DB   7.0f        /* over the local floor: marked */
#define LOCK_DB     10.0f       /* AUTO locks on this much */
#define HEARD_DB    6.0f        /* tracked: still there */
#define TARGET_SEP  6           /* bins between two targets, the stronger kept: an AM carrier's sidebands */
#define MAX_TARGETS 24
#define LABELS      12
#define TICKS       9

/* The screen */
#define SW          800
#define HEAD_H      44
#define BODY_Y      50
#define BOX_H       96          /* a box of three option rows */
#define LOW_H       74          /* a box of two option rows */
#define DLOW_H      82          /* DECODE's, three rows of the small font */
#define KEYS_H      46          /* the F1-F5 row, the mock's, over the radio's own buttons */
#define LEGEND_H    20          /* over it: what the knobs do on this screen */

/* The sentry */
#define SETTLE_FRAMES 8         /* frames after a move before AUTO decides */
#define LOST_MS     4000        /* tracked, gone this long: lost */
#define DWELL_MS    20000       /* AUTO: this long on a target unless held */
#define SWEEP_MS    7000        /* SWEEP: this long on a band without a target */
#define REVISIT_MS  120000      /* AUTO: a target visited comes again after this */
#define VISITED     16
#define IGNORED     32

typedef enum { SCR_SCAN, SCR_TRACK, SCR_BANDS, SCR_DECODE, SCR_SYSTEM, SCR_RTL, SCR_N } scr_t;
typedef enum { MODE_AUTO, MODE_SEMI, MODE_MAN } sysmode_t;
typedef enum { ROUT_BAND, ROUT_SWEEP } routine_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static bool front_key(int key, int state);
static void f1_cb(button_data_t *b);
static void f2_cb(button_data_t *b);
static void f3_cb(button_data_t *b);
static void f4_cb(button_data_t *b);
static bool knob_taken(void);
static void rtl_listen(bool on);
static int64_t rtl_snap(int64_t hz);
static void osd(const char *title, const char *text, int bar);
static void show_head(void);
static void f5_cb(button_data_t *b);
static const char *f1_label(void);
static const char *f2_label(void);
static const char *f3_label(void);
static const char *f4_label(void);
static const char *f5_label(void);

static button_data_t btn[5] = {
    {.type = BTN_TEXT_FN, .label_fn = f1_label, .press = f1_cb},
    {.type = BTN_TEXT_FN, .label_fn = f2_label, .press = f2_cb},
    {.type = BTN_TEXT_FN, .label_fn = f3_label, .press = f3_cb},
    {.type = BTN_TEXT_FN, .label_fn = f4_label, .press = f4_cb},
    {.type = BTN_TEXT_FN, .label_fn = f5_label, .press = f5_cb},
};

static buttons_page_t page = {{&btn[0], &btn[1], &btn[2], &btn[3], &btn[4]}};

static dialog_t dialog = {
    .run          = false,
    .construct_cb = construct_cb,
    .destruct_cb  = destruct_cb,
    .rotary_cb    = rotary_cb,
    .btn_page     = &page,
    .key_cb       = key_cb,
    .front_key_cb = front_key,
    .rotary_takes_cb = knob_taken,
};

dialog_t *dialog_sentry = &dialog;

typedef struct {
    int32_t hz;
    float   db, con, width_khz;
    bool    ignored;
} target_t;

typedef struct {
    char    label[12];      /* over the mark */
    char    name[72];       /* in the readouts */
    int32_t tune_hz;
    int     mode;           /* radio_mode_t, -1: the radio's own */
} ident_t;

/* ---- What there is to hear: the bands ---- */

typedef struct {
    const char *name;
    int32_t     lo, hi;         /* kHz */
    const char *hear, *ant;
    char        when;           /* N night, D day, A any time, L local */
    int32_t     listen_hz;
    int         mode;
    bool        sweep;          /* in the sweep at first */
    const char *about;
} band_t;

static const band_t bands[] = {
    {"SHIP MF", 415, 526, "NAVTEX: SHIP WARNINGS ON 518", "LONG WIRE, UNTRIED", 'N', 518000, radio_mode_usb, false,
     "Coast stations' NAVTEX on 518 kHz (English) and 490: weather and navigation warnings, farther at night."},
    {"AM BC", 530, 1700, "AM RADIO: NEWS, TALK, SPORTS", "ANY WIRE; CB WHIP OK", 'N', 720000, radio_mode_am, true,
     "Your AM radio band. By day the locals; after dark big stations 1000 km away (WGN 720, Chicago)."},
    {"160 M", 1800, 2000, "HAM CW AND VOICE, WINTER", "LONG WIRE, UNTRIED", 'N', 1900000, radio_mode_lsb, false,
     "The top band: hams on winter nights. It needs a long wire; the JPC-12 does not reach it."},
    {"80 M", 3500, 4000, "HAM NETS, REGIONAL VOICE", "LONG WIRE, UNTRIED", 'N', 3850000, radio_mode_lsb, false,
     "Evening ham nets within a few hundred km: voice on LSB above 3.6 MHz, CW and FT8 below."},
    {"SHIP 4", 4063, 4438, "NAVTEX 4209.5, WEATHER FAX", "LONG WIRE, UNTRIED", 'N', 4235000, radio_mode_usb, false,
     "The 4 MHz marine band: NAVTEX on 4209.5, NOAA weather fax (4235 Boston, 4317.9 New Orleans, 4346 Pt Reyes)."},
    {"60 M BC", 4750, 5060, "TROPICAL BROADCASTS, WWV 5", "JPC-12 RED-SILVER", 'N', 4950000, radio_mode_am, false,
     "Shortwave stations from the tropics at night, and WWV's time signal on 5.000 MHz."},
    {"60 M", 5330, 5410, "HAM CHANNELS, FT8 ON 5.357", "JPC-12 RED-SILVER", 'N', 5357000, radio_mode_usb, false,
     "Five ham channels and FT8 on 5.357: regional by day, farther at night, as 80 m."},
    {"49 M BC", 5900, 6200, "SHORTWAVE BROADCASTS", "JPC-12 RED-SILVER", 'N', 6050000, radio_mode_am, true,
     "International broadcasters in the evening: news and music from abroad, in AM."},
    {"SHIP 6", 6200, 6525, "WEATHER FAX, SHIP VOICE", "JPC-12 RED-SILVER", 'N', 6340500, radio_mode_usb, false,
     "The 6 MHz marine band: NOAA weather fax from Boston on 6340.5, ship voice in USB, evening and night."},
    {"40 M", 7000, 7300, "FT8, CW, VOICE ALL NIGHT", "JPC-12 RED-SILVER, TESTED", 'N', 7150000, radio_mode_lsb, true,
     "The night band, the whole country after dark. FT8 on 7.074, voice from 7.125 up in LSB."},
    {"41 M BC", 7300, 7600, "SHORTWAVE BROADCASTS", "JPC-12 RED-SILVER", 'N', 7450000, radio_mode_am, false,
     "Broadcasters just above the 40 m ham band, strong in the evening (7.505 was loud in the coil test)."},
    {"SHIP 8", 8195, 8815, "WEATHER FAX, SHIP VOICE", "JPC-12 MIDWAY", 'A', 8503900, radio_mode_usb, false,
     "The 8 MHz marine band: NOAA weather fax from New Orleans (8503.9) and Pt Reyes (8682), coast stations' voice."},
    {"UTIL 9", 9040, 9399, "NOAA WEATHER FAX, BOSTON", "JPC-12 MIDWAY", 'A', 9110000, radio_mode_usb, false,
     "Utility stations below the 31 m broadcasters: NOAA's weather fax from Boston on 9110 kHz, day and night."},
    {"31 M BC", 9400, 9900, "SHORTWAVE BROADCASTS", "JPC-12 MIDWAY", 'A', 9650000, radio_mode_am, true,
     "The busiest broadcast band, day and evening: AM programmes from around the world."},
    {"30 M", 10100, 10150, "FT8, CW; WWV ON 10.000", "JPC-12 MIDWAY, TESTED", 'A', 10000000, radio_mode_am, true,
     "Data and CW only, no voice. WWV's time signal on 10.000 MHz day and night: a good antenna test."},
    {"25 M BC", 11600, 12100, "SHORTWAVE BROADCASTS", "JPC-12 MIDWAY", 'D', 11850000, radio_mode_am, false,
     "Broadcasters by day and into the evening."},
    {"SHIP 12", 12230, 13200, "WEATHER FAX, SHIP VOICE", "JPC-12 MIDWAY", 'D', 12750000, radio_mode_usb, false,
     "The 12 MHz marine band by day: NOAA weather fax (12750 Boston, 12789.9 New Orleans, 12786 Pt Reyes)."},
    {"20 M", 14000, 14350, "THE WORLD: FT8, VOICE, SSTV", "JPC-12 RED-GOLD, TESTED", 'D', 14200000, radio_mode_usb,
     true, "The daytime DX band: FT8 on 14.074, voice in USB above 14.150, SSTV pictures on 14.230."},
    {"19 M BC", 15100, 15800, "BROADCASTS, WWV 15", "JPC-12 RED-GOLD", 'D', 15000000, radio_mode_am, false,
     "Daytime broadcasters, and WWV on 15.000 MHz."},
    {"SHIP 16", 16360, 17410, "WEATHER FAX, PT REYES", "JPC-12 RED-GOLD", 'D', 17151200, radio_mode_usb, false,
     "The 16 MHz marine band by day: Pt Reyes' weather fax on 17151.2, and coast stations."},
    {"17 M", 18068, 18168, "QUIET DX BAND", "JPC-12 ABOVE GOLD, UNTRIED", 'D', 18120000, radio_mode_usb,
     false, "A small, quiet band for long-distance contacts by day."},
    {"15 M", 21000, 21450, "DX BY DAY", "JPC-12 ABOVE GOLD, UNTRIED", 'D', 21300000, radio_mode_usb, false,
     "Long distance by day when the sun is active."},
    {"12 M", 24890, 24990, "DX WHEN THE SUN IS UP", "CB WHIP IS CLOSE", 'D', 24940000, radio_mode_usb, false,
     "A narrow DX band, open on sunny days near the solar maximum."},
    {"11 M CB", 26965, 27405, "CB VOICE, TRUCKERS ON 19", "CB WHIP, TESTED", 'L', 27185000, radio_mode_am, true,
     "Citizens band: local talk within a few miles, 19 for truckers, 9 for emergencies; skip from afar in summer."},
    {"10 M", 28000, 29700, "FT8, BEACONS, VOICE WHEN OPEN", "CB WHIP WORKS, TESTED", 'D', 28400000, radio_mode_usb,
     true, "Open by day near the solar maximum: FT8 on 28.074, voice from 28.300. The CB whip heard FT8 here."},
    {"6 M", 50000, 54000, "SUMMER OPENINGS, MOSTLY QUIET", "SHORT WHIP", 'D', 50125000, radio_mode_usb, false,
     "The magic band: mostly quiet, with sudden summer openings."},
    {"FM BC", 88000, 108000, "FM RADIO, LOCAL", "ANY SHORT WHIP", 'L', 92300000, radio_mode_wfm, false,
     "Local FM stations, one or two 200 kHz-wide ones in the picture at a time."},
};

#define BANDS_N ((int)(sizeof(bands) / sizeof(bands[0])))

static struct {
    bool     sweep;
    int      seen;          /* targets the last time the picture was on it */
    float    best;
    uint32_t ms;            /* lv_tick of that, 0 never */
} band_rt[BANDS_N];

/* ---- The decoders and where they work ---- */

static const int32_t f_ft8[] = {1840000, 3573000, 5357000, 7074000, 10136000, 14074000, 18100000, 21074000,
                                24915000, 28074000, 50313000};
static const int32_t f_ft4[] = {3575000, 7047500, 10140000, 14080000, 18104000, 21140000, 24919000, 28180000,
                                50318000};
static const int32_t f_wspr[] = {1836600, 3568600, 5364700, 7038600, 10138700, 14095600, 18104600, 21094600,
                                 24924600, 28124600, 50293000};
static const int32_t f_cw[] = {3530000, 7030000, 10115000, 14030000, 18080000, 21030000, 24900000, 28030000};
static const int32_t f_sstv[] = {3845000, 7171000, 14230000, 21340000, 28680000};
static const int32_t f_navtex[] = {518000, 490000, 4209500};
static const int32_t f_wefax[] = {4235000, 6340500, 9110000, 12750000, 4317900, 8503900, 12789900, 4346000,
                                  8682000, 12786000, 17151200};      /* NOAA's: Boston, New Orleans, Pt Reyes */
static const int32_t f_js8[] = {3578000, 7078000, 10130000, 14078000, 18104000, 21078000, 28078000};

typedef struct {
    const char     *name;
    const char     *mem;        /* the app's memory (app_window.c app_mem_save): it opens where that says */
    press_action_t  app;        /* 0: none, the main screen */
    int             mode, proto;
    const int32_t  *hz;
    int             n;
    const char     *about;
    int             audio;      /* where the app's decoder listens, Hz: the dial that far under the station (USB) */
} decoder_t;

#define N_OF(a) ((int)(sizeof(a) / sizeof(a[0])))

static const decoder_t decoders[] = {
    {"FT8", "FT8", ACTION_APP_FT8, radio_mode_usb_dig, 1, f_ft8, N_OF(f_ft8),
     "15-SECOND DIGITAL MESSAGES FROM HAMS WORLDWIDE: CALLS, GRIDS, SIGNAL REPORTS. THE BUSIEST MODE ON THE AIR."},
    {"FT4", "FT4", ACTION_APP_FT8, radio_mode_usb_dig, 0, f_ft4, N_OF(f_ft4),
     "FT8'S FASTER COUSIN, 7.5-SECOND SLOTS, USED IN CONTESTS. THE FT8 APP DECODES IT."},
    {"WSPR", "WSPR", ACTION_APP_WSPR, radio_mode_usb_dig, -1, f_wspr, N_OF(f_wspr),
     "WEAK BEACONS: WHO IS HEARD WHERE, EVERY 2 MINUTES. THE WSPR APP CHOOSES ITS OWN BAND."},
    {"CW", "", 0, radio_mode_cw, -1, f_cw, N_OF(f_cw),
     "MORSE CODE. THE MAIN SCREEN DECODES IT IN CW MODE; THE SLOWER, THE BETTER IT COPIES."},
    {"SSTV", "SSTV", ACTION_APP_SSTV, -1, -1, f_sstv, N_OF(f_sstv),
     "SLOW-SCAN TV: PICTURES SENT BY HAMS, ONE EVERY MINUTE OR TWO. 14.230 IS THE PLACE."},
    {"NAVTEX", "NavTex", ACTION_APP_NAVTEX, radio_mode_usb, -1, f_navtex, N_OF(f_navtex),
     "WEATHER AND NAVIGATION WARNINGS FOR SHIPS, AS TEXT. NEEDS A LONG WIRE FOR 518 KHZ.", 1000},
    {"WEFAX", "WeFax", ACTION_APP_WEFAX, radio_mode_usb, -1, f_wefax, N_OF(f_wefax),
     "WEATHER MAPS BY RADIO FROM NOAA: BOSTON, NEW ORLEANS, PT REYES. A MAP TAKES 10 MINUTES.", 1500},
    {"JS8", "JS8", ACTION_APP_JS8, radio_mode_usb_dig, -1, f_js8, N_OF(f_js8),
     "KEYBOARD CHAT BUILT ON FT8: SLOW, BUT IT GETS THROUGH. FEW STATIONS ON THE AIR."},
};

#define DECODERS_N N_OF(decoders)

/* The tables' rows, in the monospaced font: their heads take the same format */
#define BAND_ROW "%c%-8.8s %-11.11s %-29.29s %-5.5s %-26.26s %s"
#define DEC_ROW  " %-12s %-9s %-5s %-26.26s %s"     /* C's columns in C's order: 71 of the row's 72 at most */

/* ---- State ---- */

/* The picture */
static lv_img_dsc_t *frame;
static lv_obj_t     *img;
static lv_timer_t   *timer, *blink_timer;
static int           scr_h, img_h, wf_y0;
static bool          wide_ok, rx_held, rec_ours, blink;
static lv_color_t    amb, dim, ink, faint;

/* The spectrum: power average, its dB, the noise around each bin */
static float    raw[MAX_BINS], avg_lin[MAX_BINS], row_lin[MAX_BINS], avg_db[MAX_BINS], floor_db[MAX_BINS];
static uint16_t nbins;
static uint32_t span_hz;
static double   bin_hz;
static int64_t  centre;         /* of the average, Hz; 0: none yet */
static double   wf_centre;      /* of the waterfall's columns */
static uint32_t last_seq;
static int      settle, row_frames, frames_in, since_move, rejected;
static float    floor_med, top_db = -60, bot_db = -130;
static bool     tx_now, broad;

static target_t tg[MAX_TARGETS];
static int      ntg, nlive;

/* F VHF/UHF's listening (an RTL-SDR stick, rtl_spectrum.c) */
enum { VIEW_RTL, VIEW_SPLIT, VIEW_X6200 };      /* cfg_rtl_view: F4 goes RTL, SPLIT, X6200 */

/* The receiver on the speaker: the X6200's own, or the stick at listen_hz (it then has the VFO knob) */
static bool    listen_rtl;
static int64_t listen_hz;
static const char *const rtl_mode_names[] = {"", "NFM", "AM", "WFM"};

/* The stick's demodulator for a frequency: broadcast FM wide, the airband AM, the rest narrow FM */
static int rtl_mode_of(int64_t hz) {
    return hz >= 87500000 && hz < 108000000 ? 3 : hz >= 108000000 && hz < 137000000 ? 2 : 1;
}

/* The sentry */
static scr_t     scr;
static sysmode_t smode;
static routine_t routine;
static int32_t   cursor_hz;
static int64_t   anchor;
static int       sweep_i = -1;
static uint32_t  sweep_next;
static int       logged;

static struct {
    bool     on, held, auto_;
    int32_t  hz;            /* the signal's own frequency */
    ident_t  id;
    uint32_t start, last_seen, last_ms;
    float    heard_s, con, peak;
} trk;

static struct { int32_t hz; uint32_t ms; } visited[VISITED];

/* The contacts: what was tracked a second or more, newest last */
#define LOGS 4
static struct { time_t t; char label[12]; char name[40]; float secs, peak; } logs[LOGS];
static int       nlogs;
static lv_obj_t *t_log_l;
static int     visited_i;
static int32_t ignored[IGNORED];
static int     n_ignored;
static int     band_sel, dec_sel, dec_row;
static int32_t last_tap_hz;
static uint32_t last_tap_ms;

/* The widgets */
static lv_obj_t *root, *scr_obj[SCR_N];

/* The 3D picture under the trace: the last D3_ROWS rows of levels (0-255, one a column), drawn in perspective
 * back to front, as the main screen's 3D band scope: the newest in front, wide and bright, the older ones higher,
 * narrower and dimmer behind, a row hiding what is behind it. Colours from deep orange (the noise) through orange
 * and amber to yellow (the strongest), from the theme's red, accent and pale accent */
#define D3_ROWS 40
static uint8_t  d3_lv[D3_ROWS][IMG_W];
static int      d3_head, d3_count;
static uint32_t d3_pal[256];
static lv_obj_t *badge_l[2], *title_l, *status_l;
typedef struct { lv_obj_t *frame, *cap, *line, *opt[12]; int n; } box_t;     /* opt: E SYSTEM's 9 rows the most */
static box_t     b_mode, b_tx, b_sig, b_rout, b_band, b_prof, b_tgts;
static lv_obj_t *tick_l[TICKS], *tgt_l[LABELS];
static lv_obj_t *t_state_l, *t_crit_frame, *t_crit_l, *t_time_cap_l, *t_big_l, *t_sub_l, *t_time_l, *t_id_l, *t_db_l;
static lv_obj_t *t_sig_fill, *t_noise_fill;
static int       t_bar_y, t_bar_h;
static lv_obj_t *bd_head_l, *bd_row_l[20], *bd_about_l;
static int       bd_rows;
static box_t     d_sel, d_how, d_ant, d_now;
static lv_obj_t *d_head_l, *d_row_l[12];

/* E SYSTEM: the rest of the radio, from here */
typedef enum {
    SYS_SETTINGS, SYS_APPS, SYS_MODES, SYS_AIRSPACE, SYS_VHF, SYS_CLASSIC, SYS_SHELL, SYS_BASEFW, SYS_HOME, SYS_N
} sysitem_t;
static const struct {
    const char *name, *about;
} sys_items[SYS_N] = {
    {"SETTINGS", "EVERY SETTING OF THE RADIO: DISPLAY AND THEME, AUDIO, KEYS, BANDS, NETWORK, TRANSMIT. THEY OPEN IN "
                 "THIS LOOK; CLOSING THEM BRINGS YOU BACK HERE."},
    {"APPS", "THE RADIO'S OTHER APPS: CLUSTER, FIELD OPS, PROPAGATION, CHANNELS, SCANNER, RECORDER, CW TRAINER, GPS, "
             "BIRDIE CAL. CLOSING ONE BRINGS YOU BACK HERE."},
    {"DIGITAL MODES", "THE MODES MENU: FT8, FT4, FT2, JS8, WSPR, PSK, RTTY, JTTY, OLIVIA, SSTV, WEFAX, NAVTEX, RADE. "
                      "D DECODE OPENS THEM ON KNOWN FREQUENCIES."},
    {"AIRSPACE (ADS-B)", "AIRCRAFT ON 1090 MHZ FROM AN ADS-B RECEIVER ON USB (GNS 5890 OR MICROADSB): A SCOPE AROUND "
                         "YOUR QTH, THEIR CALLS, HEIGHTS AND TRACKS, AND THE HISTORY FADING OUT. IT KEEPS LISTENING "
                         "WHEN CLOSED."},
    {"VHF/UHF (RTL-SDR)", "F VHF/UHF: AN RTL-SDR STICK'S SPECTRUM, 24 MHZ TO 1.7 GHZ, ALONE, SPLIT WITH THE X6200'S OR "
                          "THE X6200'S ALONE (F4). F5 OR A TAP ON A SIGNAL PUTS THE STICK ON THE SPEAKER (FM, AM, "
                          "WIDE FM BY BAND) AND GIVES IT THE VFO KNOB. THE MFK PANS, F2 CHOOSES A PRESET. AIRSPACE "
                          "LENDS IT THE STICK MEANWHILE."},
    {"CLASSIC RADIO", "THE FULL RADIO SCREEN, WITH TRANSMIT. THE SENTRY STAYS AWAY TILL YOU OPEN IT AGAIN: APP, THEN "
                      "THE SENTRY TILE."},
    {"UNIX SHELL", "A SHELL ON THE RADIO ITSELF (/BIN/SH, AS ROOT): A KEYBOARD TYPES INTO IT, TYPE OPENS THE ON-SCREEN "
                   "ONE FOR A LINE, CTRL-C INTERRUPTS. CLOSING IT ENDS THE SHELL AND BRINGS YOU BACK HERE."},
#ifdef BOARD_X6200
    {"BASE FIRMWARE", "XIEGU'S X6200 BASE FIRMWARE, ANY RELEASE, FETCHED FROM XIEGU.EU AND WRITTEN FROM HERE AS XIEGU'S "
                      "OWN UPGRADE SCREEN DOES IT, WITH THE KERNEL'S MESSAGES AND THE BASE'S PORT SHOWN AS IT GOES. THE "
                      "BASE SWITCHES THE RADIO OFF A FEW MINUTES AFTER: SWITCH IT ON AGAIN."},
#else
    {"BASE FIRMWARE", "NOT SUPPORTED ON THE X6100: THE BASE FIRMWARE UPDATER IS THE X6200'S ONLY, AND NOTHING OF IT IS IN "
                      "THIS RADIO'S SOFTWARE. THE X6100'S BASE IS WRITTEN ANOTHER WAY, WITH XIEGU'S OWN SYSTEM."},
#endif
    {"START IN", "WHERE THE RADIO STARTS AND COMES BACK TO WHEN AN APP CLOSES: THE SENTRY, OR THE CLASSIC RADIO "
                 "SCREEN. PUSH TO SWITCH."},
};
static int       sys_sel;
static box_t     s_list, s_what, s_look, s_home, s_keys;
static lv_obj_t *key_l[5];

/* The knobs and keys, said: the legend, a word on screen for a moment (OSD), the list of them all (help) */
typedef enum { VF_VOL, VF_SQL, VF_RFG } volfn_t;
static volfn_t     vol_fn;
static lv_obj_t   *legend_l, *osd_frame, *osd_title_l, *osd_text_l, *osd_bar_frame, *osd_bar, *help_frame, *help_l;
static lv_timer_t *osd_timer;
static bool        help_on;

/* ---- Frequencies ---- */

static int64_t dial_centre(void) {
    return (int64_t)cparam_i_get(cfg_fg_freq) - param_i_get(cfg_band_if_shift) + cparam_i_get(cfg_mode_lo_offset);
}

static double px_of(int64_t hz) {
    return (hz - wf_centre) / HZ_PX + IMG_W / 2.0;
}

static int64_t hz_of_px(double x) {
    return (int64_t)llround(centre + (x - IMG_W / 2.0) * HZ_PX);
}

static int bin_of(int64_t hz) {
    return (int)lround((hz - centre) / bin_hz) + nbins / 2;
}

static uint32_t argb(lv_color_t c) {
    return lv_color_to32(c) | 0xff000000;
}

static int band_of(int64_t hz) {
    int32_t khz = (int32_t)(hz / 1000);

    for (int i = 0; i < BANDS_N; i++)
        if (khz >= bands[i].lo && khz <= bands[i].hi) return i;
    return -1;
}

static void fmt_freq(char *s, size_t n, int64_t hz) {
    if (hz < 2000000)
        snprintf(s, n, "%lld", (long long)(hz / 1000));
    else
        snprintf(s, n, "%lld.%03lld", (long long)(hz / 1000000), (long long)((hz / 1000) % 1000));
}

static const char *mode_name(int m) {
    static const char *const names[] = {"LSB", "LSB-D", "USB", "USB-D", "CW", "CW-R", "AM", "NFM", "SAM", "WFM"};
    return m >= 0 && m <= radio_mode_last ? names[m] : "";
}

/* ---- What a signal at hz is, where to tune for it and in what mode ---- */

static const int32_t cb_khz[40] = {
    26965, 26975, 26985, 27005, 27015, 27025, 27035, 27055, 27065, 27075, 27085, 27105, 27115, 27125,
    27135, 27155, 27165, 27175, 27185, 27205, 27215, 27225, 27255, 27235, 27245, 27265, 27275, 27285,
    27295, 27305, 27315, 27325, 27335, 27345, 27355, 27365, 27375, 27385, 27395, 27405};

static const struct { int32_t lo, hi; bool r2_ham; const char *m; } sw_bc[] = {
    {2300, 2495, false, "120 M"}, {3200, 3400, false, "90 M"}, {3900, 4000, true, "75 M"},
    {4750, 5060, false, "60 M"}, {5900, 6200, false, "49 M"}, {7200, 7600, true, "41 M"},
    {9400, 9900, false, "31 M"}, {11600, 12100, false, "25 M"}, {13570, 13870, false, "22 M"},
    {15100, 15800, false, "19 M"}, {17480, 17900, false, "16 M"}, {18900, 19020, false, "15 M"},
    {21450, 21850, false, "13 M"}, {25670, 26100, false, "11 M"}};

static const struct { int32_t lo, hi; const char *name; } ham[] = {
    {1800, 2000, "160 M"}, {3500, 4000, "80 M"}, {5330, 5410, "60 M"}, {7000, 7300, "40 M"},
    {10100, 10150, "30 M"}, {14000, 14350, "20 M"}, {18068, 18168, "17 M"}, {21000, 21450, "15 M"},
    {24890, 24990, "12 M"}, {28000, 29700, "10 M"}, {50000, 54000, "6 M"}};

static const struct { int32_t hz; const char *name; } time_st[] = {
    {2500000, "WWV/WWVH 2.5 MHZ"}, {5000000, "WWV/WWVH 5 MHZ"}, {10000000, "WWV/WWVH 10 MHZ"},
    {15000000, "WWV/WWVH 15 MHZ"}, {20000000, "WWV 20 MHZ"}, {25000000, "WWV 25 MHZ"},
    {3330000, "CHU CANADA 3.330"}, {7850000, "CHU CANADA 7.850"}, {14670000, "CHU CANADA 14.670"}};

/* The AM band's channels: 10 kHz apart in the Americas, 9 elsewhere. The band plan's region is not always the
 * radio's (a plan never chosen is Region 1's), so the strong carriers say: one within 600 Hz of one grid's channel
 * and 900 Hz or more from the other's is a vote for its grid (the picture's bins are 750 Hz, a carrier's peak found
 * within a few hundred Hz). The votes settle it, else the channel the frequency is nearer, else the plan's region */
static int mw_votes10, mw_votes9;

static int32_t mw_step(int64_t hz, bool r2) {
    int64_t e10 = llabs(hz - llround(hz / 10000.0) * 10000);
    int64_t e9  = llabs(hz - llround(hz / 9000.0) * 9000);

    if (mw_votes10 >= mw_votes9 + 3) return 10000;
    if (mw_votes9 >= mw_votes10 + 3) return 9000;
    if (e10 != e9) return e10 < e9 ? 10000 : 9000;     /* the channel it is nearer */
    return r2 ? 10000 : 9000;
}

static void mw_vote(int64_t hz) {
    if (hz < 525000 || hz > 1715000) return;
    int64_t e10 = llabs(hz - llround(hz / 10000.0) * 10000);
    int64_t e9  = llabs(hz - llround(hz / 9000.0) * 9000);

    if (e10 <= 600 && e9 > 900 && mw_votes10 < 1000) mw_votes10++;
    if (e9 <= 600 && e10 > 900 && mw_votes9 < 1000) mw_votes9++;
}

static const int32_t ft8_khz[] = {1840, 3573, 5357, 7074, 10136, 14074, 18100, 21074, 24915, 28074, 50313};

static void identify_ex(int64_t hz, ident_t *id, bool full) {
    int32_t khz = (int32_t)(hz / 1000);
    bool    r2  = band_plan_region() == 2;
    bool    wfm = radio_caps() & HW_CAP_SAM_WFM;

    id->tune_hz = (int32_t)(llround(hz / 100.0) * 100);
    id->mode    = -1;
    snprintf(id->label, sizeof(id->label), "%d", khz);
    snprintf(id->name, sizeof(id->name), "UNKNOWN SIGNAL");

    for (size_t i = 0; i < sizeof(time_st) / sizeof(time_st[0]); i++) {
        if (llabs(hz - time_st[i].hz) <= 1500) {
            id->tune_hz = time_st[i].hz;
            id->mode    = radio_mode_am;
            snprintf(id->label, sizeof(id->label), "TIME");
            snprintf(id->name, sizeof(id->name), "TIME SIGNAL - %s", time_st[i].name);
            return;
        }
    }
    if (hz >= 516000 && hz <= 520000) {
        id->tune_hz = 518000;
        id->mode    = radio_mode_usb;
        snprintf(id->label, sizeof(id->label), "NAVTEX");
        snprintf(id->name, sizeof(id->name), "NAVTEX 518 KHZ - SHIP WEATHER");
        return;
    }
    if (hz >= 525000 && hz <= 1715000) {
        int32_t step = mw_step(hz, r2);
        int32_t ch   = (int32_t)(llround((double)hz / step) * step);

        id->tune_hz = ch;
        id->mode    = radio_mode_am;
        snprintf(id->label, sizeof(id->label), "%d", ch / 1000);
        snprintf(id->name, sizeof(id->name), "AM BROADCAST %d KHZ", ch / 1000);
        return;
    }
    if (hz >= 26960000 && hz <= 27410000) {
        int best = 0;

        for (int i = 1; i < 40; i++)
            if (llabs(hz - cb_khz[i] * 1000LL) < llabs(hz - cb_khz[best] * 1000LL)) best = i;
        if (llabs(hz - cb_khz[best] * 1000LL) <= 3000) {
            id->tune_hz = cb_khz[best] * 1000;
            id->mode    = radio_mode_am;
            snprintf(id->label, sizeof(id->label), "CH%d", best + 1);
            snprintf(id->name, sizeof(id->name), "CB CHANNEL %d%s", best + 1,
                     best + 1 == 9 ? " - EMERGENCY" : best + 1 == 19 ? " - TRUCKERS' CHANNEL" : "");
            return;
        }
    }
    if (hz >= 87500000 && hz <= 108000000) {
        int32_t ch = (int32_t)(llround(hz / 100000.0) * 100000);

        id->tune_hz = ch;
        id->mode    = wfm ? radio_mode_wfm : radio_mode_nfm;
        snprintf(id->label, sizeof(id->label), "%d.%d", ch / 1000000, (ch / 100000) % 10);
        snprintf(id->name, sizeof(id->name), "FM BROADCAST %d.%d MHZ", ch / 1000000, (ch / 100000) % 10);
        return;
    }
    for (size_t i = 0; i < sizeof(ft8_khz) / sizeof(ft8_khz[0]); i++) {
        if (hz >= ft8_khz[i] * 1000LL && hz <= ft8_khz[i] * 1000LL + 3200) {
            id->tune_hz = ft8_khz[i] * 1000;
            id->mode    = radio_mode_usb_dig;
            snprintf(id->label, sizeof(id->label), "FT8");
            snprintf(id->name, sizeof(id->name), "FT8 %d.%03d - DIGITAL, SEE DECODE", ft8_khz[i] / 1000,
                     ft8_khz[i] % 1000);
            return;
        }
    }
    for (size_t i = 0; i < sizeof(sw_bc) / sizeof(sw_bc[0]); i++) {
        if (khz < sw_bc[i].lo || khz > sw_bc[i].hi) continue;
        /* the Americas: 75 m broadcasting and 7.2-7.3 MHz are the hams' phone bands */
        if (r2 && sw_bc[i].r2_ham && ((khz >= 3900 && khz < 4000) || (khz >= 7200 && khz < 7300))) continue;
        int32_t           ch = (int32_t)(llround(hz / 5000.0) * 5000);
        broadcast_match_t m[2];
        size_t            n = full ? broadcast_db_find_matches(ch, 2500, m, 2) : 0;

        id->tune_hz = ch;
        id->mode    = radio_mode_am;
        snprintf(id->label, sizeof(id->label), "%d", ch / 1000);
        if (n > 0)
            snprintf(id->name, sizeof(id->name), "SHORTWAVE %d - %.44s", ch / 1000, m[0].station);
        else
            snprintf(id->name, sizeof(id->name), "SHORTWAVE BROADCAST %s %d KHZ", sw_bc[i].m, ch / 1000);
        for (char *c = id->name; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c -= 32;
        return;
    }
    for (size_t i = 0; i < sizeof(ham) / sizeof(ham[0]); i++) {
        if (khz >= ham[i].lo && khz <= ham[i].hi) {
            const char *how;

            if (band_plan_is_cw((int32_t)hz)) {
                id->mode = radio_mode_cw;
                how      = "CW";
            } else {
                id->mode = hz < 10000000 && !(khz >= 5330 && khz <= 5410) ? radio_mode_lsb : radio_mode_usb;
                how      = id->mode == radio_mode_lsb ? "VOICE LSB" : "VOICE USB";
            }
            snprintf(id->name, sizeof(id->name), "HAM %s - %s", ham[i].name, how);
            return;
        }
    }
}

static void identify(int64_t hz, ident_t *id) {
    identify_ex(hz, id, true);
}

/* ---- Ignored and visited ---- */

static bool is_ignored(int32_t hz) {
    int32_t near = broad ? 60000 : 2500;

    for (int i = 0; i < n_ignored; i++)
        if (labs(ignored[i] - hz) <= near) return true;
    return false;
}

static bool was_visited(int32_t hz) {
    uint32_t now = lv_tick_get();

    for (int i = 0; i < VISITED; i++)
        if (visited[i].ms && now - visited[i].ms < REVISIT_MS && labs(visited[i].hz - hz) <= 2500) return true;
    return false;
}

static void visit(int32_t hz) {
    visited[visited_i].hz = hz;
    visited[visited_i].ms = lv_tick_get() | 1;
    visited_i             = (visited_i + 1) % VISITED;
}

/* ---- The spectrum ---- */

static float median(float *v, int n) {
    int k = n / 2, lo = 0, hi = n - 1;

    while (lo < hi) {
        float p = v[k];
        int   i = lo, j = hi;

        while (i <= j) {
            while (v[i] < p) i++;
            while (v[j] > p) j--;
            if (i <= j) {
                float t = v[i];
                v[i++]  = v[j];
                v[j--]  = t;
            }
        }
        if (j < k) lo = i;
        if (k < i) hi = j;
    }
    return v[k];
}

/* The noise around each bin: the median of the bins +-FLOOR_HALF. FM broadcast (anything 200 kHz wide) would be
 * its own floor there: then the floor is the picture's, the targets 100 kHz apart and placed at their middle */
static void find_floor(void) {
    float w[2 * FLOOR_HALF + 1], all[MAX_BINS];

    broad = centre > 87000000 && centre < 108500000;
    for (int i = 0; i < nbins && !broad; i++) {
        int lo = i - FLOOR_HALF < 0 ? 0 : i - FLOOR_HALF;
        int hi = i + FLOOR_HALF >= nbins ? nbins - 1 : i + FLOOR_HALF;
        int n  = hi - lo + 1;

        memcpy(w, avg_db + lo, n * sizeof(float));
        floor_db[i] = median(w, n);
    }
    int b0 = bin_of(centre - VIEW_HZ / 2), b1 = bin_of(centre + VIEW_HZ / 2), n = 0;

    for (int i = b0 < 0 ? 0 : b0; i <= b1 && i < nbins; i++) all[n++] = avg_db[i];
    if (n) floor_med = median(all, n);
    if (broad)
        for (int i = 0; i < nbins; i++) floor_db[i] = floor_med;
}

/* The bins around a peak within drop dB of it: its width, and its middle by power */
static int peak_extent(int i, float drop, double *mid) {
    int    lo = i, hi = i;
    double s = 0, sw = 0;

    while (lo > 0 && avg_db[lo - 1] > avg_db[i] - drop) lo--;
    while (hi < nbins - 1 && avg_db[hi + 1] > avg_db[i] - drop) hi++;
    for (int k = lo; k <= hi; k++) {
        s += avg_lin[k] * k;
        sw += avg_lin[k];
    }
    *mid = sw > 0 ? s / sw : i;
    return hi - lo + 1;
}

static int cmp_con(const void *a, const void *b) {
    float d = ((const target_t *)b)->con - ((const target_t *)a)->con;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void find_targets(void) {
    static target_t c[MAX_BINS / 2];
    int             nc = 0;
    int             b0 = bin_of(centre - VIEW_HZ / 2) + 1, b1 = bin_of(centre + VIEW_HZ / 2) - 1;

    rejected = 0;
    if (b0 < 1) b0 = 1;
    if (b1 > nbins - 2) b1 = nbins - 2;
    for (int i = b0; i <= b1 && nc < MAX_BINS / 2; i++) {
        float con = avg_db[i] - floor_db[i];

        if (con < TARGET_DB || avg_db[i] < avg_db[i - 1] || avg_db[i] <= avg_db[i + 1]) continue;
        float  a = avg_db[i - 1], b = avg_db[i], d = avg_db[i + 1], den = a - 2 * b + d;
        double at = i + (den != 0 ? 0.5f * (a - d) / den : 0), mid;
        int    w;

        if (broad) {
            w  = peak_extent(i, 8, &mid);
            at = mid;
        } else if ((w = peak_extent(i, 6, &mid)) > 16) {
            rejected++;                     /* wider than 12 kHz at 6 dB down: a hump of noise, not a station */
            continue;
        }
        c[nc++] = (target_t){.hz = (int32_t)llround(centre + (at - nbins / 2) * bin_hz), .db = b, .con = con,
                             .width_khz = (float)(w * bin_hz / 1000)};
    }
    qsort(c, nc, sizeof(c[0]), cmp_con);
    ntg   = 0;
    nlive = 0;
    double sep = broad ? 100000 : TARGET_SEP * bin_hz;
    for (int i = 0; i < nc && ntg < MAX_TARGETS; i++) {
        bool near = false;

        for (int j = 0; j < ntg && !near; j++) near = llabs((int64_t)c[i].hz - tg[j].hz) < sep;
        if (near) continue;
        c[i].ignored = is_ignored(c[i].hz);
        tg[ntg++]    = c[i];
        if (!c[i].ignored) nlive++;
        if (c[i].con >= 12) mw_vote(c[i].hz);
    }
}

static float con_at(int64_t hz, int half) {
    int   b   = bin_of(hz);
    float con = -99;

    for (int i = b - half; i <= b + half; i++)
        if (i >= 0 && i < nbins && avg_db[i] - floor_db[i] > con) con = avg_db[i] - floor_db[i];
    return con;
}

/* What a target sounds like: what it is says first (AM broadcast and CB are voice under their carrier), else its
 * width: a carrier is one or two bins */
static const char *profile(const target_t *t) {
    ident_t id;

    if (broad) return "FM BROADCAST";
    identify_ex(t->hz, &id, false);
    if (!strcmp(id.label, "FT8")) return "DATA";
    if (!strcmp(id.label, "TIME")) return "TIME SIGNAL";
    if (id.mode == radio_mode_am) return "VOICE / AM";
    if (t->width_khz <= 1.6f) return "CARRIER / CW";
    return "VOICE";
}

static const target_t *target_near(int32_t hz, int32_t within) {
    const target_t *best = NULL;

    for (int i = 0; i < ntg; i++)
        if (labs(tg[i].hz - hz) <= within && (!best || labs(tg[i].hz - hz) < labs(best->hz - hz))) best = &tg[i];
    return best;
}

/* ---- Drawing the picture ---- */

static int y_of(float db) {
    int y = SPEC_H - 1 - (int)((db - bot_db) / (top_db - bot_db) * (SPEC_H - 1));
    return y < 0 ? 0 : y > SPEC_H - 1 ? SPEC_H - 1 : y;
}

static void bracket(uint32_t *px, int x, int w, uint32_t c) {
    int x0 = x - w, x1 = x + w;

    for (int y = 3; y < SPEC_H - 2; y++) {
        if (x0 >= 0 && x0 < IMG_W) px[y * IMG_W + x0] = c;
        if (x1 >= 0 && x1 < IMG_W) px[y * IMG_W + x1] = c;
    }
    for (int xx = x0; xx <= x1; xx++) {
        if (xx < 0 || xx >= IMG_W || (xx > x0 + 4 && xx < x1 - 4)) continue;
        px[3 * IMG_W + xx] = px[(SPEC_H - 3) * IMG_W + xx] = c;
    }
}

static uint8_t  level_of(float db);
static uint32_t scale_rgb(uint32_t c, float f);

static void draw_spectrum(void) {
    uint32_t *px   = (uint32_t *)frame->data;
    uint32_t  bg   = argb(ink), grid = argb(faint), fill = argb(dim), line = argb(amb);
    float     top  = -200, want_top, want_bot;
    int       prev = -1;

    for (int i = bin_of(centre - VIEW_HZ / 2); i <= bin_of(centre + VIEW_HZ / 2); i++)
        if (i >= 0 && i < nbins && avg_db[i] > top) top = avg_db[i];
    want_top = top + 4;
    want_bot = floor_med - 8;
    if (want_top < want_bot + 30) want_top = want_bot + 30;
    top_db += (want_top - top_db) * (want_top > top_db ? 0.5f : 0.08f);
    bot_db += (want_bot - bot_db) * 0.1f;

    for (int i = 0; i < (SPEC_H + SCALE_H) * IMG_W; i++) px[i] = bg;
    for (int d = (int)ceilf(bot_db / 10) * 10; d < top_db; d += 10) {
        int y = y_of((float)d);
        for (int x = 0; x < IMG_W; x += 3) px[y * IMG_W + x] = grid;
    }
    for (int x = 0; x < IMG_W; x++) {
        int      b  = bin_of(hz_of_px(x + 0.5));
        float    db = b >= 0 && b < nbins ? avg_db[b] : bot_db;
        int      y  = y_of(db);
        int      y0 = prev < 0 ? y : prev < y ? prev : y, y1 = prev < 0 ? y : prev > y ? prev : y;
        /* coloured by level as the 3D picture's front row: deep orange in the noise, yellow at the top */
        uint8_t  v    = level_of(db);
        uint32_t edge = d3_pal[v], body = scale_rgb(d3_pal[v / 2], 0.45f);

        for (int yy = y + 1; yy < SPEC_H; yy++) px[yy * IMG_W + x] = yy == y + 1 ? scale_rgb(edge, 0.8f) : body;
        for (int yy = y0; yy <= y1; yy++) px[yy * IMG_W + x] = edge;
        prev = y;
    }
    for (int i = 0; i < ntg; i++) {
        int x = (int)px_of(tg[i].hz), y = y_of(tg[i].db);

        if (x < 1 || x >= IMG_W - 1) continue;
        for (int yy = y - 9; yy <= y - 3; yy++)
            if (yy >= 0) px[yy * IMG_W + x - 1] = px[yy * IMG_W + x] = px[yy * IMG_W + x + 1] = tg[i].ignored ? grid : line;
    }
    /* the sights: the one chosen, and the one tracked */
    bracket(px, (int)lround(px_of(cursor_hz)), 14, line);
    if (trk.on) bracket(px, (int)lround(px_of(trk.hz)), 18, line);
    /* the scale: a tick every 50 kHz, a small one every 10 */
    for (int64_t f = (centre - VIEW_HZ / 2) / 10000 * 10000; f <= centre + VIEW_HZ / 2; f += 10000) {
        int x = (int)lround(px_of(f)), h = f % 50000 == 0 ? 6 : 3;

        if (x < 0 || x >= IMG_W) continue;
        for (int yy = SPEC_H; yy < SPEC_H + h; yy++) px[yy * IMG_W + x] = fill;
    }
    /* scan lines, as an old tube */
    for (int y = 2; y < SPEC_H; y += 3) {
        uint32_t *r = px + y * IMG_W;
        for (int x = 0; x < IMG_W; x++) r[x] = 0xff000000 | ((r[x] >> 1) & 0x7f7f7f) | ((r[x] >> 2) & 0x3f3f3f);
    }
}

/* A row's level: the noise (up to 2 dB over the floor) flat, 36 dB over it the top */
static uint8_t level_of(float db) {
    int v = (int)((db - (floor_med + 2)) * 255 / 34);
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static uint32_t scale_rgb(uint32_t c, float f) {
    uint32_t r = (uint32_t)(((c >> 16) & 0xff) * f), g = (uint32_t)(((c >> 8) & 0xff) * f), b = (uint32_t)((c & 0xff) * f);
    return 0xff000000 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
}

static void render_3d(void) {
    uint32_t *px = (uint32_t *)frame->data;
    int       h = img_h - wf_y0, n = d3_count < D3_ROWS ? d3_count : D3_ROWS;
    float     cx = IMG_W / 2.0f;

    for (int i = wf_y0 * IMG_W; i < img_h * IMG_W; i++) px[i] = argb(ink);
    for (int r = n - 1; r >= 0; r--) {                      /* the oldest, at the back, first */
        const uint8_t *lv     = d3_lv[(d3_head - 1 - r + D3_ROWS) % D3_ROWS];
        float          t      = (float)r / (D3_ROWS - 1);
        int            yb     = wf_y0 + h - 2 - (int)(t * h * 0.6f);
        float          shrink = 1.0f - 0.36f * t, amp = h * 0.62f * (1.0f - 0.3f * t), fade = 1.0f - 0.62f * t;
        int            x0     = (int)(cx - cx * shrink), x1 = (int)(cx + cx * shrink);

        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < IMG_W; x++) {
            int sx = (int)(cx + (x - cx) / shrink);

            if (sx < 0 || sx >= IMG_W) continue;
            uint8_t  v    = lv[sx];
            int      top  = yb - (int)(v * amp / 255);
            uint32_t edge = scale_rgb(d3_pal[v], fade), body = scale_rgb(d3_pal[v / 2], fade * 0.45f);

            if (top < wf_y0) top = wf_y0;
            px[top * IMG_W + x] = edge;
            if (top + 1 <= yb) px[(top + 1) * IMG_W + x] = scale_rgb(d3_pal[v], fade * 0.8f);
            for (int y = top + 2; y <= yb; y++) px[y * IMG_W + x] = body;
        }
    }
}

static void wf_clear(void) {
    d3_count = d3_head = 0;
    memset(d3_lv, 0, sizeof(d3_lv));
    render_3d();
}

/* The rows' columns follow the dial: what they showed moves aside */
static void wf_shift(int dx) {
    if (abs(dx) >= IMG_W) {
        wf_clear();
        return;
    }
    for (int r = 0; r < D3_ROWS; r++) {
        uint8_t *row = d3_lv[r];

        if (dx > 0) {
            memmove(row, row + dx, IMG_W - dx);
            memset(row + IMG_W - dx, 0, dx);
        } else if (dx < 0) {
            memmove(row - dx, row, IMG_W + dx);
            memset(row, 0, -dx);
        }
    }
    render_3d();
}

static void wf_row(void) {
    uint8_t *row = d3_lv[d3_head];

    for (int x = 0; x < IMG_W; x++) {
        int b = bin_of(hz_of_px(x + 0.5));
        row[x] = b >= 0 && b < nbins ? level_of(10 * log10f(row_lin[b] / row_frames + 1e-20f)) : 0;
    }
    d3_head = (d3_head + 1) % D3_ROWS;
    if (d3_count < D3_ROWS) d3_count++;
    if (scr == SCR_SCAN) render_3d();
}

/* ---- Widgets ---- */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, lv_color_t c, int x, int y, int w, lv_text_align_t al,
                       const char *s) {
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_obj_set_style_text_align(l, al, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, s);
    return l;
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, int border, bool filled) {
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_border_width(o, border, 0);
    lv_obj_set_style_border_color(o, amb, 0);
    lv_obj_set_style_bg_color(o, filled ? amb : ink, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void tap_cb(lv_event_t *e);

/* A box of the mock: a frame, its caption over a line, n option rows; the options' taps go to tap_cb with tag */
static void box_make(box_t *b, lv_obj_t *parent, int x, int y, int w, int h, const char *cap, int n, int tag) {
    b->frame = rect(parent, x, y, w, h, 3, false);
    b->cap   = label(parent, &pixel_20, amb, x + 3, y + 3, w - 6, LV_TEXT_ALIGN_CENTER, cap);
    b->line  = rect(parent, x, y + 25, w, 3, 0, true);
    if (n > (int)(sizeof(b->opt) / sizeof(b->opt[0]))) n = (int)(sizeof(b->opt) / sizeof(b->opt[0]));
    b->n     = n;
    for (int i = 0; i < n; i++) {
        b->opt[i] = label(parent, &pixel_20, amb, x + 3, y + 30 + i * 20, w - 6, LV_TEXT_ALIGN_CENTER, "");
        lv_obj_set_height(b->opt[i], 20);
        lv_label_set_long_mode(b->opt[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(b->opt[i], amb, 0);
        if (tag >= 0) {
            lv_obj_add_flag(b->opt[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(b->opt[i], tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(tag * 16 + i));
        }
    }
}

/* An option: selected (inverse), plain or off (dim) */
static void opt_set(box_t *b, int i, const char *text, int how) {
    if (i >= b->n) return;
    lv_label_set_text(b->opt[i], text);
    lv_obj_set_style_bg_opa(b->opt[i], how == 1 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(b->opt[i], how == 1 ? ink : how == 2 ? dim : amb, 0);
}

static void cap_set(box_t *b, bool on) {
    lv_obj_set_style_bg_opa(b->cap, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b->cap, amb, 0);
    lv_obj_set_style_text_color(b->cap, on ? ink : amb, 0);
}

static lv_obj_t *screen_obj(void) {
    lv_obj_t *o = lv_obj_create(root);

    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_size(o, SW, scr_h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* ---- Showing the state ---- */

static const char *titles[SCR_N] = {"REMOTE SENTRY RECEIVER SYSTEM", "REMOTE SENTRY RECEIVER SYSTEM",
                                    "BAND INTELLIGENCE", "DECODER SENTRY", "SYSTEM CONTROL", "VHF/UHF SPECTRUM"};

static void show_head(void) {
    char       f[24], t[16], tc[12] = "";
    time_t     now = time(NULL);
    struct tm  tm;
    int32_t    fq  = cparam_i_get(cfg_fg_freq);
    int        c   = clock_cpu_temp();  /* the processor's, the kernel's cpu-thermal zone (sun4i-gpadc-iio) */

    localtime_r(&now, &tm);             /* the radio's time zone (Settings > General > DATE / TIME), named */
    strftime(t, sizeof(t), "%H:%M %Z", &tm);
    if (c != CLOCK_NO_TEMP)             /* after the time, as the other clocks show it; none read: none shown */
        snprintf(tc, sizeof(tc), " %dC", c);
    snprintf(f, sizeof(f), "%d.%03d.%02d", fq / 1000000, (fq / 1000) % 1000, (fq % 1000) / 10);
    static const char *const modes[] = {"AUTO-SCAN", "SEMI-AUTO", "MAN-OVERRIDE"};

    if (scr == SCR_RTL && listen_rtl) {     /* the receiver heard: the stick */
        snprintf(f, sizeof(f), "RTL %lld.%03lld.%02lld", (long long)(listen_hz / 1000000),
                 (long long)(listen_hz / 1000 % 1000), (long long)(listen_hz % 1000 / 10));
        lv_label_set_text_fmt(status_l, "%s %s  %s  %s  %s%s", f, rtl_mode_names[rtl_mode_of(listen_hz)],
                              rx_held ? "RX ONLY" : "TX NOT LOCKED", modes[smode], t, tc);
        return;
    }
    lv_label_set_text_fmt(status_l, "%s %s%s%s  %s  %s  %s%s", f, mode_name(cparam_i_get(cfg_cur_mode)),
                          cparam_i_get(cfg_cur_pre) ? " PRE" : "", cparam_i_get(cfg_cur_att) ? " ATT" : "",
                          rx_held ? "RX ONLY" : "TX NOT LOCKED", modes[smode], t, tc);
}

static void show_scan(void) {
    static const char *const modes[] = {"AUTO-SCAN", "SEMI-AUTO", "MAN-OVERRIDE"};
    char                      s[64];

    for (int i = 0; i < 3; i++) opt_set(&b_mode, i, modes[i], smode == (sysmode_t)i);
    opt_set(&b_tx, 0, rx_held ? "SAFE" : "NOT LOCKED", 1);
    opt_set(&b_tx, 1, "ARMED", 2);
    opt_set(&b_tx, 2, rx_held ? "(LOCKED)" : "", 2);
    opt_set(&b_sig, 0, blink && !trk.on ? "SEARCH_" : "SEARCH", !trk.on);
    opt_set(&b_sig, 1, "TRACK", trk.on);
    opt_set(&b_sig, 2, "DECODE", scr == SCR_DECODE);
    opt_set(&b_rout, 0, "BAND", routine == ROUT_BAND);
    opt_set(&b_rout, 1, "SWEEP", routine == ROUT_SWEEP);
    {
        int n = 0;
        for (int i = 0; i < BANDS_N; i++) n += band_rt[i].sweep;
        if (smode == MODE_MAN)
            snprintf(s, sizeof(s), "KNOB HAS IT");
        else if (routine == ROUT_SWEEP)
            snprintf(s, sizeof(s), "%d BANDS", n);
        else
            snprintf(s, sizeof(s), "360 KHZ");
        opt_set(&b_rout, 2, s, 2);
    }
    cap_set(&b_mode, true);

    int  bi = band_of(centre);
    char a[16], b[16];

    fmt_freq(a, sizeof(a), centre - VIEW_HZ / 2);
    fmt_freq(b, sizeof(b), centre + VIEW_HZ / 2);
    if (bi >= 0)
        snprintf(s, sizeof(s), "%s  %s - %s", bands[bi].name, a, b);
    else
        snprintf(s, sizeof(s), "%s - %s %s", a, b, centre < 2000000 ? "KHZ" : "MHZ");
    opt_set(&b_band, 0, s, 0);
    if (bi == 15) {     /* CB: the channels in the picture */
        int lo = 0, hi = 0;
        for (int i = 0; i < 40; i++)
            if (llabs(cb_khz[i] * 1000LL - centre) <= VIEW_HZ / 2) {
                if (!lo) lo = i + 1;
                hi = i + 1;
            }
        snprintf(s, sizeof(s), "CB CH %d TO %d IN VIEW", lo, hi);
    } else {
        snprintf(s, sizeof(s), "%s", bi >= 0 ? bands[bi].hear : "384 KHZ WIDE");
    }
    opt_set(&b_band, 1, s, 2);

    const target_t *t = target_near(cursor_hz, 3000);

    cap_set(&b_prof, trk.on);
    lv_label_set_text(b_prof.cap, trk.on ? (trk.auto_ ? "LOCKED ON" : "TRACKING") : "SIGNAL PROFILE");
    if (trk.on) {
        bool lost = lv_tick_get() - trk.last_seen > 1500;

        opt_set(&b_prof, 0, trk.id.label, lost ? 2 : 1);
        snprintf(s, sizeof(s), lost ? "%.1f S  LOST" : "%.1f S  +%.0f DB", trk.heard_s, trk.con);
        opt_set(&b_prof, 1, s, 0);
    } else if (t) {
        ident_t id;

        identify_ex(t->hz, &id, false);
        opt_set(&b_prof, 0, profile(t), 1);
        snprintf(s, sizeof(s), "%s  +%.0f DB", id.label, t->con);
        opt_set(&b_prof, 1, s, 0);
    } else {
        opt_set(&b_prof, 0, "NO TARGET", 2);
        opt_set(&b_prof, 1, "MFK: CHOOSE ONE", 2);
    }

    snprintf(s, sizeof(s), "%02d LIVE  /  %02d LOGGED", nlive, logged);
    opt_set(&b_tgts, 0, s, 0);
    snprintf(s, sizeof(s), "NOISE REJECTED: %d", rejected);
    if (n_ignored) snprintf(s + strlen(s), sizeof(s) - strlen(s), "  IGN %d", n_ignored);
    opt_set(&b_tgts, 1, s, 2);
}

static void show_labels(void) {
    int  placed[LABELS], np = 0, t = 0, l = 0;
    char s[24];

    for (int64_t f = ((centre - VIEW_HZ / 2) / 50000 + 1) * 50000; f < centre + VIEW_HZ / 2 && t < TICKS; f += 50000) {
        int x = (int)lround(px_of(f));

        if (x < 24 || x > IMG_W - 24) continue;
        fmt_freq(s, sizeof(s), f);
        lv_label_set_text(tick_l[t], s);
        lv_obj_set_pos(tick_l[t], IMG_X + x - lv_txt_get_width(s, strlen(s), &pixel_16, 0, 0) / 2, lv_obj_get_y(img) + SPEC_H);
        lv_obj_clear_flag(tick_l[t], LV_OBJ_FLAG_HIDDEN);
        t++;
    }
    for (; t < TICKS; t++) lv_obj_add_flag(tick_l[t], LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < ntg && l < LABELS; i++) {
        int     x = (int)px_of(tg[i].hz);
        bool    clash = false;
        ident_t id;

        if (x < 16 || x > IMG_W - 16 || tg[i].ignored) continue;
        for (int j = 0; j < np && !clash; j++) clash = abs(placed[j] - x) < 46;
        if (clash) continue;
        identify_ex(tg[i].hz, &id, false);
        lv_label_set_text(tgt_l[l], id.label);
        int w = lv_txt_get_width(id.label, strlen(id.label), &pixel_16, 0, 0);
        int y = y_of(tg[i].db) - 26;

        lv_obj_set_pos(tgt_l[l], IMG_X + x - w / 2, lv_obj_get_y(img) + (y < 0 ? 0 : y));
        lv_obj_clear_flag(tgt_l[l], LV_OBJ_FLAG_HIDDEN);
        placed[np++] = x;
        l++;
    }
    for (; l < LABELS; l++) lv_obj_add_flag(tgt_l[l], LV_OBJ_FLAG_HIDDEN);
}

static void show_log(void) {
    char s[320] = "RECENT CONTACTS\n";

    for (int i = nlogs - 1; i >= 0; i--) {
        struct tm tm;
        char      t[8];

        gmtime_r(&logs[i].t, &tm);
        strftime(t, sizeof(t), "%H:%M", &tm);
        snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s %-7.7s %5.1fS +%2.0f %.18s\n", t, logs[i].label, logs[i].secs,
                 logs[i].peak, logs[i].name);
    }
    lv_label_set_text(t_log_l, nlogs ? s : "");
}

static void show_track(void) {
    char s[96];

    show_log();

    if (!trk.on) {
        lv_label_set_text(t_state_l, "NO TARGET");
        lv_label_set_text(t_crit_l, "SEARCHING");
        lv_label_set_text(t_big_l, "---");
        lv_label_set_text(t_sub_l, "");
        lv_label_set_text(t_time_l, "0.00");
        lv_label_set_text(t_id_l, "F1 SCAN: THE SENTRY LOCKS ON WHAT IT FINDS");
        lv_label_set_text(t_db_l, "");
        lv_obj_set_height(t_sig_fill, 0);
        return;
    }
    bool lost = lv_tick_get() - trk.last_seen > 1500;

    lv_label_set_text(t_state_l, lost ? "TARGET\nLOST" : trk.auto_ ? "TARGET\nLOCKED" : "TARGET\nTRACKED");
    lv_label_set_text(t_crit_l, lost ? "SIGNAL LOST" : trk.con >= 20 ? "STRONG SIGNAL" : trk.con >= 10 ? "SIGNAL" : "WEAK SIGNAL");
    bool inv = (lost || trk.con >= 20) ? blink : true;
    lv_obj_set_style_bg_opa(t_crit_l, inv ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(t_crit_l, inv ? ink : amb, 0);

    lv_label_set_text(t_big_l, trk.id.label);
    snprintf(s, sizeof(s), "%d.%03d.%d %s", trk.id.tune_hz / 1000000, (trk.id.tune_hz / 1000) % 1000,
             (trk.id.tune_hz % 1000) / 100, mode_name(cparam_i_get(cfg_cur_mode)));
    lv_label_set_text(t_sub_l, s);
    snprintf(s, sizeof(s), "%.2f", trk.heard_s);
    lv_label_set_text(t_time_l, s);
    snprintf(s, sizeof(s), "ID: %s", trk.id.name);
    lv_label_set_text(t_id_l, s);
    snprintf(s, sizeof(s), "+%.0f DB NOW  +%.0f PEAK%s%s", trk.con > 0 ? trk.con : 0, trk.peak, trk.held ? "  HELD" : "",
             recorder_is_on() ? "  REC" : "");
    lv_label_set_text(t_db_l, s);

    float sig = trk.con / 40, noise = (floor_med + 130) / 50;
    sig   = sig < 0 ? 0 : sig > 1 ? 1 : sig;
    noise = noise < 0 ? 0 : noise > 1 ? 1 : noise;
    lv_obj_set_height(t_sig_fill, (int)(sig * t_bar_h));
    lv_obj_set_y(t_sig_fill, t_bar_y + t_bar_h - (int)(sig * t_bar_h));
    lv_obj_set_height(t_noise_fill, (int)(noise * t_bar_h));
    lv_obj_set_y(t_noise_fill, t_bar_y + t_bar_h - (int)(noise * t_bar_h));
}

/* NOW for a band: what the Sentry heard there in the last half hour, else what the sun says at the station's QTH.
 * One set of words on C and D: "3 SIG LIVE", "3 SIG 12 MIN", OPEN, CLOSED, OPENING, CLOSING, LOCAL, SET QTH */
static bool sun_elev(time_t t, double *elev) {
    const char *q = params.qth.x;
    double      lat, lon;

    if (!q || strlen(q) < 4) return false;
    qth_str_to_pos(q, &lat, &lon);
    double n   = t / 86400.0 + 2440587.5 - 2451545.0;
    double L   = fmod(280.460 + 0.9856474 * n, 360);
    double g   = fmod(357.528 + 0.9856003 * n, 360) * M_PI / 180;
    double lam = (L + 1.915 * sin(g) + 0.020 * sin(2 * g)) * M_PI / 180;
    double eps = (23.439 - 0.0000004 * n) * M_PI / 180;
    double ra  = atan2(cos(eps) * sin(lam), cos(lam)), dec = asin(sin(eps) * sin(lam));
    double ha  = fmod(280.46061837 + 360.98564736629 * n + lon, 360) * M_PI / 180 - ra;
    double la  = lat * M_PI / 180;

    *elev = asin(sin(la) * sin(dec) + cos(la) * cos(dec) * cos(ha)) * 180 / M_PI;
    return true;
}

/* BEST, C's and D's column: when a band is at its best (bands[].when) */
static const char *best_name(char when) {
    return when == 'N' ? "NIGHT" : when == 'D' ? "DAY" : when == 'A' ? "ANY" : "LOCAL";
}

static void band_now(int i, char *s, size_t n) {
    double   e, later;
    time_t   t   = time(NULL);
    uint32_t age = band_rt[i].ms ? (lv_tick_get() - band_rt[i].ms) / 60000 : 0;

    if (band_of(centre) == i && nbins) {
        snprintf(s, n, "%d SIG LIVE", nlive);
    } else if (band_rt[i].ms && age < 30) {
        snprintf(s, n, "%d SIG %u MIN", band_rt[i].seen, age);     /* minutes ago: "12M" read as the 12 m band */
    } else if (bands[i].when == 'A') {
        snprintf(s, n, "OPEN");
    } else if (bands[i].when == 'L') {
        snprintf(s, n, "LOCAL");
    } else if (sun_elev(t, &e) && sun_elev(t + 600, &later)) {
        /* A night band (N) is open in the dark and short-range by day; a day band (D) the other way round. Within 6
         * degrees of the horizon it is on its way: a setting sun opens the night bands and closes the day ones */
        bool night = bands[i].when == 'N', rising = later > e;

        if (e < -6)
            snprintf(s, n, "%s", night ? "OPEN" : "CLOSED");
        else if (e > 6)
            snprintf(s, n, "%s", night ? "LOCAL" : "OPEN");
        else
            snprintf(s, n, "%s", rising == night ? "CLOSING" : "OPENING");
    } else {
        snprintf(s, n, "SET QTH");      /* Settings > General: QTH locator */
    }
}

static void show_bands(void) {
    char s[128], now[16];
    int  top = band_sel - bd_rows / 2;

    if (top > BANDS_N - bd_rows) top = BANDS_N - bd_rows;
    if (top < 0) top = 0;
    for (int r = 0; r < bd_rows; r++) {
        int i = top + r;

        if (i >= BANDS_N) {
            lv_label_set_text(bd_row_l[r], "");
            continue;
        }
        char mhz[16];
        if (bands[i].hi < 2000)
            snprintf(mhz, sizeof(mhz), "%d-%d K", bands[i].lo, bands[i].hi);
        else
            snprintf(mhz, sizeof(mhz), "%.1f-%.1f", bands[i].lo / 1000.0, bands[i].hi / 1000.0);
        band_now(i, now, sizeof(now));
        snprintf(s, sizeof(s), BAND_ROW, band_rt[i].sweep ? '*' : ' ',
                 bands[i].name, mhz, bands[i].hear, best_name(bands[i].when), bands[i].ant, now);
        lv_label_set_text(bd_row_l[r], s);
        lv_obj_set_style_bg_opa(bd_row_l[r], i == band_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(bd_row_l[r], i == band_sel ? ink : amb, 0);
    }
    snprintf(s, sizeof(s), "%s: %s", bands[band_sel].name, bands[band_sel].about);
    for (char *c = s; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    lv_label_set_text(bd_about_l, s);
}

static void show_decode(void) {
    const decoder_t *d = &decoders[dec_sel];
    char             s[96], now[16];

    for (int i = 0; i < DECODERS_N && i < d_sel.n; i++) opt_set(&d_sel, i, decoders[i].name, i == dec_sel);
    for (int r = 0; r < 12; r++) {
        if (r >= d->n) {
            lv_label_set_text(d_row_l[r], "");
            lv_obj_set_style_bg_opa(d_row_l[r], LV_OPA_TRANSP, 0);
            continue;
        }
        int32_t f  = d->hz[r];
        int     bi = band_of(f);
        char    fs[16];

        if (f < 1000000)
            snprintf(fs, sizeof(fs), "%d.%d KHZ", f / 1000, (f % 1000) / 100);
        else
            snprintf(fs, sizeof(fs), "%d.%04d", f / 1000000, (f % 1000000) / 100);
        if (bi >= 0)
            band_now(bi, now, sizeof(now));
        else
            snprintf(now, sizeof(now), "NO BAND");     /* a frequency added without one in bands[]: says so */
        snprintf(s, sizeof(s), DEC_ROW, fs, bi >= 0 ? bands[bi].name : "", bi >= 0 ? best_name(bands[bi].when) : "",
                 bi >= 0 ? bands[bi].ant : "", now);
        lv_label_set_text(d_row_l[r], s);
        lv_obj_set_style_bg_opa(d_row_l[r], r == dec_row ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(d_row_l[r], r == dec_row ? ink : amb, 0);
    }
    opt_set(&d_how, 0, d->about, 0);
    snprintf(s, sizeof(s), "%s", d->app == ACTION_APP_FT8    ? "THE FT8 APP"
                                 : d->app == ACTION_APP_WSPR  ? "THE WSPR APP"
                                 : d->app == ACTION_APP_SSTV  ? "THE SSTV APP"
                                 : d->app == ACTION_APP_NAVTEX ? "THE NAVTEX APP"
                                 : d->app == ACTION_APP_WEFAX ? "THE WEFAX APP"
                                 : d->app == ACTION_APP_JS8   ? "THE JS8 APP"
                                                              : "THE MAIN SCREEN");   /* in CW: the text says so */
    opt_set(&d_ant, 0, s, 0);
    opt_set(&d_ant, 1, "F2 OR PRESS: OPEN", 2);
    {
        int bi = dec_row < d->n ? band_of(d->hz[dec_row]) : -1;
        opt_set(&d_now, 0, bi >= 0 ? bands[bi].ant : "", 0);
        opt_set(&d_now, 1, bi >= 0 ? bands[bi].name : "", 2);
    }
}

static void show_system(void) {
    char s[48];
    bool home = param_i_get(cfg_sentry_home);

    for (int i = 0; i < SYS_N; i++) {
        if (i == SYS_HOME)
            snprintf(s, sizeof(s), "START IN: %s", home ? "SENTRY" : "CLASSIC");
        else
            snprintf(s, sizeof(s), "%s", sys_items[i].name);
        opt_set(&s_list, i, s, i == sys_sel);
    }
    opt_set(&s_what, 0, sys_items[sys_sel].about, 0);
    snprintf(s, sizeof(s), "%s THEME", params.theme_name.x);
    for (char *c = s; *c; c++) *c = (char)toupper((unsigned char)*c);
    opt_set(&s_look, 0, s, 0);
    opt_set(&s_look, 1, "SETTINGS > DISPLAY", 2);
    opt_set(&s_home, 0, home ? "THE SENTRY" : "CLASSIC RADIO", 0);
    opt_set(&s_home, 1, home ? "APPS COME BACK HERE" : "APPS GO TO CLASSIC", 2);
    opt_set(&s_keys, 0, "APP: THIS SCREEN", 0);
    opt_set(&s_keys, 1, "F5: CLASSIC RADIO", 2);
}

/* ---- F VHF/UHF: an RTL-SDR stick's spectrum (rtl_spectrum.c), alone or under the X6200's ---- */

#define RTL_COLS     IMG_W
#define RTL_TICKS    9
#define RTL_PEAKS    6
#define RTL_TRACE_H  90         /* the stick alone: its trace, the scale, the rest its waterfall */
#define RTL_HALF_TR  46         /* split: each half's trace */
#define RTL_SCALE_H  16

static const struct {
    const char *name;
    int32_t     hz;
    const char *about;
} rtl_presets[] = {
    {"NOAA WX", 162475000, "WEATHER RADIO 162.400-.550, AIS 161.975 / 162.025"},
    {"2 M", 145000000, "HAMS 144-146: APRS 144.390, THE ISS 145.800"},
    {"2 M RPT", 147000000, "HAMS 146-148: REPEATERS AND SIMPLEX"},
    {"AIRBAND", 121000000, "AIRCRAFT VOICE (AM) 120-122, GUARD 121.500"},
    {"FM BC", 98000000, "FM BROADCAST 97-99 MHZ"},
    {"MARINE", 156800000, "MARINE VHF, CALLING CHANNEL 16 156.800"},
    {"WX SAT", 137500000, "METEOR-M WEATHER SATELLITES 137.100 / 137.900"},
    {"SONDES", 403000000, "WEATHER BALLOONS 400-406 MHZ"},
    {"70 CM", 446000000, "HAMS 445-447: SIMPLEX 446.000"},
    {"ISM 433", 433920000, "SENSORS, KEY FOBS, WEATHER STATIONS"},
    {"GMRS", 462600000, "GMRS AND FRS RADIOS"},
    {"ISM 915", 915000000, "METERS AND SENSORS 914-916 MHZ"},
    {"UAT 978", 978000000, "AIRCRAFT UAT, 978 MHZ"},
    {"ADS-B", 1090000000, "AIRCRAFT, 1090 MHZ (AIRSPACE DECODES THEM)"},
};
#define RTL_PRESETS N_OF(rtl_presets)

static const int32_t rtl_steps[] = {12500, 25000, 100000, 500000, 1000000};

static const int     rtl_gains[] = {-1, 97, 197, 280, 372, 496};     /* tenths of dB; -1 the tuner's AGC */

/* A half of the picture: its trace, the scale under it and the 3D picture of its last rows, as A SCAN's */
typedef struct {
    int   y0, trace_h, wf_y0, wf_h;
    float floor, top_db, bot_db;
    uint8_t (*rows)[RTL_COLS];      /* D3_ROWS rows of levels, newest at head - 1 */
    int   head, count;
    float pend[RTL_COLS];           /* frames not yet a row: their strongest */
    int   npend;
} panel_t;

static uint8_t rows_rtl[D3_ROWS][RTL_COLS], rows_hf[D3_ROWS][RTL_COLS];

static lv_img_dsc_t *rtl_frame;
static lv_obj_t     *rtl_img, *rtl_why_l, *rtl_tick_l[RTL_TICKS], *hf_tick_l[RTL_TICKS], *rtl_peak_l[RTL_PEAKS], *rtl_src_l[2];
static box_t         r_dev, r_tune, r_pre, r_sig;
static lv_timer_t   *rtl_timer;
static int           rtl_img_h, rtl_preset, rtl_step = 2, rtl_drop;
static panel_t       p_rtl, p_hf;
static float         rtl_db[8192], rtl_cols[RTL_COLS], hf_cols[RTL_COLS];
static uint16_t      rtl_n;
static uint32_t      rtl_rate, rtl_seq, hf_seen;
static int64_t       rtl_centre, rtl_drawn;      /* the stick's tuning, what the waterfall's columns show */
static bool          rtl_wanted;

static int64_t rtl_view_hz(void) {
    return rtl_rate ? (int64_t)rtl_rate * 5 / 6 : 2000000;   /* the middle of the band: the edges roll off */
}

static double rtl_px_hz(void) {
    return (double)rtl_view_hz() / RTL_COLS;
}

/* A NOAA Weather Radio channel's own frequency for one within 6 kHz of it, 0 for any other */
static int64_t rtl_wx_hz(int64_t hz) {
    static const int32_t wx[] = {162400000, 162425000, 162450000, 162475000, 162500000, 162525000, 162550000};

    for (int i = 0; i < 7; i++)
        if (llabs(hz - wx[i]) <= 6000) return wx[i];
    return 0;
}

/* What a VHF/UHF frequency is, in a few letters, "" not known */
static const char *rtl_name(int64_t hz) {
    static const struct { int32_t hz; int32_t w; const char *n; } exact[] = {
        {162550000, 6000, "WX1"}, {162400000, 6000, "WX2"}, {162475000, 6000, "WX3"}, {162425000, 6000, "WX4"},
        {162450000, 6000, "WX5"}, {162500000, 6000, "WX6"}, {162525000, 6000, "WX7"}, {161975000, 6000, "AIS 1"},
        {162025000, 6000, "AIS 2"}, {144390000, 6000, "APRS"}, {145800000, 6000, "ISS"}, {156800000, 6000, "CH16"},
        {121500000, 6000, "GUARD"}, {137100000, 40000, "METEOR"}, {137900000, 40000, "METEOR"},
        {1090000000, 1500000, "ADS-B"}, {978000000, 1000000, "UAT"}, {146520000, 6000, "CALL"},
        {446000000, 6000, "CALL"},
    };
    static const struct { int32_t lo, hi; const char *n; } ranges[] = {
        {88000000, 108000000, "FM"}, {108000000, 118000000, "NAV"}, {118000000, 137000000, "AIR"},
        {137000000, 138000000, "SAT"}, {144000000, 148000000, "2M"}, {156000000, 157450000, "MARINE"},
        {150800000, 174000000, "LMR"}, {400000000, 406000000, "SONDE"}, {433050000, 434790000, "ISM"},
        {420000000, 450000000, "70CM"}, {462550000, 467725000, "GMRS"}, {902000000, 928000000, "ISM"},
    };

    for (int i = 0; i < N_OF(exact); i++)
        if (llabs(hz - exact[i].hz) <= exact[i].w) return exact[i].n;
    for (int i = 0; i < N_OF(ranges); i++)
        if (hz >= ranges[i].lo && hz < ranges[i].hi) return ranges[i].n;
    return "";
}

/* Columns from bins (n of them, the first at lo_hz, bin_hz apart): each the strongest bin in it, so a narrow
 * carrier stays in sight; an empty one from its neighbour */
static void to_cols(const float *db, int n, double lo_hz, double bin_hz, int64_t view_lo, double px_hz, float *cols) {
    for (int x = 0; x < RTL_COLS; x++) {
        double f0 = view_lo + x * px_hz, f1 = f0 + px_hz;
        int    b0 = (int)floor((f0 - lo_hz) / bin_hz), b1 = (int)ceil((f1 - lo_hz) / bin_hz);
        float  best = -300;

        if (b1 <= b0) b1 = b0 + 1;
        for (int b = b0; b < b1; b++)
            if (b >= 0 && b < n && db[b] > best) best = db[b];
        cols[x] = best > -300 ? best : (x ? cols[x - 1] : -150);
    }
}

static float cols_floor(const float *cols) {
    static float tmp[RTL_COLS];

    memcpy(tmp, cols, sizeof(tmp));
    return median(tmp, RTL_COLS);
}

static uint8_t panel_level(const panel_t *p, float db) {
    int v = (int)((db - (p->floor + 2)) * 255 / 34);
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* The trace: coloured by level, as A SCAN's */
static void panel_trace(panel_t *p, const float *cols) {
    uint32_t *px  = (uint32_t *)rtl_frame->data;
    uint32_t  bg  = argb(ink), grid = argb(faint);
    float     top = -300;
    int       h   = p->trace_h, prev = -1;

    for (int x = 0; x < RTL_COLS; x++)
        if (cols[x] > top) top = cols[x];
    p->floor = p->floor < -250 ? cols_floor(cols) : p->floor * 0.8f + cols_floor(cols) * 0.2f;
    float want_top = top + 4 > p->floor + 30 ? top + 4 : p->floor + 30, want_bot = p->floor - 8;
    p->top_db += (want_top - p->top_db) * (want_top > p->top_db ? 0.5f : 0.08f);
    p->bot_db += (want_bot - p->bot_db) * 0.1f;

    for (int i = p->y0 * RTL_COLS; i < (p->y0 + h + RTL_SCALE_H) * RTL_COLS; i++) px[i] = bg;
    for (int d = (int)ceilf(p->bot_db / 10) * 10; d < p->top_db; d += 10) {
        int y = h - 1 - (int)((d - p->bot_db) / (p->top_db - p->bot_db) * (h - 1));
        if (y >= 0 && y < h)
            for (int x = 0; x < RTL_COLS; x += 3) px[(p->y0 + y) * RTL_COLS + x] = grid;
    }
    for (int x = 0; x < RTL_COLS; x++) {
        int y = h - 1 - (int)((cols[x] - p->bot_db) / (p->top_db - p->bot_db) * (h - 1));
        y     = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
        int      y0 = prev < 0 ? y : prev < y ? prev : y, y1 = prev < 0 ? y : prev > y ? prev : y;
        uint8_t  v    = panel_level(p, cols[x]);
        uint32_t edge = 0xff000000 | d3_pal[v], body = scale_rgb(d3_pal[v / 2], 0.45f);

        for (int yy = y + 1; yy < h; yy++) px[(p->y0 + yy) * RTL_COLS + x] = yy == y + 1 ? scale_rgb(edge, 0.8f) : body;
        for (int yy = y0; yy <= y1; yy++) px[(p->y0 + yy) * RTL_COLS + x] = edge;
        prev = y;
    }
    for (int y = 2; y < h; y += 3) {        /* scan lines, as an old tube */
        uint32_t *r = px + (p->y0 + y) * RTL_COLS;
        for (int x = 0; x < RTL_COLS; x++) r[x] = 0xff000000 | ((r[x] >> 1) & 0x7f7f7f) | ((r[x] >> 2) & 0x3f3f3f);
    }
}

/* The 3D picture of the half's rows, back to front: A SCAN's render_3d in the half's own place */
static void panel_render(const panel_t *p) {
    uint32_t *px = (uint32_t *)rtl_frame->data, bg = argb(ink);
    int       h = p->wf_h, y0 = p->wf_y0, n = p->count;
    float     cx = RTL_COLS / 2.0f;

    if (h < 8) return;
    for (int i = y0 * RTL_COLS; i < (y0 + h) * RTL_COLS; i++) px[i] = bg;
    for (int r = n - 1; r >= 0; r--) {
        const uint8_t *lv     = p->rows[(p->head - 1 - r + D3_ROWS) % D3_ROWS];
        float          t      = (float)r / (D3_ROWS - 1);
        int            yb     = y0 + h - 2 - (int)(t * h * 0.6f);
        float          shrink = 1.0f - 0.36f * t, amp = h * 0.62f * (1.0f - 0.3f * t), fade = 1.0f - 0.62f * t;
        int            x0 = (int)(cx - cx * shrink), x1 = (int)(cx + cx * shrink);

        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < RTL_COLS; x++) {
            int sx = (int)(cx + (x - cx) / shrink);

            if (sx < 0 || sx >= RTL_COLS) continue;
            uint8_t  v    = lv[sx];
            int      top  = yb - (int)(v * amp / 255);
            uint32_t edge = scale_rgb(d3_pal[v], fade), body = scale_rgb(d3_pal[v / 2], fade * 0.45f);

            if (top < y0) top = y0;
            px[top * RTL_COLS + x] = edge;
            if (top + 1 <= yb) px[(top + 1) * RTL_COLS + x] = scale_rgb(d3_pal[v], fade * 0.8f);
            for (int y = top + 2; y <= yb; y++) px[y * RTL_COLS + x] = body;
        }
    }
}

/* A frame into the half's rows: every second one makes a row (the strongest of the two), drawn at once */
static void panel_row(panel_t *p, const float *cols) {
    for (int x = 0; x < RTL_COLS; x++) p->pend[x] = p->npend ? fmaxf(p->pend[x], cols[x]) : cols[x];
    if (++p->npend < 2) return;
    p->npend = 0;

    uint8_t *row = p->rows[p->head];
    for (int x = 0; x < RTL_COLS; x++) row[x] = panel_level(p, p->pend[x]);
    p->head = (p->head + 1) % D3_ROWS;
    if (p->count < D3_ROWS) p->count++;
    panel_render(p);
}

/* The rows' columns follow a retune: what they showed moves aside, or goes */
static void panel_shift(panel_t *p, int dx) {
    for (int r = 0; r < D3_ROWS; r++) {
        uint8_t *row = p->rows[r];

        if (abs(dx) >= RTL_COLS) {
            memset(row, 0, RTL_COLS);
        } else if (dx > 0) {
            memmove(row, row + dx, RTL_COLS - dx);
            memset(row + RTL_COLS - dx, 0, dx);
        } else if (dx < 0) {
            memmove(row - dx, row, RTL_COLS + dx);
            memset(row, 0, -dx);
        }
    }
    p->npend = 0;
    panel_render(p);
}

/* The halves: the stick alone fills the picture; split, the X6200 above and the stick below */
static void rtl_layout(void) {
    int  view  = param_i_get(cfg_rtl_view);
    bool split = view == VIEW_SPLIT;
    int  h     = rtl_img_h;

    p_hf = (panel_t){.rows = rows_hf};
    memset(rows_hf, 0, sizeof(rows_hf));
    memset(rows_rtl, 0, sizeof(rows_rtl));
    if (split) {
        int half = (h - 4) / 2;

        p_hf  = (panel_t){.y0 = 0, .trace_h = RTL_HALF_TR, .wf_y0 = RTL_HALF_TR + RTL_SCALE_H,
                          .wf_h = half - RTL_HALF_TR - RTL_SCALE_H, .floor = -300, .top_db = -60, .bot_db = -130,
                          .rows = rows_hf};
        p_rtl = (panel_t){.y0 = half + 4, .trace_h = RTL_HALF_TR, .wf_y0 = half + 4 + RTL_HALF_TR + RTL_SCALE_H,
                          .wf_h = h - (half + 4 + RTL_HALF_TR + RTL_SCALE_H), .floor = -300, .top_db = -60, .bot_db = -130,
                          .rows = rows_rtl};
    } else if (view == VIEW_X6200) {        /* the X6200 alone: its half the whole picture, the stick's none */
        p_hf  = (panel_t){.y0 = 0, .trace_h = RTL_TRACE_H, .wf_y0 = RTL_TRACE_H + RTL_SCALE_H,
                          .wf_h = h - RTL_TRACE_H - RTL_SCALE_H, .floor = -300, .top_db = -60, .bot_db = -130,
                          .rows = rows_hf};
        p_rtl = (panel_t){.y0 = h, .rows = rows_rtl};
    } else {
        p_rtl = (panel_t){.y0 = 0, .trace_h = RTL_TRACE_H, .wf_y0 = RTL_TRACE_H + RTL_SCALE_H,
                          .wf_h = h - RTL_TRACE_H - RTL_SCALE_H, .floor = -300, .top_db = -60, .bot_db = -130,
                          .rows = rows_rtl};
    }
    uint32_t *px = (uint32_t *)rtl_frame->data, bg = argb(ink), line = argb(amb);
    for (int i = 0; i < RTL_COLS * h; i++) px[i] = bg;
    if (split)
        for (int y = p_rtl.y0 - 3; y < p_rtl.y0 - 1; y++)
            for (int x = 0; x < RTL_COLS; x++) px[y * RTL_COLS + x] = line;
    rtl_drawn = 0;
    lv_label_set_text(rtl_src_l[0], "");
    lv_label_set_text(rtl_src_l[1], "");
    for (int i = 0; i < RTL_TICKS; i++) {
        lv_obj_add_flag(hf_tick_l[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rtl_tick_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < RTL_PEAKS; i++) lv_obj_add_flag(rtl_peak_l[i], LV_OBJ_FLAG_HIDDEN);
    if (rtl_img) lv_obj_invalidate(rtl_img);
}

static void fmt_mhz(char *s, size_t n, int64_t hz, int dec) {
    snprintf(s, n, "%lld.%0*lld", (long long)(hz / 1000000), dec, (long long)((hz % 1000000) / (dec == 3 ? 1000 : 100)));
}

/* The scale's labels: a tick label every step that leaves room for them */
static void rtl_ticks(lv_obj_t **l, const panel_t *p, int64_t lo, double px_hz, int64_t every, bool mhz) {
    int  t = 0, iy = lv_obj_get_y(rtl_img);
    char s[24];

    for (int64_t f = (lo / every + 1) * every; f < lo + (int64_t)(px_hz * RTL_COLS) && t < RTL_TICKS; f += every) {
        int x = (int)lround((f - lo) / px_hz);

        if (x < 30 || x > RTL_COLS - 30) continue;
        if (mhz) fmt_mhz(s, sizeof(s), f, 3);
        else fmt_freq(s, sizeof(s), f);
        lv_label_set_text(l[t], s);
        lv_obj_set_pos(l[t], IMG_X + x - lv_txt_get_width(s, strlen(s), &pixel_16, 0, 0) / 2, iy + p->y0 + p->trace_h);
        lv_obj_clear_flag(l[t], LV_OBJ_FLAG_HIDDEN);
        t++;
    }
    for (; t < RTL_TICKS; t++) lv_obj_add_flag(l[t], LV_OBJ_FLAG_HIDDEN);
}

/* The strongest peaks over the stick's floor, labelled with their frequency and what they are */
static int rtl_peaks(int64_t lo, double px_hz, int32_t *best_hz, float *best_db, int64_t *wx_hz) {
    int   placed[RTL_PEAKS], np = 0, iy = lv_obj_get_y(rtl_img), found = 0;
    bool  used[RTL_COLS] = {false};
    char  s[32], f[16];

    *best_hz = 0;
    *best_db = -300;
    *wx_hz   = 0;
    float wx_db = -300;
    for (int k = 0; k < RTL_PEAKS * 3 && np < RTL_PEAKS; k++) {
        int   bx = -1;
        float bv = p_rtl.floor + 10;

        for (int x = 2; x < RTL_COLS - 2; x++)
            if (!used[x] && rtl_cols[x] > bv && rtl_cols[x] >= rtl_cols[x - 1] && rtl_cols[x] >= rtl_cols[x + 1]) {
                bv = rtl_cols[x];
                bx = x;
            }
        if (bx < 0) break;
        for (int x = bx - 12; x <= bx + 12; x++)
            if (x >= 0 && x < RTL_COLS) used[x] = true;
        found++;
        int64_t hz = lo + (int64_t)((bx + 0.5) * px_hz);

        /* A broadcast FM station is a hump 200 kHz wide, its strongest column anywhere on it: its middle, to its
         * channel */
        if (rtl_mode_of(hz) == 3) {
            int l = bx, r = bx;

            while (l > 0 && rtl_cols[l - 1] > bv - 10) l--;
            while (r < RTL_COLS - 1 && rtl_cols[r + 1] > bv - 10) r++;
            hz = rtl_snap(lo + (int64_t)(((l + r) / 2.0 + 0.5) * px_hz));
            bx = (int)lround((hz - lo) / px_hz);
        }
        if (bv > *best_db) *best_db = bv, *best_hz = (int32_t)hz;
        if (rtl_wx_hz(hz) && bv > wx_db) wx_db = bv, *wx_hz = rtl_wx_hz(hz);    /* the strongest NOAA channel */

        bool clash = false;
        for (int j = 0; j < np && !clash; j++) clash = abs(placed[j] - bx) < 70;
        if (clash) continue;
        fmt_mhz(f, sizeof(f), hz, 3);
        snprintf(s, sizeof(s), "%s%s%s", f, rtl_name(hz)[0] ? " " : "", rtl_name(hz));
        int w = lv_txt_get_width(s, strlen(s), &pixel_16, 0, 0), x = IMG_X + bx - w / 2;
        int y = p_rtl.trace_h - 1 - (int)((bv - p_rtl.bot_db) / (p_rtl.top_db - p_rtl.bot_db) * (p_rtl.trace_h - 1)) - 20;

        x = x < IMG_X ? IMG_X : x + w > IMG_X + RTL_COLS ? IMG_X + RTL_COLS - w : x;
        if (y < 22 && x + w > IMG_X + RTL_COLS - 250) y = 22;       /* clear of the half's name at the top right */
        lv_label_set_text(rtl_peak_l[np], s);
        lv_obj_set_pos(rtl_peak_l[np], x, iy + p_rtl.y0 + (y < 0 ? 0 : y));
        lv_obj_clear_flag(rtl_peak_l[np], LV_OBJ_FLAG_HIDDEN);
        placed[np++] = bx;
    }
    for (int i = np; i < RTL_PEAKS; i++) lv_obj_add_flag(rtl_peak_l[i], LV_OBJ_FLAG_HIDDEN);
    return found;
}

static void show_rtl(void) {
    static const char *const st[] = {"OFF", "NO HELPER ON THIS IMAGE", "NO RTL-SDR ON USB", "STICK BUSY: WAITING",
                                     "OPENING THE STICK", "RECEIVING", "STICK SENDS NOTHING"};
    rtl_spec_state_t s = rtl_spectrum_state();
    char             t[48], tn[16], f[16];
    int              g = param_i_get(cfg_rtl_gain);

    if (listen_rtl && (s == RTL_SPEC_NO_STICK || s == RTL_SPEC_NO_HELPER || s == RTL_SPEC_STALLED)) {
        rtl_listen(false);
        osd("BACK TO THE X6200", "THE RTL-SDR WENT: THE SPEAKER AND THE VFO KNOB ARE THE X6200'S AGAIN.", -1);
    }
    rtl_spectrum_tuner(tn, sizeof(tn));
    snprintf(t, sizeof(t), "RTL-SDR %s", tn[0] ? tn : "");
    opt_set(&r_dev, 0, t, 0);
    if (s == RTL_SPEC_ON) snprintf(t, sizeof(t), "HEARD: %s", listen_rtl ? "RTL-SDR" : "X6200");
    opt_set(&r_dev, 1, s == RTL_SPEC_ON ? t : st[s], s == RTL_SPEC_ON ? (listen_rtl ? 1 : 2) : 0);
    fmt_mhz(f, sizeof(f), param_i_get(cfg_rtl_freq), 3);
    snprintf(t, sizeof(t), "%s MHZ", f);
    opt_set(&r_tune, 0, t, 0);
    static const char *const steps[] = {"12.5K", "25K", "100K", "500K", "1M"};
    snprintf(t, sizeof(t), "%.1f MHZ / %s", rtl_view_hz() / 1e6, steps[rtl_step]);
    opt_set(&r_tune, 1, t, 2);
    opt_set(&r_pre, 0, rtl_presets[rtl_preset].name, 0);
    opt_set(&r_pre, 1, rtl_presets[rtl_preset].about, 2);
    if (g < 0) snprintf(t, sizeof(t), "GAIN AUTO");
    else snprintf(t, sizeof(t), "GAIN %d.%d DB", g / 10, g % 10);
    opt_set(&r_sig, 1, t, 2);
    /* No rows: why, in the picture */
    static const char *const why[] = {"", "THIS IMAGE HAS NO /USR/BIN/X6X00-RTLSPEC", "NO RTL-SDR ON USB\n\nPLUG ONE INTO THE HOST PORT",
                                      "THE STICK IS BUSY\n\nWAITING FOR IT", "OPENING THE RTL-SDR", "",
                                      "THE RTL-SDR SENDS NOTHING\n\nUNPLUG IT AND PLUG IT IN AGAIN"};
    lv_label_set_text(rtl_why_l, why[s]);
    if (s == RTL_SPEC_ON || s == RTL_SPEC_OFF) lv_obj_add_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
}

/* The stick's tuning, kept and sent */
static void rtl_tune(int64_t hz) {
    if (hz < RTL_SPEC_MIN_HZ) hz = RTL_SPEC_MIN_HZ;
    if (hz > RTL_SPEC_MAX_HZ) hz = RTL_SPEC_MAX_HZ;
    param_i_set(cfg_rtl_freq, (int32_t)hz);
    rtl_spectrum_tune(hz);
    show_rtl();
}

static int64_t rtl_best;      /* the strongest signal of the last row, 0 none */

/* A NOAA Weather Radio alert (SAME, rtl_spectrum_alert): shown in the title until it expires */
static uint32_t alert_seq;
static char     alert_event[48];
static time_t   alert_until;

static void show_title(void);

/* A SAME header said: its event in the title for its duration (15 minutes at least), the rest in a pop-up */
static void alert_take(const char *text) {
    char        h[300], code[4] = "", sender[12] = "", what[48];
    int         areas = 0, dur = 0;
    const char *name = strchr(text, '|');

    snprintf(h, sizeof(h), "%.*s", name ? (int)(name - text) : (int)strlen(text), text);
    name = name ? name + 1 : NULL;
    const char *plus = strchr(h, '+');

    if (strncmp(h, "ZCZC-", 5) != 0 || strlen(h) < 16 || !plus) return;
    memcpy(code, h + 9, 3);                                 /* ZCZC-ORG-EEE-... */
    for (const char *q = h + 13; q < plus; q++) areas += *q == '-';
    areas++;
    if (strlen(plus) >= 5) dur = (plus[1] - '0') * 600 + (plus[2] - '0') * 60 + (plus[3] - '0') * 10 + (plus[4] - '0');
    const char *s1 = plus + 1;                              /* +TTTT-JJJHHMM-LLLLLLLL- */
    for (int i = 0; i < 2 && s1; i++) s1 = strchr(s1, '-') ? strchr(s1, '-') + 1 : NULL;
    if (s1) snprintf(sender, sizeof(sender), "%.8s", s1);
    snprintf(alert_event, sizeof(alert_event), "%s", name ? name : code);
    alert_until = time(NULL) + 60 * (dur > 15 ? dur : 15);
    snprintf(what, sizeof(what), "%s, %d AREA%s, %d MIN", sender, areas, areas == 1 ? "" : "S", dur);
    char title[64];
    snprintf(title, sizeof(title), "WX ALERT: %s", alert_event);
    osd(title, what, -1);
    show_title();
}

/* A frequency to the channels of its band: FM broadcast 100 kHz, the airband 25 kHz, the rest 12.5 kHz */
static int64_t rtl_step_of(int64_t hz) {
    int m = rtl_mode_of(hz);
    return m == 3 ? 100000 : m == 2 ? 25000 : 12500;
}

static int64_t rtl_snap(int64_t hz) {
    int64_t st = rtl_step_of(hz);
    return (hz + st / 2) / st * st;
}

/* The receiver on the speaker: the stick at listen_hz (with the VFO knob), or the X6200 again */
static void rtl_listen(bool on) {
    listen_rtl = on && rtl_wanted;
    if (listen_rtl) {
        int64_t c = param_i_get(cfg_rtl_freq);

        if (!listen_hz || llabs(listen_hz - c) > rtl_view_hz() / 2) listen_hz = rtl_snap(rtl_best ? rtl_best : c);
        rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
    } else {
        rtl_spectrum_listen(0, 0);
    }
    show_head();
}

/* The VFO knob while the stick is listened to: a channel a click; the stick follows when it nears an edge */
static void rtl_listen_turn(int32_t diff) {
    int64_t c = param_i_get(cfg_rtl_freq), half = rtl_view_hz() / 2;

    listen_hz = rtl_snap(listen_hz + diff * rtl_step_of(listen_hz));
    if (listen_hz < RTL_SPEC_MIN_HZ) listen_hz = RTL_SPEC_MIN_HZ;
    if (listen_hz > RTL_SPEC_MAX_HZ) listen_hz = RTL_SPEC_MAX_HZ;
    if (llabs(listen_hz - c) > half * 85 / 100) rtl_tune(listen_hz);
    rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
    show_head();
}

static bool knob_taken(void) {
    return scr == SCR_RTL && listen_rtl;
}

static void rtl_want(bool on) {
    if (on == rtl_wanted) return;
    if (!on && listen_rtl) {
        listen_rtl = false;
        rtl_spectrum_listen(0, 0);      /* the X6200 on the speaker again */
    }
    rtl_wanted = on;
    rtl_spectrum_want(on, param_i_get(cfg_rtl_freq), param_i_get(cfg_rtl_gain));
    if (on) {
        rtl_seq = 0;
        rtl_drop = 2;
        rtl_layout();
    }
}

static void rtl_timer_cb(lv_timer_t *t) {
    uint16_t n;
    uint32_t rate;
    int64_t  c;
    bool     fresh, drew = false;
    int32_t  best_hz;
    float    best_db;
    char     s[48], f[16];

    (void)t;
    {   /* a weather alert, whatever the screen */
        char   h[300];
        time_t at;

        if (rtl_spectrum_alert(h, sizeof(h), &at, &alert_seq)) alert_take(h);
        static time_t titled;
        if (alert_until && time(NULL) != titled) {
            titled = time(NULL);
            if (titled >= alert_until) alert_until = 0;
            show_title();
        }
    }
    if (scr != SCR_RTL) return;
    static uint32_t shown_ms;
    if (lv_tick_elaps(shown_ms) > 1000) {   /* the boxes, once a second */
        shown_ms = lv_tick_get();
        show_rtl();
    }
    /* The X6200 above, split: A SCAN's own average, a row each frame of it */
    int view = param_i_get(cfg_rtl_view);

    if (view != VIEW_RTL && centre && nbins && last_seq != hf_seen) {
        hf_seen = last_seq;
        to_cols(avg_db, nbins, centre - span_hz / 2.0, bin_hz, centre - VIEW_HZ / 2, HZ_PX, hf_cols);
        panel_trace(&p_hf, hf_cols);
        panel_row(&p_hf, hf_cols);
        rtl_ticks(hf_tick_l, &p_hf, centre - VIEW_HZ / 2, HZ_PX, 50000, false);
        fmt_freq(f, sizeof(f), centre);
        snprintf(s, sizeof(s), "%sX6200  %s", listen_rtl ? "" : "LISTENING  ", f);
        lv_label_set_text(rtl_src_l[0], s);
        drew = true;
    }
    while (rtl_spectrum_frame(rtl_db, N_OF(rtl_db), &n, &rate, &c, &rtl_seq, &fresh)) {
        if (fresh) rtl_drop = 2;
        if (rtl_drop > 0) {                 /* the rows of before a retune */
            rtl_drop--;
            continue;
        }
        if (rtl_rate && rate != rtl_rate) {     /* another span (listening or not): the old rows meant other columns */
            memset(rows_rtl, 0, sizeof(rows_rtl));
            p_rtl.head = p_rtl.count = p_rtl.npend = 0;
            p_rtl.floor = -300;
        }
        rtl_n = n;
        rtl_rate = rate;
        rtl_centre = c;
        double  px_hz = rtl_px_hz();
        int64_t lo    = c - rtl_view_hz() / 2;

        if (rtl_drawn && rtl_drawn != c) panel_shift(&p_rtl, (int)lround((c - rtl_drawn) / px_hz));
        rtl_drawn = c;
        to_cols(rtl_db, n, c - rate / 2.0, (double)rate / n, lo, px_hz, rtl_cols);
        if (view == VIEW_X6200) continue;   /* the stick's half not shown: kept running for a quick swap */
        panel_trace(&p_rtl, rtl_cols);
        if (listen_rtl) {                   /* the channel listened to: a bracket on the trace */
            int x = (int)lround((listen_hz - lo) / px_hz);
            uint32_t *px = (uint32_t *)rtl_frame->data, line = argb(amb);

            for (int y = p_rtl.y0 + 2; y < p_rtl.y0 + p_rtl.trace_h - 1; y++)
                for (int dx = -10; dx <= 10; dx += 20)
                    if (x + dx >= 0 && x + dx < RTL_COLS) px[y * RTL_COLS + x + dx] = line;
            for (int dx = -10; dx <= 10; dx++)
                if (x + dx >= 0 && x + dx < RTL_COLS && (dx < -6 || dx > 6))
                    px[(p_rtl.y0 + 2) * RTL_COLS + x + dx] = px[(p_rtl.y0 + p_rtl.trace_h - 2) * RTL_COLS + x + dx] = line;
        }
        panel_row(&p_rtl, rtl_cols);
        drew = true;
    }
    if (drew && rtl_rate && view != VIEW_X6200) {
        double  px_hz = rtl_px_hz();
        int64_t lo    = rtl_centre - rtl_view_hz() / 2, every = rtl_view_hz() > 1500000 ? 250000 : 100000;
        int64_t wx;
        int     found = rtl_peaks(lo, px_hz, &best_hz, &best_db, &wx);

        rtl_best = best_hz;
        rtl_spectrum_watch(wx);             /* SAME alerts from the strongest NOAA channel in view */
        rtl_ticks(rtl_tick_l, &p_rtl, lo, px_hz, every, true);
        fmt_mhz(f, sizeof(f), rtl_centre, 3);
        if (listen_rtl) {
            char lf[16];
            fmt_mhz(lf, sizeof(lf), listen_hz, 3);
            snprintf(s, sizeof(s), "LISTENING  %s %s", lf, rtl_mode_names[rtl_mode_of(listen_hz)]);
        } else {
            snprintf(s, sizeof(s), "RTL-SDR  %s MHZ", f);
        }
        lv_label_set_text(rtl_src_l[1], s);
        if (best_hz) {
            fmt_mhz(f, sizeof(f), best_hz, 3);
            snprintf(s, sizeof(s), "%s %s", f, rtl_name(best_hz));
        } else {
            snprintf(s, sizeof(s), "ONLY NOISE");
        }
        opt_set(&r_sig, 0, s, 0);
        snprintf(s, sizeof(s), "SIGNALS: %d", found);
        lv_label_set_text(r_sig.cap, s);
    }
    if (drew) lv_obj_invalidate(rtl_img);
}

/* A tap on the stick's half: that frequency to the middle */
static void rtl_touch_cb(lv_event_t *e) {
    lv_point_t p;
    lv_area_t  a;

    (void)e;
    if (!rtl_rate || !lv_indev_get_act()) return;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_obj_get_coords(rtl_img, &a);
    if (p.y - a.y1 < p_rtl.y0) {             /* the X6200's half: the X6200 heard, its VFO knob */
        rtl_listen(false);
        show_rtl();
        return;
    }
    /* the stick's: that channel heard, the VFO knob the stick's */
    listen_hz = rtl_snap(rtl_centre - rtl_view_hz() / 2 + (int64_t)((p.x - a.x1 + 0.5) * rtl_px_hz()));
    rtl_listen(true);
    show_rtl();
}

static void build_rtl(lv_obj_t *p) {
    int w = (SW - 12 - 18) / 4, iy = BODY_Y + LOW_H + 6;

    box_make(&r_dev, p, 6, BODY_Y, w, LOW_H, "RECEIVER", 2, -1);
    box_make(&r_tune, p, 6 + (w + 6), BODY_Y, w, LOW_H, "TUNED", 2, -1);
    box_make(&r_pre, p, 6 + 2 * (w + 6), BODY_Y, w, LOW_H, "PRESET", 2, -1);
    box_make(&r_sig, p, 6 + 3 * (w + 6), BODY_Y, w, LOW_H, "SIGNALS", 2, -1);
    lv_label_set_long_mode(r_pre.opt[1], LV_LABEL_LONG_SCROLL_CIRCULAR);    /* the preset's about, running by */
    rtl_img_h = scr_h - iy - 6 - 6;
    rect(p, IMG_X - 3, iy, IMG_W + 6, rtl_img_h + 6, 3, false);
    rtl_frame = lv_img_buf_alloc(RTL_COLS, rtl_img_h, LV_IMG_CF_TRUE_COLOR);
    rtl_img   = lv_img_create(p);
    lv_img_set_src(rtl_img, rtl_frame);
    lv_obj_set_pos(rtl_img, IMG_X, iy + 3);
    lv_obj_add_flag(rtl_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(rtl_img, rtl_touch_cb, LV_EVENT_CLICKED, NULL);
    for (int i = 0; i < RTL_TICKS; i++) {
        rtl_tick_l[i] = label(p, &pixel_16, dim, 0, 0, 0, 0, "");
        hf_tick_l[i]  = label(p, &pixel_16, dim, 0, 0, 0, 0, "");
        lv_obj_add_flag(rtl_tick_l[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hf_tick_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < RTL_PEAKS; i++) {
        rtl_peak_l[i] = label(p, &pixel_16, amb, 0, 0, 0, 0, "");
        lv_obj_add_flag(rtl_peak_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    rtl_why_l = label(p, &pixel_24, amb, IMG_X, iy + rtl_img_h / 2 - 40, RTL_COLS, LV_TEXT_ALIGN_CENTER, "");
    /* which half is which, at the right of each trace */
    rtl_src_l[0] = label(p, &pixel_16, amb, IMG_X + RTL_COLS - 250, iy + 6, 244, LV_TEXT_ALIGN_RIGHT, "");
    rtl_src_l[1] = label(p, &pixel_16, amb, IMG_X + RTL_COLS - 250, iy + 6, 244, LV_TEXT_ALIGN_RIGHT, "");
    for (int i = 0; i < RTL_PRESETS; i++)       /* the preset the tuning is at, if one */
        if (rtl_presets[i].hz == param_i_get(cfg_rtl_freq)) rtl_preset = i;
    rtl_layout();
}

/* The second source label goes with the stick's half */
static void rtl_labels_place(void) {
    int iy = lv_obj_get_y(rtl_img);

    lv_obj_set_y(rtl_src_l[1], iy + p_rtl.y0 + 3);
    lv_obj_set_y(rtl_src_l[0], iy + p_hf.y0 + 3);
}

static void rtl_fkey(int k) {
    switch (k) {
        case 1:
            rtl_preset = (rtl_preset + 1) % RTL_PRESETS;
            rtl_tune(rtl_presets[rtl_preset].hz);
            break;
        case 2:
            rtl_step = (rtl_step + 1) % N_OF(rtl_steps);
            break;
        case 3:
            param_i_set(cfg_rtl_view, (param_i_get(cfg_rtl_view) + 1) % 3);
            rtl_layout();
            rtl_labels_place();
            break;
        case 4:
            if (!listen_rtl && rtl_spectrum_state() != RTL_SPEC_ON) {
                osd("NO RTL-SDR", "NOTHING TO LISTEN TO: PLUG AN RTL-SDR INTO THE HOST PORT.", -1);
                break;
            }
            rtl_listen(!listen_rtl);
            break;
        case 5: {                       /* the MFK's push: the gain */
            int g = param_i_get(cfg_rtl_gain), i = 0;

            while (i < N_OF(rtl_gains) && rtl_gains[i] != g) i++;
            g = rtl_gains[(i + 1) % N_OF(rtl_gains)];
            param_i_set(cfg_rtl_gain, g);
            rtl_spectrum_gain(g);
            p_rtl.floor = -300;
            break;
        }
        default:
            break;
    }
    show_rtl();
}

static const char *f_label(int k);

static const char *const vol_names[] = {"VOLUME", "SQUELCH", "RF GAIN"};
static const char *const vol_short[] = {"VOLUME", "SQL", "RF GAIN"};

static int vol_value(void) {
    switch (vol_fn) {
        case VF_SQL: return param_i_get(cfg_squelch);
        case VF_RFG: return param_i_get(cfg_rfgain);
        default: return radio_change_vol(0);
    }
}

static void show_legend(void) {
    static const char *const mfk[SCR_N] = {"TARGET, PUSH TRACK", "TARGET, PUSH HOLD", "BAND, PUSH LISTEN",
                                           "FREQ, PUSH OPEN", "ITEM, PUSH OPEN", "RTL PAN, PUSH GAIN"};

    lv_label_set_text_fmt(legend_l, "VFO: TUNE | MFK: %s | RING: SCREEN | VOL: %s %d, PUSH %s | GEN: HELP",
                          mfk[scr], vol_short[vol_fn], vol_value(), vol_short[(vol_fn + 1) % 3]);
}

static void osd_hide_cb(lv_timer_t *t) {
    lv_timer_del(t);
    osd_timer = NULL;
    lv_obj_add_flag(osd_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_title_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_text_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_bar_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_bar, LV_OBJ_FLAG_HIDDEN);
}

/* A word over the screen for 2.5 s: what a knob or key just did; bar 0-100, or -1 none */
static void osd(const char *title, const char *text, int bar) {
    lv_label_set_text(osd_title_l, title);
    lv_label_set_text(osd_text_l, text);
    lv_obj_clear_flag(osd_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(osd_title_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(osd_text_l, LV_OBJ_FLAG_HIDDEN);
    if (bar >= 0) {
        lv_obj_clear_flag(osd_bar_frame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(osd_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(osd_bar, (bar > 100 ? 100 : bar) * (lv_obj_get_width(osd_bar_frame) - 12) / 100 + 1);
    } else {
        lv_obj_add_flag(osd_bar_frame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(osd_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (osd_timer)
        lv_timer_reset(osd_timer);
    else
        osd_timer = lv_timer_create(osd_hide_cb, 2500, NULL);
}

static void vol_say(void) {
    static const char *const how[] = {
        "HOW LOUD. PUSH THE VOL KNOB FOR SQUELCH.",
        "MUTES THE HISS UNTIL A SIGNAL COMES. 0: ALWAYS OPEN. PUSH: RF GAIN.",
        "SIGNALS AND NOISE DOWN TOGETHER. LOWER IT ONLY IF STRONG ONES DISTORT. PUSH: VOLUME."};
    char t[32];

    snprintf(t, sizeof(t), "%s %d", vol_names[vol_fn], vol_value());
    osd(t, how[vol_fn], vol_value());
    show_legend();
}

static void vol_turn(int d) {
    int v;

    switch (vol_fn) {
        case VF_SQL:
            v = param_i_get(cfg_squelch) + 2 * d;
            param_i_set(cfg_squelch, v < 0 ? 0 : v > BOARD_SQL_MAX ? BOARD_SQL_MAX : v);
            break;
        case VF_RFG:
            v = param_i_get(cfg_rfgain) + 2 * d;
            param_i_set(cfg_rfgain, v < 0 ? 0 : v > BOARD_RFG_MAX ? BOARD_RFG_MAX : v);
            break;
        default:
            radio_change_vol(d);
            break;
    }
    vol_say();
}

static void help_show(bool on) {
    help_on = on;
    if (on) {
        lv_obj_clear_flag(help_frame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(help_l, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(help_frame, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(help_l, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The F1-F5 row: "F1 NAME" over the value, the radio's own labels in capitals */
static void show_keys(void) {
    for (int i = 0; i < 5; i++) {
        char        t[40], *nl;
        const char *l = f_label(i);

        snprintf(t, sizeof(t), "F%d %s", i + 1, l);
        for (char *c = t; *c; c++)
            if (*c >= 'a' && *c <= 'z') *c -= 32;
        nl = strchr(t, '\n');
        if (nl && nl > t && nl[-1] == ':') memmove(nl - 1, nl, strlen(nl) + 1);   /* "MODE:\nAUTO": no colon */
        lv_label_set_text(key_l[i], t);
        lv_obj_set_style_pad_top(key_l[i], strchr(t, '\n') ? 2 : 12, 0);
        lv_obj_set_style_text_color(key_l[i], l[0] ? amb : dim, 0);
    }
}

static void show_screen(void) {
    for (int i = 0; i < SCR_N; i++) {
        if (i == (int)scr || (i == SCR_SCAN && scr == SCR_SCAN))
            lv_obj_clear_flag(scr_obj[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(scr_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    static const char letters[SCR_N] = {'A', 'B', 'C', 'D', 'E', 'F'};
    char              l[2]           = {letters[scr], 0};

    lv_label_set_text(badge_l[0], l);
    lv_label_set_text(badge_l[1], l);
    show_title();
    for (int i = 0; i < 5; i++) buttons_refresh(&btn[i]);
    show_keys();
    show_legend();
    switch (scr) {
        case SCR_SCAN: show_scan(); break;
        case SCR_TRACK: show_track(); break;
        case SCR_BANDS: show_bands(); break;
        case SCR_DECODE: show_decode(); break;
        case SCR_SYSTEM: show_system(); break;
        case SCR_RTL: show_rtl(); rtl_labels_place(); break;
        default: break;
    }
    show_head();
}

/* The title: the screen's, or a weather alert's event while it lasts, blinking */
static void show_title(void) {
    if (alert_until && time(NULL) < alert_until && (time(NULL) & 1))
        lv_label_set_text_fmt(title_l, "WX ALERT: %s", alert_event);
    else {
        /* after SYSTEM (E's "SYSTEM CONTROL" whole): the release (src/main.h's version, the tag it is built for),
         * the commit and the base's firmware version, "... SYSTEM: v0.3.9.4 c6e93af - base 1.0.8". One line between
         * the badges, 68 of pixel_20's 10 px characters: A and B's long title gives up "XIEGU " for it */
        const char *t = titles[scr], *ver = radio_base_ver_str(), *rel = strrchr(VERSION, ' ') + 1;
        char        s[128];

        if (strstr(t, "SYSTEM")) {
            snprintf(s, sizeof(s), "XIEGU X6200 %s: %s %s - base %s", t, rel, GIT_COMMIT, ver && ver[0] ? ver : "?");
            if (lv_txt_get_width(s, strlen(s), lv_obj_get_style_text_font(title_l, 0), 0, LV_TEXT_FLAG_NONE) >
                lv_obj_get_style_width(title_l, LV_PART_MAIN))    /* the width given, laid out or not yet */
                memmove(s, s + 6, strlen(s + 6) + 1);     /* "XIEGU " */
            lv_label_set_text(title_l, s);
        } else
            lv_label_set_text_fmt(title_l, "XIEGU X6200 %s", t);
    }
}

static void go(scr_t s) {
    scr = s;
    rtl_want(s == SCR_RTL);     /* the stick only while F shows it: AIRSPACE has it the rest of the time */
    show_screen();
}

/* ---- Tuning and tracking ---- */

static bool tune(int32_t hz, int mode) {
    if (!cfg_is_valid_hw_freq(hz)) {
        msg_update_text_fmt("The radio can't tune %d.%03d MHz", hz / 1000000, (hz / 1000) % 1000);
        return false;
    }
    if (main_screen_freq_locked()) {
        msg_update_text_fmt("The dial is locked");
        return false;
    }
    main_screen_set_freq(hz);
    if (mode >= 0 && !main_screen_mode_locked()) cparam_i_set(cfg_cur_mode, mode);
    return true;
}

static void track_start(int32_t hz, bool auto_) {
    ident_t id;

    identify(hz, &id);
    if (!tune(id.tune_hz, id.mode)) return;
    memset(&trk, 0, sizeof(trk));
    trk.on        = true;
    trk.auto_     = auto_;
    trk.hz        = hz;
    trk.id        = id;
    trk.start     = trk.last_seen = trk.last_ms = lv_tick_get();
    cursor_hz     = hz;
    logged++;
    visit(hz);
    if (!auto_)
        go(SCR_TRACK);
    else
        show_screen();
}

static void track_end(void) {
    if (trk.heard_s >= 1.0f) {
        if (nlogs == LOGS) memmove(&logs[0], &logs[1], sizeof(logs[0]) * (LOGS - 1)), nlogs--;
        logs[nlogs].t    = time(NULL);
        logs[nlogs].secs = trk.heard_s;
        logs[nlogs].peak = trk.peak;
        snprintf(logs[nlogs].label, sizeof(logs[0].label), "%s", trk.id.label);
        snprintf(logs[nlogs].name, sizeof(logs[0].name), "%s", trk.id.name);
        nlogs++;
    }
    if (rec_ours) {
        recorder_set_on(false);
        rec_ours = false;
    }
    trk.on = false;
    if (smode == MODE_AUTO && routine == ROUT_BAND && anchor) tune((int32_t)anchor, -1);   /* back to look again */
    if (smode == MODE_AUTO && routine == ROUT_SWEEP) sweep_next = 0;                       /* on to the next band */
    show_screen();      /* TRACK stays TRACK: it shows the search, then the next lock */
}

/* The tracked signal heard: its time on target, still there or lost; AUTO goes on after a while */
static void track_step(void) {
    uint32_t now = lv_tick_get();

    if (since_move >= 3) {
        trk.con = con_at(trk.hz, broad ? 40 : 2);
        if (trk.con >= HEARD_DB) {
            trk.heard_s += (now - trk.last_ms) / 1000.0f;
            trk.last_seen = now;
            if (trk.con > trk.peak) trk.peak = trk.con;
        }
    }
    trk.last_ms = now;
    if (smode == MODE_AUTO && !trk.held &&
        (now - trk.last_seen > LOST_MS || now - trk.start > DWELL_MS))
        track_end();
}

static int sweep_next_band(int from) {
    for (int k = 1; k <= BANDS_N; k++) {
        int i = (from + k + BANDS_N) % BANDS_N;
        if (band_rt[i].sweep && cfg_is_valid_hw_freq(bands[i].listen_hz)) return i;
    }
    return -1;
}

/* AUTO: lock on the strongest target not ignored nor seen lately, in the routine's place; SWEEP hops on */
static void sentry_step(void) {
    uint32_t now = lv_tick_get();

    if (trk.on) {
        track_step();
        return;
    }
    if (smode == MODE_MAN) return;
    if (routine == ROUT_SWEEP && (sweep_next == 0 || now > sweep_next)) {
        int i = sweep_next_band(sweep_i);

        sweep_next = now + SWEEP_MS;
        if (i >= 0) {
            sweep_i = i;
            tune(bands[i].listen_hz, bands[i].mode);
        }
        return;
    }
    if (smode != MODE_AUTO || since_move < SETTLE_FRAMES) return;
    for (int i = 0; i < ntg; i++) {
        if (tg[i].ignored || tg[i].con < LOCK_DB || was_visited(tg[i].hz)) continue;
        if (routine == ROUT_BAND && anchor && llabs(tg[i].hz - anchor) > VIEW_HZ / 2) continue;
        track_start(tg[i].hz, true);
        return;
    }
}

static void move_cursor(int dir) {
    int     best = -1;
    int64_t from = cursor_hz;

    if (!centre) return;
    for (int i = 0; i < ntg; i++) {
        int64_t d = (int64_t)(tg[i].hz - from) * dir;
        if (!tg[i].ignored && d > bin_hz && (best < 0 || d < (int64_t)(tg[best].hz - from) * dir)) best = i;
    }
    cursor_hz = best >= 0 ? tg[best].hz : (int32_t)(from + dir * 5000);
    if (llabs(cursor_hz - centre) > VIEW_HZ / 2) cursor_hz = (int32_t)(centre + dir * VIEW_HZ / 2);
}

static void set_mode(sysmode_t m) {
    smode = m;
    if (m == MODE_AUTO) {
        anchor     = centre ? centre : dial_centre();
        sweep_next = 0;
    }
    show_screen();
}

static void set_routine(routine_t r) {
    routine    = r;
    sweep_next = 0;
    if (r == ROUT_BAND) anchor = centre ? centre : dial_centre();
    show_screen();
}

/* ---- The base's packets ---- */

static void timer_cb(lv_timer_t *t) {
    uint16_t n;
    uint32_t span, seq;
    bool     tx;

    (void)t;
    if (!dsp_psd_raw(raw, MAX_BINS, &n, &span, &tx, &seq) || seq == last_seq) return;
    last_seq = seq;
    tx_now   = tx;
    if (tx || span < 300000 || n < 64) return;          /* on the air, or not yet the wide one */

    int64_t c = dial_centre();

    if (n != nbins || span != span_hz || !centre) {
        nbins     = n;
        span_hz   = span;
        bin_hz    = (double)span / n;
        centre    = c;
        wf_centre = c;
        frames_in = row_frames = since_move = 0;
        if (!cursor_hz) cursor_hz = (int32_t)c;
        if (!anchor) anchor = c;
        wf_clear();
        settle = 2;
        return;
    }
    if (c != centre) {
        int db = (int)lround((c - centre) / bin_hz);

        if (llabs(c - centre) < VIEW_HZ / 2 && db != 0) {
            if (db > 0) {
                memmove(avg_lin, avg_lin + db, (nbins - db) * sizeof(float));
                memmove(row_lin, row_lin + db, (nbins - db) * sizeof(float));
                for (int i = nbins - db; i < nbins; i++) avg_lin[i] = avg_lin[nbins - db - 1], row_lin[i] = 0;
            } else {
                memmove(avg_lin - db, avg_lin, (nbins + db) * sizeof(float));
                memmove(row_lin - db, row_lin, (nbins + db) * sizeof(float));
                for (int i = 0; i < -db; i++) avg_lin[i] = avg_lin[-db], row_lin[i] = 0;
            }
        } else if (llabs(c - centre) >= VIEW_HZ / 2) {
            frames_in = row_frames = 0;
        }
        centre = c;
        if (llabs(cursor_hz - c) > VIEW_HZ / 2) cursor_hz = (int32_t)c;     /* off the picture: to the dial */
        int dx = (int)lround((c - wf_centre) / HZ_PX);
        wf_shift(dx);
        wf_centre += dx * HZ_PX;
        since_move = 0;
        settle     = 2;                     /* the next packets may be of the old frequency */
        return;
    }
    if (settle > 0) {
        settle--;
        return;
    }

    for (int i = 0; i < nbins; i++) {
        float lin = powf(10.0f, raw[i] * 0.1f);

        avg_lin[i] = frames_in ? avg_lin[i] * 0.6f + lin * 0.4f : lin;
        row_lin[i] = row_frames ? row_lin[i] + lin : lin;
        avg_db[i]  = 10 * log10f(avg_lin[i] + 1e-20f);
    }
    frames_in++;
    row_frames++;
    since_move++;
    find_floor();
    find_targets();
    if (row_frames >= 2) {
        wf_row();
        row_frames = 0;
    }
    if (since_move >= SETTLE_FRAMES) {
        int bi = band_of(centre);
        if (bi >= 0) {
            float best = 0;
            for (int i = 0; i < ntg; i++)
                if (!tg[i].ignored && tg[i].con > best) best = tg[i].con;
            band_rt[bi].seen = nlive;
            band_rt[bi].best = best;
            band_rt[bi].ms   = lv_tick_get() | 1;
        }
    }
    sentry_step();
    if (scr == SCR_SCAN) {
        draw_spectrum();
        lv_obj_invalidate(img);
        show_labels();
    } else if (scr == SCR_TRACK) {
        show_track();
    }
}

static void blink_cb(lv_timer_t *t) {
    (void)t;
    blink = !blink;
    show_head();
    if (scr == SCR_SCAN) show_scan();
    if (scr == SCR_TRACK) show_track();
    if (scr == SCR_BANDS && (lv_tick_get() / 500) % 20 == 0) show_bands();
}

/* ---- The keys ---- */

static const char *f_label(int k) {
    static char s[24];

    switch (scr) {
        case SCR_SCAN: {
            static const char *const m[] = {"Mode:\nAuto", "Mode:\nSemi", "Mode:\nManual"};
            const char *const        l[] = {m[smode], routine == ROUT_BAND ? "Routine:\nBand" : "Routine:\nSweep",
                                            "Bands", "Decode", "System"};
            return l[k];
        }
        case SCR_TRACK: {
            const char *const l[] = {"Scan", trk.held ? "Held" : "Hold", recorder_is_on() ? "Rec:\nOn" : "Rec",
                                     "Next", "Ignore"};
            return l[k];
        }
        case SCR_BANDS:
            if (k == 2) {
                snprintf(s, sizeof(s), "Sweep:\n%s", band_rt[band_sel].sweep ? "In" : "Out");
                return s;
            } else {
                const char *const l[] = {"Scan", "Listen", "", "Decode", "System"};
                return l[k];
            }
        case SCR_DECODE:
            if (k == 3) {
                snprintf(s, sizeof(s), "Type:\n%s", decoders[dec_sel].name);
                return s;
            } else {
                const char *const l[] = {"Scan", "Open", "Bands", "", "System"};
                return l[k];
            }
        case SCR_SYSTEM: {
            const char *const l[] = {"Scan", "Open", "Bands", "Decode", "Classic"};
            return l[k];
        }
        case SCR_RTL: {
            int g = param_i_get(cfg_rtl_gain);

            static const char *const views[] = {"RTL", "Split", "X6200"};

            (void)g;
            if (k == 1) snprintf(s, sizeof(s), "Preset:\n%s", rtl_presets[rtl_preset].name);
            else if (k == 2) snprintf(s, sizeof(s), "Step:\n%g kHz", rtl_steps[rtl_step] / 1000.0);
            else if (k == 3) snprintf(s, sizeof(s), "View:\n%s", views[param_i_get(cfg_rtl_view) % 3]);
            else if (k == 4) snprintf(s, sizeof(s), "Listen:\n%s", listen_rtl ? "RTL" : "X6200");
            else snprintf(s, sizeof(s), "Scan");
            return s;
        }
        default:
            return "";
    }
}

static const char *f1_label(void) { return f_label(0); }
static const char *f2_label(void) { return f_label(1); }
static const char *f3_label(void) { return f_label(2); }
static const char *f4_label(void) { return f_label(3); }
static const char *f5_label(void) { return f_label(4); }

/* The decoder chosen opened in its app (or the main screen for CW), on the frequency chosen */
static int32_t open_hz;
static int     open_mode;

static void open_tune_cb(lv_timer_t *t) {
    lv_timer_del(t);
    if (main_screen_freq_locked()) {
        msg_update_text_fmt("Choose %d.%03d MHz in the app", open_hz / 1000000, (open_hz / 1000) % 1000);
        return;
    }
    main_screen_set_freq(open_hz);
    if (open_mode >= 0 && !main_screen_mode_locked()) cparam_i_set(cfg_cur_mode, open_mode);
}

static void open_async(void *arg) {
    const decoder_t *d = &decoders[(intptr_t)arg];

    if (d->proto >= 0) param_i_set(cfg_ft8_protocol, d->proto);    /* the FT8 app: 0 FT4, 1 FT8 */
    if (!d->app) {                          /* CW: the main screen, there, till the Sentry opens again */
        main_screen_home_pause(true);
        dialog_destruct();
        lv_timer_create(open_tune_cb, 50, NULL);
        return;
    }
    /* A digital app opens where its memory says and then holds the frequency (the FT8 app on the preset nearest
     * it): the frequency chosen goes there first */
    if (tune(open_hz, open_mode)) app_mem_save(d->mem);
    main_screen_start_app(d->app);          /* closes the Sentry first */
}

static void open_decoder(void) {
    const decoder_t *d = &decoders[dec_sel];

    if (dec_row >= d->n) return;
    open_hz   = d->hz[dec_row] - d->audio;     /* the dial: the station that far up in the audio */
    open_mode = d->mode;
    if (d->mode < 0) open_mode = open_hz < 10000000 ? radio_mode_lsb : radio_mode_usb;
    lv_async_call(open_async, (void *)(intptr_t)dec_sel);
}

static void sys_open_async(void *arg) {
    switch ((int)(intptr_t)arg) {
        case SYS_SETTINGS: main_screen_start_app(ACTION_APP_SETTINGS); break;
        case SYS_APPS: main_screen_open_apps(); break;
        case SYS_MODES: main_screen_start_app(ACTION_APP_MODES); break;
        case SYS_AIRSPACE: main_screen_start_app(ACTION_APP_ADSB); break;
        case SYS_SHELL: main_screen_open_dialog(dialog_shell); break;
#ifdef BOARD_X6200
        case SYS_BASEFW: main_screen_open_dialog(dialog_base_update); break;
#else
        case SYS_BASEFW: msg_update_text_fmt("Base firmware updates are not supported on the X6100"); break;
#endif
        case SYS_CLASSIC:
            main_screen_home_pause(true);   /* not straight back here */
            dialog_destruct();
            break;
        default: break;
    }
}

static void sys_open(int i) {
    if (i == SYS_VHF) {
        go(SCR_RTL);
        return;
    }
    if (i == SYS_HOME) {
        param_i_set(cfg_sentry_home, !param_i_get(cfg_sentry_home));
        show_screen();
        return;
    }
    lv_async_call(sys_open_async, (void *)(intptr_t)i);
}

static void listen_band(void) {
    const band_t *b = &bands[band_sel];

    if (!cfg_is_valid_hw_freq(b->listen_hz)) {
        msg_update_text_fmt("The radio can't tune %s", b->name);
        return;
    }
    /* the new band the place to come back to before a lock ends: track_end goes back there */
    routine = ROUT_BAND;
    anchor  = b->listen_hz;
    smode   = MODE_AUTO;
    if (trk.on) track_end();
    tune(b->listen_hz, b->mode);
    go(SCR_SCAN);
}

static void fkey(int k);

static void key_tap_cb(lv_event_t *e) {
    fkey((int)(intptr_t)lv_event_get_user_data(e));
}

static void fkey(int k) {
    switch (scr) {
        case SCR_SCAN:
            if (k == 0) set_mode((sysmode_t)((smode + 1) % 3));
            if (k == 1) set_routine(routine == ROUT_BAND ? ROUT_SWEEP : ROUT_BAND);
            if (k == 2) go(SCR_BANDS);
            if (k == 3) go(SCR_DECODE);
            if (k == 4) go(SCR_SYSTEM);
            break;
        case SCR_TRACK:
            if (k == 0) {
                if (trk.on && smode != MODE_AUTO) track_end();     /* by hand: done with it; AUTO goes on */
                go(SCR_SCAN);
            }
            if (k == 1 && trk.on) trk.held = !trk.held;
            if (k == 2 && trk.on) {
                recorder_set_on(!recorder_is_on());
                rec_ours = recorder_is_on();
            }
            if (k == 3 && trk.on) {
                move_cursor(1);
                int32_t next = cursor_hz;
                bool    semi = smode != MODE_AUTO;

                track_end();
                if (semi && target_near(next, 2000)) track_start(next, false);
            }
            if (k == 4 && trk.on) {
                if (n_ignored < IGNORED) ignored[n_ignored++] = trk.hz;
                msg_update_text_fmt("Ignored until the Sentry closes");
                track_end();
            }
            if (scr == SCR_TRACK) show_screen();
            break;
        case SCR_BANDS:
            if (k == 0) go(SCR_SCAN);
            if (k == 1) listen_band();
            if (k == 2) {
                band_rt[band_sel].sweep = !band_rt[band_sel].sweep;
                show_screen();
            }
            if (k == 3) go(SCR_DECODE);
            if (k == 4) go(SCR_SYSTEM);
            break;
        case SCR_DECODE:
            if (k == 0) go(SCR_SCAN);
            if (k == 1) open_decoder();
            if (k == 2) go(SCR_BANDS);
            if (k == 3) {
                dec_sel = (dec_sel + 1) % DECODERS_N;
                dec_row = 0;
                show_screen();
            }
            if (k == 4) go(SCR_SYSTEM);
            break;
        case SCR_SYSTEM:
            if (k == 0) go(SCR_SCAN);
            if (k == 1) sys_open(sys_sel);
            if (k == 2) go(SCR_BANDS);
            if (k == 3) go(SCR_DECODE);
            if (k == 4) sys_open(SYS_CLASSIC);
            break;
        case SCR_RTL:
            if (k == 0) go(SCR_SCAN);
            else rtl_fkey(k);
            if (scr == SCR_RTL) show_screen();
            break;
        default:
            break;
    }
}

static void f1_cb(button_data_t *b) { (void)b; fkey(0); }
static void f2_cb(button_data_t *b) { (void)b; fkey(1); }
static void f3_cb(button_data_t *b) { (void)b; fkey(2); }
static void f4_cb(button_data_t *b) { (void)b; fkey(3); }
static void f5_cb(button_data_t *b) { (void)b; fkey(4); }

static void step(int dir) {
    switch (scr) {
        case SCR_SCAN:
            if (smode == MODE_AUTO) smode = MODE_SEMI;  /* the MFK takes over the choosing */
            move_cursor(dir);
            show_screen();
            break;
        case SCR_TRACK:
            if (trk.on) {
                move_cursor(dir);
                if (target_near(cursor_hz, 2000) && labs(cursor_hz - trk.hz) > 2000) {
                    int32_t next = cursor_hz;
                    trk.auto_    = false;
                    trk.on       = false;
                    track_start(next, false);
                }
            }
            break;
        case SCR_BANDS:
            band_sel = (band_sel + dir + BANDS_N) % BANDS_N;
            show_screen();
            break;
        case SCR_DECODE: {
            int n   = decoders[dec_sel].n;
            dec_row = (dec_row + dir + n) % n;
            show_screen();
            break;
        }
        case SCR_SYSTEM:
            sys_sel = (sys_sel + dir + SYS_N) % SYS_N;
            show_screen();
            break;
        case SCR_RTL:
            rtl_tune((int64_t)param_i_get(cfg_rtl_freq) / rtl_steps[rtl_step] * rtl_steps[rtl_step] +
                     (int64_t)dir * rtl_steps[rtl_step]);
            break;
        default:
            break;
    }
}

static void press(void) {
    switch (scr) {
        case SCR_SCAN: {
            const target_t *t = target_near(cursor_hz, 2000);

            if (trk.on && labs(cursor_hz - trk.hz) <= 2500)
                go(SCR_TRACK);
            else if (centre)
                track_start(t ? t->hz : cursor_hz, false);
            break;
        }
        case SCR_TRACK:
            if (trk.on) trk.held = !trk.held;
            show_screen();
            break;
        case SCR_BANDS: listen_band(); break;
        case SCR_DECODE: open_decoder(); break;
        case SCR_SYSTEM: sys_open(sys_sel); break;
        case SCR_RTL: rtl_fkey(5); show_screen(); break;
        default: break;
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (help_on && key != KEY_VOL_LEFT_EDIT && key != KEY_VOL_RIGHT_EDIT && key != KEY_VOL_LEFT_SELECT &&
        key != KEY_VOL_RIGHT_SELECT) {
        help_show(false);
        return;
    }
    switch (key) {
        case KEY_VOL_PRESS:
            vol_fn = (volfn_t)((vol_fn + 1) % 3);
            vol_say();
            return;
        case KEY_MFK_PREV:
        case KEY_MFK_NEXT:
            go((scr_t)((scr + (key == KEY_MFK_NEXT ? 1 : SCR_N - 1)) % SCR_N));
            return;
        default:
            break;
    }
    switch (key) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            step(-1);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
            step(1);
            break;
        case LV_KEY_ENTER:
            press();
            break;
        case LV_KEY_ESC:
            dialog_destruct();
            break;
        case KEY_VOL_LEFT_EDIT:
        case KEY_VOL_LEFT_SELECT:
            vol_turn(-1);
            break;
        case KEY_VOL_RIGHT_EDIT:
        case KEY_VOL_RIGHT_SELECT:
            vol_turn(1);
            break;
        default:
            break;
    }
}

/* MFK press: the encoder sends a click, not ENTER */
static void click_cb(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        if (help_on)
            help_show(false);
        else
            press();
    }
}

/* The main knob: the radio is the operator's again */
static void rotary_cb(int32_t diff) {
    if (knob_taken()) {                 /* the stick is heard: the knob is its */
        rtl_listen_turn(diff);
        return;
    }
    if (smode != MODE_MAN) {
        smode = MODE_MAN;
        if (trk.on) trk.held = true;
        show_screen();
        osd("MAN-OVERRIDE", "THE VFO KNOB TUNES BY HAND NOW. F1: BACK TO AUTO OR SEMI.", -1);
    }
}

/* The filter: wide, normal, narrow, where the mode has a voice filter */
static void set_filter(int lo, int hi) {
    if (hi > cparam_i_get(cfg_cur_filter_high)) {
        cparam_i_set(cfg_cur_filter_high, hi);
        cparam_i_set(cfg_cur_filter_low, lo);
    } else {
        cparam_i_set(cfg_cur_filter_low, lo);
        cparam_i_set(cfg_cur_filter_high, hi);
    }
}

static void filter_step(void) {
    static const int  am[] = {5000, 3500, 2500}, ssb[] = {3000, 2400, 1800};
    static const char *const words[] = {"FILTER WIDE", "FILTER NORMAL", "FILTER NARROW"};
    int               m  = cparam_i_get(cfg_cur_mode), hi = cparam_i_get(cfg_cur_filter_high), i = 0;
    const int        *w  = m == radio_mode_am || m == radio_mode_sam ? am
                         : m == radio_mode_lsb || m == radio_mode_usb || m == radio_mode_lsb_dig || m == radio_mode_usb_dig ? ssb
                                                                                                                        : NULL;
    if (!w) {
        osd("FILTER", "THIS MODE KEEPS ITS OWN FILTER.", -1);
        return;
    }
    for (int k = 1; k < 3; k++)
        if (abs(w[k] - hi) < abs(w[i] - hi)) i = k;
    i = (i + 1) % 3;
    set_filter(w == am ? 0 : cparam_i_get(cfg_cur_filter_low), w[i]);
    osd(words[i], i == 2 ? "LESS NOISE AND NEIGHBOURS, VOICES MUFFLED. DFL AGAIN: WIDE."
                  : i == 1 ? "THE USUAL. DFL AGAIN: NARROW."
                           : "ALL THE AUDIO, AND MORE NOISE. DFL AGAIN: NORMAL.", -1);
}

static void band_key(bool up) {
    int32_t khz = cparam_i_get(cfg_fg_freq) / 1000;
    int     i   = band_of((int64_t)khz * 1000), n = 0;

    if (i < 0) {        /* between bands: the next one past the dial */
        i = up ? -1 : BANDS_N;
        for (int k = 0; k < BANDS_N; k++)
            if (up ? bands[k].lo > khz : bands[k].hi < khz) {
                i = up ? k - 1 : k + 1;
                if (up) break;
            }
    }
    do {
        i = (i + (up ? 1 : -1) + BANDS_N) % BANDS_N;
    } while (!cfg_is_valid_hw_freq(bands[i].listen_hz) && ++n < BANDS_N);
    band_sel = i;
    listen_band();
    osd(bands[i].name, bands[i].hear, -1);
}

static bool front_key(int key, int state) {
    bool tap = state == KEYPAD_RELEASE, hold = state == KEYPAD_LONG;
    char t[48];

    if (help_on) {
        if (tap || hold) help_show(false);
        return true;
    }
    switch (key) {
        case KEYPAD_GEN:
            if (tap) help_show(true);
            return true;
        case KEYPAD_APP:
            if (tap) go(SCR_SYSTEM);
            return true;
        case KEYPAD_BAND_UP:
        case KEYPAD_BAND_DOWN:
            if (tap) band_key(key == KEYPAD_BAND_UP);
            return true;
        case KEYPAD_DFN:
            if (tap) {
                param_i_set(cfg_nr, !param_i_get(cfg_nr));
                osd(param_i_get(cfg_nr) ? "NOISE REDUCTION ON" : "NOISE REDUCTION OFF",
                    "SMOOTHS THE HISS UNDER VOICES; OFF FOR DATA. HOLD DFN: NOISE BLANKER.", -1);
            } else if (hold) {
                param_i_set(cfg_nb, !param_i_get(cfg_nb));
                osd(param_i_get(cfg_nb) ? "NOISE BLANKER ON" : "NOISE BLANKER OFF",
                    "CUTS CLICKS AND POPS (ENGINES, FENCES). NO HELP WITH STEADY HISS.", -1);
            }
            return true;
        case KEYPAD_DFL:
            if (tap) filter_step();
            return true;
        case KEYPAD_PRE:
            if (tap) {
                cparam_i_set(cfg_cur_pre, !cparam_i_get(cfg_cur_pre));
                osd(cparam_i_get(cfg_cur_pre) ? "PREAMP ON" : "PREAMP OFF", "ABOUT 15 DB MORE FOR WEAK SIGNALS.", -1);
            } else if (hold) {
                cparam_i_set(cfg_cur_att, !cparam_i_get(cfg_cur_att));
                osd(cparam_i_get(cfg_cur_att) ? "ATTENUATOR ON" : "ATTENUATOR OFF", "LESS OF EVERYTHING, FOR VERY STRONG SIGNALS.", -1);
            }
            show_head();
            return true;
        case KEYPAD_AGC:
            if (tap) {
                static const char *const agc[] = {"AGC OFF", "AGC SLOW", "AGC FAST", "AGC AUTO"};
                int                      a;

                main_screen_agc_next();
                a = cparam_i_get(cfg_cur_agc);
                osd(a >= 0 && a <= 3 ? agc[a] : "AGC", "KEEPS LOUD AND QUIET STATIONS EVEN. AUTO SUITS MOST.", -1);
            } else if (hold) {
                osd("SPLIT", "NOT USED IN THE SENTRY.", -1);
            }
            return true;
        case KEYPAD_ATU:
            if (hold) {
                osd("ANTENNA TUNER", "TUNING TRANSMITS A CARRIER: NOT IN THE SENTRY.", -1);
                return true;
            }
            if (tap) osd(param_i_get(cfg_atu_enabled) ? "TUNER OUT" : "TUNER IN",
                         "THE RADIO'S ANTENNA TUNER, WITH THE TUNING IT KEPT FOR THIS BAND.", -1);
            return false;       /* the main screen switches it */
        case KEYPAD_AB:
            if (tap || hold) osd("A/B", "THE SECOND VFO IS NOT USED IN THE SENTRY.", -1);
            return true;
        case KEYPAD_KEY:
        case KEYPAD_MSG:
            if (tap || hold) {
                snprintf(t, sizeof(t), "%s", key == KEYPAD_KEY ? "KEY: CW KEYER" : "MSG: MESSAGES");
                osd(t, "THEY TRANSMIT: LOCKED OUT IN THE SENTRY. E SYSTEM, CLASSIC RADIO LEAVES IT.", -1);
            }
            return true;
        case KEYPAD_MODE_AM:
            if (tap) osd("AM", "BROADCAST STATIONS, CB, TIME SIGNALS: THE CARRIER AND BOTH SIDES.", -1);
            return false;
        case KEYPAD_MODE_CW:
            if (tap) osd("CW", "MORSE CODE: A NARROW TONE FOR EACH SIGNAL.", -1);
            return false;
        case KEYPAD_MODE_SSB:
            if (tap) osd("SSB", "HAM VOICE: LSB BELOW 10 MHZ, USB ABOVE. TUNE SLOWLY TILL VOICES SOUND RIGHT.", -1);
            return false;
        case KEYPAD_FST:
            if (tap) osd("TUNING STEP", "HOW FAR ONE CLICK OF THE VFO KNOB MOVES.", -1);
            return false;
        default:
            return false;       /* PTT (refused), POWER, F1-F5: as ever */
    }
}

/* Touch: the picture picks the nearest target (twice: track it); options, rows */
static void img_touch_cb(lv_event_t *e) {
    lv_point_t p;
    lv_area_t  a;

    if (!centre || !lv_indev_get_act()) return;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_obj_get_coords(img, &a);
    double          x    = p.x - a.x1;
    const target_t *best = NULL;

    for (int i = 0; i < ntg; i++) {
        double d = fabs(px_of(tg[i].hz) - x);
        if (d <= 14 && (!best || d < fabs(px_of(best->hz) - x))) best = &tg[i];
    }
    int32_t hz = best ? best->hz : (int32_t)hz_of_px(x);

    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (best && last_tap_hz == hz && lv_tick_get() - last_tap_ms < 1500) {
            track_start(hz, false);
            return;
        }
        last_tap_hz = hz;
        last_tap_ms = lv_tick_get();
    }
    if (smode == MODE_AUTO) smode = MODE_SEMI;
    cursor_hz = hz;
    show_scan();
}

static void tap_cb(lv_event_t *e) {
    int tag = (int)(intptr_t)lv_event_get_user_data(e), box = tag / 16, i = tag % 16;

    switch (box) {
        case 0: set_mode((sysmode_t)i); break;                     /* SYSTEM MODE */
        case 1: if (i < 2) set_routine((routine_t)i); break;       /* SCAN ROUTINE */
        case 4: if (i == 1) go(SCR_TRACK); break;                  /* SIGNAL STATUS: TRACK */
        case 5:                                                    /* a SYSTEM item */
            if (i < SYS_N) {
                if (i == sys_sel) {
                    sys_open(i);
                } else {
                    sys_sel = i;
                    show_screen();
                }
            }
            break;
        case 3:                                                    /* a decoder */
            if (i < DECODERS_N) {
                dec_sel = i;
                dec_row = 0;
                show_screen();
            }
            break;
        default: break;
    }
}

static void row_tap_cb(lv_event_t *e) {
    int tag = (int)(intptr_t)lv_event_get_user_data(e);

    if (scr == SCR_BANDS) {
        int top = band_sel - bd_rows / 2;
        if (top > BANDS_N - bd_rows) top = BANDS_N - bd_rows;
        if (top < 0) top = 0;
        int i = top + tag;
        if (i >= BANDS_N) return;
        if (i == band_sel)
            listen_band();
        else {
            band_sel = i;
            show_screen();
        }
    } else if (scr == SCR_DECODE) {
        if (tag >= decoders[dec_sel].n) return;
        if (tag == dec_row)
            open_decoder();
        else {
            dec_row = tag;
            show_screen();
        }
    }
}

/* ---- Building it ---- */

static void build_head(void) {
    rect(root, 6, 3, SW - 12, HEAD_H, 3, false);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = rect(root, i ? SW - 50 : 12, 7, 36, 36, 3, false);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        badge_l[i] = label(root, &pixel_28, amb, i ? SW - 50 : 12, 10, 36, LV_TEXT_ALIGN_CENTER, "A");
    }
    title_l  = label(root, &pixel_20, amb, 60, 6, SW - 120, LV_TEXT_ALIGN_CENTER, "");
    status_l = label(root, &pixel_16, amb, 60, 27, SW - 120, LV_TEXT_ALIGN_CENTER, "");
}

static void build_scan(lv_obj_t *p) {
    int w = (SW - 12 - 18) / 4;

    box_make(&b_mode, p, 6, BODY_Y, w, BOX_H, "SYSTEM MODE", 3, 0);
    box_make(&b_tx, p, 6 + (w + 6), BODY_Y, w, BOX_H, "TRANSMITTER", 3, -1);
    box_make(&b_sig, p, 6 + 2 * (w + 6), BODY_Y, w, BOX_H, "SIGNAL STATUS", 3, 4);
    box_make(&b_rout, p, 6 + 3 * (w + 6), BODY_Y, w, BOX_H, "SCAN ROUTINE", 3, 1);

    int iy = BODY_Y + BOX_H + 6;

    img_h = scr_h - LOW_H - 6 - iy - 6;
    wf_y0 = SPEC_H + SCALE_H;
    rect(p, IMG_X - 3, iy, IMG_W + 6, img_h + 6, 3, false);
    frame = lv_img_buf_alloc(IMG_W, img_h, LV_IMG_CF_TRUE_COLOR);
    img   = lv_img_create(p);
    lv_img_set_src(img, frame);
    lv_obj_set_pos(img, IMG_X, iy + 3);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img, img_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(img, img_touch_cb, LV_EVENT_CLICKED, NULL);
    {
        uint32_t *px = (uint32_t *)frame->data;
        for (int i = 0; i < wf_y0 * IMG_W; i++) px[i] = argb(ink);
    }
    for (int i = 0; i < TICKS; i++) {
        tick_l[i] = label(p, &pixel_16, dim, 0, 0, 0, 0, "");
        lv_obj_add_flag(tick_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < LABELS; i++) {
        tgt_l[i] = label(p, &pixel_16, amb, 0, 0, 0, 0, "");
        lv_obj_add_flag(tgt_l[i], LV_OBJ_FLAG_HIDDEN);
    }

    int ly = scr_h - LOW_H - 4, w1 = 300, w2 = 200, w3 = SW - 12 - w1 - w2 - 12;

    box_make(&b_band, p, 6, ly, w1, LOW_H, "TARGET BAND", 2, -1);
    box_make(&b_prof, p, 6 + w1 + 6, ly, w2, LOW_H, "SIGNAL PROFILE", 2, -1);
    box_make(&b_tgts, p, 6 + w1 + w2 + 12, ly, w3, LOW_H, "TARGETS", 2, -1);
}

static void build_track(lv_obj_t *p) {
    int y0 = BODY_Y + 4, h = scr_h - y0 - 6;

    t_state_l    = label(p, &pixel_28, amb, 6, y0 + 26, 230, LV_TEXT_ALIGN_CENTER, "");
    t_time_cap_l = label(p, &pixel_28, amb, 6, y0 + 128, 230, LV_TEXT_ALIGN_CENTER, "TIME ON\nTARGET (SECS)");
    t_crit_frame = rect(p, 12, y0 + 210, 218, 52, 3, false);
    t_crit_l     = label(p, &pixel_24, amb, 18, y0 + 216, 206, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_height(t_crit_l, 40);
    lv_obj_set_style_pad_top(t_crit_l, 8, 0);
    lv_obj_set_style_bg_color(t_crit_l, amb, 0);

    int mx = 244, mw = 390;
    rect(p, mx, y0, mw, 112, 3, false);
    t_big_l = label(p, &pixel_44, amb, mx + 3, y0 + 10, mw - 6, LV_TEXT_ALIGN_CENTER, "");
    t_sub_l = label(p, &pixel_24, amb, mx + 3, y0 + 70, mw - 6, LV_TEXT_ALIGN_CENTER, "");
    rect(p, mx, y0 + 120, mw, 74, 3, false);
    t_time_l = label(p, &pixel_44, amb, mx + 3, y0 + 134, mw - 6, LV_TEXT_ALIGN_CENTER, "");
    t_id_l   = label(p, &pixel_20, amb, mx, y0 + 200, mw, LV_TEXT_ALIGN_CENTER, "");
    lv_label_set_long_mode(t_id_l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(t_id_l, 24);
    t_db_l   = label(p, &pixel_20, dim, mx, y0 + 232, mw, LV_TEXT_ALIGN_CENTER, "");
    t_log_l  = label(p, &pixel_16, amb, mx, y0 + 260, mw, LV_TEXT_ALIGN_LEFT, "");

    int bx = 650;
    label(p, &pixel_20, amb, bx, y0, 64, LV_TEXT_ALIGN_CENTER, "SIG");
    label(p, &pixel_20, amb, bx + 74, y0, 70, LV_TEXT_ALIGN_CENTER, "NOISE");
    t_bar_y = y0 + 26;
    t_bar_h = h - 30;
    for (int i = 0; i < 2; i++) {
        int x = bx + 8 + i * 74;
        rect(p, x, t_bar_y, 50, t_bar_h, 3, false);
        for (int k = 1; k < 10; k++) rect(p, x + 50, t_bar_y + k * t_bar_h / 10, 10, 2, 0, true);
        lv_obj_t *f = rect(p, x + 6, t_bar_y + t_bar_h, 38, 0, 0, true);
        if (i == 0)
            t_sig_fill = f;
        else
            t_noise_fill = f;
    }
}

static void build_bands(lv_obj_t *p) {
    int y = BODY_Y + 2, rh = 18;

    rect(p, 6, y - 2, SW - 12, scr_h - y - 2, 3, false);
    char h[128];

    snprintf(h, sizeof(h), BAND_ROW, ' ', "BAND", "MHZ", "WHAT YOU'LL HEAR", "BEST", "ANTENNA", "NOW");
    bd_head_l = label(p, &pixel_16, amb, 9, y + 2, 0, 0, h);
    rect(p, 6, y + 20, SW - 12, 3, 0, true);
    bd_rows = (scr_h - (y + 26) - 50) / rh;
    if (bd_rows > 20) bd_rows = 20;
    for (int r = 0; r < bd_rows; r++) {
        bd_row_l[r] = label(p, &pixel_16, amb, 9, y + 26 + r * rh, SW - 18, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_set_height(bd_row_l[r], rh);
        lv_label_set_long_mode(bd_row_l[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(bd_row_l[r], amb, 0);
        lv_obj_add_flag(bd_row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bd_row_l[r], row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    }
    rect(p, 6, scr_h - 50, SW - 12, 3, 0, true);
    bd_about_l = label(p, &pixel_16, amb, 14, scr_h - 44, SW - 28, LV_TEXT_ALIGN_LEFT, "");
}

static void build_decode(lv_obj_t *p) {
    int y = BODY_Y, h = scr_h - y - DLOW_H - 10, lw = 196;

    box_make(&d_sel, p, 6, y, lw, h, "TARGET SELECT", DECODERS_N < 8 ? DECODERS_N : 8, 3);
    rect(p, lw + 12, y, SW - lw - 18, h, 3, false);
    char hd[96];

    snprintf(hd, sizeof(hd), DEC_ROW, "KNOWN FREQ", "BAND", "BEST", "ANTENNA", "NOW");
    d_head_l = label(p, &pixel_16, amb, lw + 15, y + 5, 0, 0, hd);
    rect(p, lw + 12, y + 25, SW - lw - 18, 3, 0, true);
    for (int r = 0; r < 12; r++) {
        d_row_l[r] = label(p, &pixel_16, amb, lw + 15, y + 30 + r * 18, SW - lw - 24, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_set_height(d_row_l[r], 18);
        lv_label_set_long_mode(d_row_l[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(d_row_l[r], amb, 0);
        lv_obj_add_flag(d_row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(d_row_l[r], row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    }

    int ly = scr_h - DLOW_H - 4, w1 = 380, w2 = 190, w3 = SW - 12 - w1 - w2 - 12;

    box_make(&d_how, p, 6, ly, w1, DLOW_H, "WHAT IT IS", 1, -1);
    lv_obj_set_height(d_how.opt[0], 48);
    lv_obj_set_style_text_font(d_how.opt[0], &pixel_16, 0);
    lv_label_set_long_mode(d_how.opt[0], LV_LABEL_LONG_WRAP);
    box_make(&d_ant, p, 6 + w1 + 6, ly, w2, DLOW_H, "OPENS IN", 2, -1);
    box_make(&d_now, p, 6 + w1 + w2 + 12, ly, w3, DLOW_H, "ANTENNA", 2, -1);
    for (int i = 0; i < 2; i++) lv_obj_set_style_text_font(d_now.opt[i], &pixel_16, 0);
}

/* E SYSTEM: the items on the left, what the chosen one does on the right, the look, the home screen and the keys
 * under them */
static void build_system(lv_obj_t *p) {
    int y = BODY_Y, h = scr_h - y - DLOW_H - 10, lw = 300;

    box_make(&s_list, p, 6, y, lw, h, "SYSTEM", SYS_N, 5);
    box_make(&s_what, p, lw + 12, y, SW - lw - 18, h, "WHAT IT DOES", 1, -1);
    lv_obj_set_height(s_what.opt[0], h - 34);
    lv_obj_set_style_text_align(s_what.opt[0], LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_what.opt[0], LV_LABEL_LONG_WRAP);

    int ly = scr_h - DLOW_H - 4, w = (SW - 12 - 2 * 6) / 3;

    box_make(&s_look, p, 6, ly, w, DLOW_H, "THE LOOK", 2, -1);
    box_make(&s_home, p, 6 + w + 6, ly, w, DLOW_H, "HOME SCREEN", 2, -1);
    box_make(&s_keys, p, 6 + 2 * (w + 6), ly, SW - 12 - 2 * (w + 6), DLOW_H, "KEYS", 2, -1);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_font(s_look.opt[i], &pixel_16, 0);
        lv_obj_set_style_text_font(s_home.opt[i], &pixel_16, 0);
        lv_obj_set_style_text_font(s_keys.opt[i], &pixel_16, 0);
    }
}

static void build_keys(void) {
    int w = (SW - 12 - 4 * 6) / 5, y = 480 - KEYS_H - 3;

    for (int i = 0; i < 5; i++) {
        lv_obj_t *k = lv_label_create(root);

        key_l[i] = k;
        lv_obj_set_pos(k, 6 + i * (w + 6), y);
        lv_obj_set_size(k, w, KEYS_H);
        lv_obj_set_style_text_font(k, &pixel_20, 0);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_border_width(k, 2, 0);
        lv_obj_set_style_border_color(k, amb, 0);
        lv_obj_set_style_bg_color(k, amb, 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(k, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(k, ink, LV_STATE_PRESSED);
        lv_label_set_long_mode(k, LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(k, key_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void help_tap_cb(lv_event_t *e) {
    (void)e;
    help_show(!help_on);
}

static void build_overlays(void) {
    legend_l = label(root, &pixel_16, dim, 6, scr_h + 1, SW - 12, LV_TEXT_ALIGN_CENTER, "");
    lv_label_set_long_mode(legend_l, LV_LABEL_LONG_CLIP);

    /* the header: a tap shows the help */
    lv_obj_t *h = lv_obj_create(root);
    lv_obj_remove_style_all(h);
    lv_obj_set_pos(h, 60, 3);
    lv_obj_set_size(h, SW - 120, HEAD_H);
    lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h, help_tap_cb, LV_EVENT_CLICKED, NULL);

    osd_frame     = rect(root, 130, 150, SW - 260, 136, 3, false);
    osd_title_l   = label(root, &pixel_28, amb, 136, 160, SW - 272, LV_TEXT_ALIGN_CENTER, "");
    osd_text_l    = label(root, &pixel_20, amb, 146, 196, SW - 292, LV_TEXT_ALIGN_CENTER, "");
    osd_bar_frame = rect(root, 160, 250, SW - 320, 24, 2, false);
    osd_bar       = rect(root, 166, 255, 1, 14, 0, true);
    osd_hide_cb(lv_timer_create(osd_hide_cb, 100000, NULL));

    help_frame = rect(root, 14, 52, SW - 28, scr_h - 56, 3, false);
    lv_obj_add_flag(help_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(help_frame, help_tap_cb, LV_EVENT_CLICKED, NULL);
    help_l = label(root, &pixel_16, amb, 26, 60, SW - 52, LV_TEXT_ALIGN_LEFT,
                   "CONTROLS IN THE SENTRY                        ANY KEY OR A TAP CLOSES THIS\n"
                   "\n"
                   "VFO KNOB       TUNES BY HAND: THE SENTRY GOES TO MAN-OVERRIDE (F1: BACK)\n"
                   "MFK KNOB       CHOOSES A TARGET, A BAND, A FREQUENCY. PUSH: TRACK, LISTEN, OPEN\n"
                   "MFK RING       THE SCREENS: A SCAN, B TRACK, C BANDS, D DECODE, E SYSTEM, F VHF/UHF\n"
                   "F VHF/UHF      AN RTL-SDR ON USB: F5 OR A TAP ON A SIGNAL LISTENS, THE VFO THEN TUNES IT\n"
                   "VOL KNOB       VOLUME. PUSH: SQUELCH, PUSH: RF GAIN, PUSH: VOLUME AGAIN\n"
                   "F1 - F5        THE KEYS ALONG THE BOTTOM OF THE SCREEN\n"
                   "BAND UP/DOWN   THE NEXT BAND OF THE BANDS LIST, SCANNED\n"
                   "AM  CW  SSB    THE MODE: AM BROADCAST AND CB, SSB HAM VOICE, CW MORSE\n"
                   "DFN            NOISE REDUCTION ON/OFF. HOLD: NOISE BLANKER (CLICKS AND POPS)\n"
                   "DFL            FILTER WIDE, NORMAL, NARROW (NARROW: LESS NOISE, MUFFLED)\n"
                   "FST            HOW FAR ONE CLICK OF THE VFO KNOB MOVES\n"
                   "ATU            ANTENNA TUNER IN/OUT. TUNING ITSELF TRANSMITS: NOT HERE\n"
                   "A/B            NOT USED HERE\n"
                   "PTT  KEY  MSG  TRANSMIT: LOCKED OUT WHILE THE SENTRY IS OPEN\n"
                   "APP            E SYSTEM: SETTINGS, APPS, MODES, THE CLASSIC RADIO SCREEN\n"
                   "GEN            THIS LIST.  A TAP ON THE TITLE AT THE TOP SHOWS IT TOO");
    help_show(false);
}

static void construct_cb(lv_obj_t *parent) {
    main_screen_home_pause(false);      /* the home screen again: apps come back here */
    amb   = TC(accent);
    ink   = TC(bg);
    dim   = lv_color_mix(amb, ink, 110);
    faint = lv_color_mix(amb, ink, 50);
    {
        lv_color_t red = TC(err), deep = lv_color_mix(amb, red, 110), orange = lv_color_mix(amb, red, 190);
        uint32_t   s3[] = {lv_color_to32(lv_color_mix(deep, ink, 120)) & 0xffffff, lv_color_to32(deep) & 0xffffff,
                           lv_color_to32(orange) & 0xffffff, theme_hex(TC_accent), theme_hex(TC_accent_soft)};
        theme_ramp_palette(s3, 5, 0, d3_pal);
    }
    scr_h = 480 - KEYS_H - 6 - LEGEND_H;  /* over the radio's buttons: the Sentry draws its own (F1-F5 still reach them) */

    root = lv_obj_create(parent);
    dialog.obj = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, SW, 480);
    lv_obj_set_style_bg_color(root, ink, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    buttons_load_page(&page);

    for (int i = 0; i < SCR_N; i++) scr_obj[i] = screen_obj();
    build_head();
    build_scan(scr_obj[SCR_SCAN]);
    build_track(scr_obj[SCR_TRACK]);
    build_bands(scr_obj[SCR_BANDS]);
    build_decode(scr_obj[SCR_DECODE]);
    build_system(scr_obj[SCR_SYSTEM]);
    build_rtl(scr_obj[SCR_RTL]);
    build_keys();
    build_overlays();

    static bool sweep_set;
    if (!sweep_set) {
        for (int i = 0; i < BANDS_N; i++) band_rt[i].sweep = bands[i].sweep;
        sweep_set = true;
    }
    nbins = 0;
    centre = anchor = 0;
    cursor_hz = 0;
    memset(&trk, 0, sizeof(trk));
    memset(visited, 0, sizeof(visited));
    n_ignored = logged = 0;
    smode     = MODE_SEMI;
    routine   = ROUT_BAND;
    sweep_i   = -1;
    {
        int bi = band_of(dial_centre());
        band_sel = bi >= 0 ? bi : 0;
    }
    wf_clear();

    rx_held = radio_rx_only(true);
    wide_ok = radio_psd_wide(true);
    if (wide_ok) timer = lv_timer_create(timer_cb, 60, NULL);
    blink_timer = lv_timer_create(blink_cb, 500, NULL);
    rtl_timer   = lv_timer_create(rtl_timer_cb, 50, NULL);

    lv_group_add_obj(keyboard_group, root);
    lv_obj_add_event_cb(root, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(root, click_cb, LV_EVENT_CLICKED, NULL);
    lv_group_focus_obj(root);
    lv_group_set_editing(keyboard_group, true);

    go(SCR_SCAN);
    if (!wide_ok) {
        opt_set(&b_band, 0, "THIS BASE SENDS NO SPECTRUM", 1);
        opt_set(&b_band, 1, "THE SENTRY NEEDS AN X6200", 2);
    }
}

static void destruct_cb(void) {
    if (timer) lv_timer_del(timer);
    if (blink_timer) lv_timer_del(blink_timer);
    if (rtl_timer) lv_timer_del(rtl_timer);
    timer = blink_timer = rtl_timer = NULL;
    rtl_want(false);
    if (wide_ok) radio_psd_wide(false);
    if (rx_held) radio_rx_only(false);
    wide_ok = rx_held = false;
    if (rec_ours) recorder_set_on(false);
    rec_ours = false;
    trk.on   = false;
    if (osd_timer) lv_timer_del(osd_timer);
    osd_timer = NULL;
    help_on   = false;
    lv_group_remove_obj(root);
    if (frame) {                            /* the image goes with the window, undrawn */
        lv_img_cache_invalidate_src(frame);
        lv_img_buf_free(frame);
        frame = NULL;
    }
    if (rtl_frame) {
        lv_img_cache_invalidate_src(rtl_frame);
        lv_img_buf_free(rtl_frame);
        rtl_frame = NULL;
    }
    root = img = rtl_img = NULL;
}
