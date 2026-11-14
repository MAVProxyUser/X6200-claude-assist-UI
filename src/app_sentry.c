/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - Sentry: a listener's receiver on the X6200 base's whole spectrum, in the look of an old amber
 *  terminal, over the whole screen but the buttons. Receive only while it is open (radio_rx_only()).
 *
 *  Seven screens on the MFK's ring and AIRSPACE (app_adsb.c, an app of its own) as the eighth, H, between G and A;
 *  the buttons F1-F5 under them, and SCAN CFG beside A and B:
 *    A X6200   384 kHz around the dial (radio_psd_wide(), dsp_psd_raw()), its middle 360 kHz as a trace over a
 *              waterfall on C's screen, the signals that stand out of the noise around them named (AM broadcast
 *              channel, CB channel, FM station, time signal, shortwave broadcaster from the EiBi list, FT8, ham
 *              band). Its keys are C's: F1 SCAN the band end to end, F2 BAND, F3 VIEW (X6200, SPLIT, RTL), F4 LISTEN
 *              (X6200, RTL, BOTH mixed or OFF), F5 SCAN CFG. The MFK pans, its push locks on the strongest; the VFO tunes
 *              the X6200 (MAN-OVERRIDE). AUTO-SCAN locks on the strongest target not seen lately and tracks it, then
 *              goes on; SEMI-AUTO only marks. Routines: BAND, the 360 kHz at the dial; SCAN, the band end to end,
 *              360 kHz at a time; SWEEP, the bands marked in D BANDS, a few seconds each.
 *    B VHF/UHF an RTL-SDR stick's spectrum (rtl_spectrum.c): its presets or D's bands, scanned end to end (F1); the
 *              VFO tunes its channel (a bracket), the MFK pans, its push goes to the strongest.
 *    C LOG     the signals A's and B's scans locked on, newest first, kept on DATA (sentry_log.txt): the MFK goes
 *              through them and each is heard on its own receiver, the X6200 or the stick; F4 scans the list.
 *    G WALKIE  the radio as a CB walkie-talkie, held up (portrait): the channel, a voiceprint, the meters. Its G
 *              where the others have their letter (the portrait screen's top right, the display's top left).
 *    H AIRSPACE ADS-B aircraft: the AIRSPACE app, opened by the ring past G (or before A), and by itself when a GNS
 *              5890 or a microADSB becomes the only ADS-B receiver on USB (screen_main.c). Its ring comes back here.
 *    A/B       on A and G arms the transmitter for PTT (the Sentry is receive only otherwise).
 *    D BANDS   what each band carries, when, with which antenna, and what the Sentry found there; LISTEN tunes it
 *              and scans it (the stick's on C), SWEEP marks it for the sweep.
 *    E DECODE  the decoders and the frequencies they work on; OPEN takes the radio there in the radio's own app.
 *    F SYSTEM  the rest of the radio, in this look: its settings, the other apps, the digital modes menu, the
 *              classic radio screen (with transmit), and where the radio starts. The APP key comes here.
 *    SCAN CFG  (on no ring: F5 on A or C) A's: the boxes A had (SYSTEM MODE, TRANSMITTER, SIGNAL STATUS, SCAN
 *              ROUTINE, TARGET BAND, SIGNAL PROFILE, TARGETS) and the lock: HOLD (the MFK's push), NEXT (the MFK),
 *              REC (F4), IGNORE (a tap on its frequency); F1 MODE, F2 SWEEP, F3 STEP. C's: the stick and its scan,
 *              F1 GAIN, F2 DWELL, F3 STEP, F4 SQUELCH. F5 goes back.
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
#include "audio.h"
#include "clock.h"
#include "smeter.h"
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
#include "adsb_rx.h"
#include "wr12.h"
#include "recorder.h"
#include "styles.h"
#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

extern const lv_font_t pixel_16, pixel_20, pixel_24, pixel_28, pixel_32, pixel_44;

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
#define A_SCAN_MS   3000        /* SCAN: this long on each 360 kHz of the band */
#define REVISIT_MS  120000      /* AUTO: a target visited comes again after this */
#define VISITED     16
#define IGNORED     32

typedef enum { SCR_SCAN, SCR_TRACK, SCR_BANDS, SCR_DECODE, SCR_SYSTEM, SCR_RTL, SCR_LOG, SCR_WALKIE, SCR_N } scr_t;

/* The MFK ring's order and the screens' letters: A the X6200's spectrum, B the stick's spectrum (A and B one screen,
 * each set up for its own radio), C the log, D the bands, E the decoders, F the rest of the radio. SCAN CFG
 * (SCR_TRACK, A's settings and its lock, or B's) is on no ring: F5 on A or B opens it for that screen's radio. G is
 * the walkie-talkie, and H, past it, AIRSPACE: an app of its own, at RING_N on the ring */
static const scr_t ring[] = {SCR_SCAN, SCR_RTL, SCR_LOG, SCR_BANDS, SCR_DECODE, SCR_SYSTEM, SCR_WALKIE};
#define RING_N (int)(sizeof(ring) / sizeof(ring[0]))
static scr_t open_at = SCR_SCAN;           /* the screen the Sentry opens on: A, or one app_sentry_open_at() said */
static scr_t cfg_from = SCR_SCAN;          /* the screen SCAN CFG was opened from: its radio's settings */
typedef enum { MODE_AUTO, MODE_SEMI, MODE_MAN } sysmode_t;
typedef enum { ROUT_BAND, ROUT_SWEEP, ROUT_SCAN } routine_t;

static void construct_cb(lv_obj_t *parent);
static void destruct_cb(void);
static void key_cb(lv_event_t *e);
static void rotary_cb(int32_t diff);
static bool front_key(int key, int state);
static void walkie_ptt(void);
static void f1_cb(button_data_t *b);
static void f2_cb(button_data_t *b);
static void f3_cb(button_data_t *b);
static void f4_cb(button_data_t *b);
static bool knob_taken(void);
static void hear_apply(int h);
static void scan_band(void);
static void hear_vol_refresh(void);
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
    {"SHIP MF", 415, 526, "NAVTEX WARNINGS ON 518", "LONG WIRE, UNTRIED", 'N', 518000, radio_mode_usb, false,
     "Coast stations' NAVTEX on 518 kHz (English) and 490: weather and navigation warnings, farther at night."},
    {"AM BC", 530, 1700, "AM RADIO: NEWS AND TALK", "ANY WIRE; CB WHIP OK", 'N', 720000, radio_mode_am, true,
     "Your AM radio band. By day the locals; after dark big stations 1000 km away (WGN 720, Chicago)."},
    {"160 M", 1800, 2000, "HAM CW AND VOICE, WINTER", "LONG WIRE, UNTRIED", 'N', 1900000, radio_mode_lsb, false,
     "The top band: hams on winter nights. It needs a long wire; the JPC-12 does not reach it."},
    {"80 M", 3500, 4000, "HAM NETS, REGIONAL VOICE", "LONG WIRE, UNTRIED", 'N', 3850000, radio_mode_lsb, false,
     "Evening ham nets within a few hundred km: voice on LSB above 3.6 MHz, CW and FT8 below."},
    {"SHIP 4", 4063, 4438, "NAVTEX 4209.5, WX FAX", "LONG WIRE, UNTRIED", 'N', 4235000, radio_mode_usb, false,
     "The 4 MHz marine band: NAVTEX on 4209.5, NOAA weather fax (4235 Boston, 4317.9 New Orleans, 4346 Pt Reyes)."},
    {"60 M BC", 4750, 5060, "TROPICAL BROADCASTS, WWV", "JPC-12 RED-SILVER", 'N', 4950000, radio_mode_am, false,
     "Shortwave stations from the tropics at night, and WWV's time signal on 5.000 MHz."},
    {"60 M", 5330, 5410, "HAM CHANNELS, FT8 5.357", "JPC-12 RED-SILVER", 'N', 5357000, radio_mode_usb, false,
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
    {"20 M", 14000, 14350, "THE WORLD: FT8 AND VOICE", "JPC-12 RED-GOLD, TESTED", 'D', 14200000, radio_mode_usb,
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
    {"10 M", 28000, 29700, "FT8, BEACONS, VOICE", "CB WHIP WORKS, TESTED", 'D', 28400000, radio_mode_usb,
     true, "Open by day near the solar maximum: FT8 on 28.074, voice from 28.300. The CB whip heard FT8 here."},
    {"6 M", 50000, 54000, "QUIET; SUMMER OPENINGS", "SHORT WHIP", 'D', 50125000, radio_mode_usb, false,
     "The magic band: mostly quiet, with sudden summer openings."},
    {"FM BC", 88000, 108000, "FM RADIO, LOCAL", "ANY SHORT WHIP", 'L', 92300000, radio_mode_wfm, false,
     "Local FM stations, one or two 200 kHz-wide ones in the picture at a time."},
    /* Above the X6200's own receiver: the RTL-SDR's (F VHF/UHF), or a WR12's */
    {"AIR", 118000, 137000, "AIRCRAFT VOICE (AM)", "THE STICK'S OWN", 'L', 121500000, radio_mode_am, false,
     "Pilots and controllers in AM, 25 kHz apart; 121.500 is the emergency guard. Aircraft are heard far off."},
    {"2 M", 144000, 148000, "HAMS: REPEATERS, APRS", "THE STICK'S OWN", 'L', 146520000, radio_mode_nfm, false,
     "Local hams in narrow FM: repeaters 145-148, simplex 146.520, APRS packets on 144.390."},
    {"MARINE", 156000, 162025, "BOATS; CHANNEL 16 CALLS", "THE STICK'S OWN", 'L', 156800000, radio_mode_nfm, false,
     "Marine VHF in narrow FM: channel 16 (156.800) for calls and distress, AIS data on 161.975 / 162.025."},
    {"NOAA WX", 162400, 162550, "WEATHER RADIO, ALERTS", "THE STICK'S OWN", 'L', 162550000, radio_mode_nfm, false,
     "NOAA Weather Radio around the clock, with SAME alerts the Sentry shows in its title."},
    {"70 CM", 420000, 450000, "HAMS: REPEATERS, SIMPLEX", "THE STICK'S OWN", 'L', 446000000, radio_mode_nfm, false,
     "Local hams in narrow FM: repeaters 442-450, simplex 446.000."},
    {"GMRS", 462550, 467725, "FAMILY RADIOS: FRS, GMRS", "THE STICK'S OWN", 'L', 462562500, radio_mode_nfm, false,
     "Handheld family radios, FRS and GMRS, 12.5 kHz apart: a few miles at most."},
};

#define BANDS_N ((int)(sizeof(bands) / sizeof(bands[0])))

static struct {
    bool     sweep;
    int      seen;          /* targets the last time the picture was on it */
    float    best;
    uint32_t ms;            /* lv_tick of that, 0 never */
} band_rt[BANDS_N];

/* The bands the stick takes: in C, any it can tune when one is plugged in (its RX column says RTL, LISTEN opens F on
 * it), and those only it reaches (above the X6200's own receiver, without a WR12) always */
static char tag_hex[8] = "E07020";      /* the tags' colour (RTL, ADS-B, WR12, RX): the 3D picture's orange, set from
                                          * the theme at the Sentry's start (construct_cb) */
static bool sdr_here;           /* an RTL-SDR on USB (adsb_rx_sdr_present), looked at every 2 s */
static bool adsb_here;          /* an ADS-B receiver on USB, or AIRSPACE decoding (the stick's dump1090) */
static bool wr12_here;          /* the WR12 on HOST, found and set up */

static bool band_in_rtl(int i) {
    return (int64_t)bands[i].lo * 1000 >= RTL_SPEC_MIN_HZ && (int64_t)bands[i].hi * 1000 <= RTL_SPEC_MAX_HZ;
}

static bool band_uses_rtl(int i) {
    return band_in_rtl(i) && (sdr_here || !cfg_is_valid_hw_freq(bands[i].listen_hz));
}

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
#define BAND_ROW "%c%-8.8s %-11.11s %-24.24s %-5.5s %-24.24s %-13.13s %s"   /* 97 of the row's 97 at most: RX 5 wide (X6200) */
#define DEC_ROW  " %-12s %-9s %-5s %-20.20s %-13.13s %s"   /* D's columns in D's order, RX last: 70 of 72 */

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
/* The speaker: what A and F each want when shown (their F5 LISTEN), and what it has now. Only A or F shown changes it
 * (or G playing an entry); the other screens keep what was last heard */
enum { HEAR_X6200, HEAR_RTL, HEAR_BOTH, HEAR_OFF, HEAR_N };
static const char *const hear_names[] = {"X6200", "RTL", "BOTH", "OFF"};
static int     heard = HEAR_X6200;
#define listen_rtl (heard == HEAR_RTL || heard == HEAR_BOTH)     /* the stick on the speaker, alone or with the X6200 */
#define listen_x62 (heard == HEAR_X6200 || heard == HEAR_BOTH)   /* the X6200's receiver on it */
static int64_t stick_at;      /* where the stick is tuned: F's rtl_hz, or a G entry's place */
static int64_t rtl_best;      /* the strongest signal of the stick's last row, 0 none */
static float   rtl_best_db;   /* and its level (dB) */
static int64_t listen_hz;

/* The stick's tuning now (cfg_rtl_freq keeps it, saved when it is tuned by hand, not at each step of a scan) and F1's
 * scan: the preset's band swept end to end, a picture at a time (rtl_scan_step) */
static int64_t  rtl_hz;
static bool     rtl_scan;
static uint32_t scan_next, scan_pick;
static int scan_dwell_ms = 2000;      /* each picture of C's scan (SCAN CFG's DWELL): 40 rows of the waterfall at 2 s */
#define SCAN_DWELL_MS scan_dwell_ms
#define SCAN_PICK_MS  1200      /* listening: the strongest of the new picture, once its rows are in */
static void rtl_scan_set(bool on);
static void set_routine(routine_t r);
static void a_scan_hop(uint32_t now);
static void band_key(bool up);
static void hit_add(int64_t hz, int mode, bool rtl, const char *what, float peak);
static void show_glog(void);
static bool tune(int32_t hz, int mode);
static void track_start(int32_t hz, bool auto_);
static void track_end(void);
static void show_scan(void);
static const target_t *target_near(int32_t hz, int32_t within);
static const char *const rtl_mode_names[] = {"", "NFM", "AM", "WFM"};

/* The stick's demodulator for a frequency: broadcast FM wide, the airband AM, the rest narrow FM */
static int rtl_mode_of(int64_t hz) {
    if (hz >= 87500000 && hz < 108000000) return 3;                 /* FM broadcast: wide FM */
    if ((hz >= 108000000 && hz < 137000000) || hz < 29500000) return 2;     /* the airband, and CB and HF: AM */
    return 1;                                                       /* 10 m's FM segment and up: narrow FM */
}

/* The sentry */
static scr_t     scr;
static sysmode_t smode;
static routine_t routine;
static int32_t   cursor_hz;
static int64_t   anchor;
static int       sweep_i = -1;
static uint32_t  sweep_next;
static int       scan_a_band = -1;      /* SCAN: the band swept end to end, and when it moves on (0: from its low end) */
static uint32_t  scan_a_next;
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

/* G LOG: what the scans locked on, the newest (or the latest heard again) first */
#define HITS      64
#define HITS_FILE BOARD_DATA "/sentry_log.txt"
typedef struct {
    int64_t hz;
    int     mode;           /* the X6200's radio_mode_t (-1 its own); the stick's 1 NFM, 2 AM, 3 WFM */
    bool    rtl;            /* the stick's (F), else the X6200's (A) */
    char    what[28];       /* what it was taken for */
    time_t  first, last;
    int     times;
    float   peak;           /* the most seen: the X6200's dB over the noise, the stick's level in dB */
} hit_t;
static hit_t    hits[HITS];
static int      nhits, g_sel, g_on = -1;    /* g_on: the one heard now, -1 none */
static int64_t  g_hz;                       /* heard on the stick: the entry's, or where the VFO took it */
static bool     g_stick, hits_dirty, hits_loaded;
static uint32_t g_clear_ms;                 /* F3 pressed once: a second press within 3 s clears it */
static bool     g_scan;                     /* F4: through the list, each on its own receiver */
static uint32_t g_scan_next;                /* when it moves on */
static int      g_dwell = 1;                /* F5: g_dwells[] */
static const int g_dwells[] = {2, 4, 8};    /* seconds on each one that is quiet */
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
static lv_obj_t *badge_l[2], *title_l, *status_l, *batt_l;
extern const lv_font_t power_sym_14;    /* the clock's: Montserrat's battery and bolt, and a plug (clock.c) */
#define SYMBOL_PLUG "\xEF\x87\xA6"     /* U+F1E6 */
typedef struct { lv_obj_t *frame, *cap, *line, *opt[12]; int n; } box_t;     /* opt: E SYSTEM's 9 rows the most */
static box_t     b_mode, b_tx, b_sig, b_rout, b_band, b_prof, b_tgts;
static lv_obj_t *tick_l[TICKS], *tgt_l[LABELS];
static lv_obj_t *t_state_l, *t_crit_frame, *t_crit_l, *t_time_cap_l, *t_big_l, *t_sub_l, *t_time_l, *t_id_l, *t_db_l;
static lv_obj_t *t_sig_fill, *t_noise_fill;
static int       t_bar_y, t_bar_h;
static lv_obj_t *bd_head_l, *bd_row_l[20], *bd_about_l;
static int       bd_rows;
static lv_obj_t *lg_head_l, *lg_row_l[20], *lg_about_l;
static int       lg_rows;
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
                      "E DECODE OPENS THEM ON KNOWN FREQUENCIES."},
    {"AIRSPACE (ADS-B)", "AIRCRAFT ON 1090 MHZ FROM AN ADS-B RECEIVER ON USB (RTL-SDR, GNS 5890 OR MICROADSB): A SCOPE AROUND "
                         "YOUR QTH, THEIR CALLS, HEIGHTS AND TRACKS, AND THE HISTORY FADING OUT. A GNS 5890 OR "
                         "MICROADSB KEEPS LISTENING WHEN CLOSED; AN RTL-SDR IS ITS ONLY WHILE IT IS OPEN."},
    {"VHF/UHF (RTL-SDR)", "B VHF/UHF: AN RTL-SDR STICK'S SPECTRUM, 24 MHZ TO 1.7 GHZ, ALONE, SPLIT WITH THE X6200'S OR "
                          "THE X6200'S ALONE (F3 VIEW), A'S KEYS: F1 SCANS THE BAND, F2 CHOOSES IT, F4 LISTEN (X6200, "
                          "RTL OR BOTH), F5 SCAN CFG. THE VFO TUNES ITS CHANNEL, THE MFK PANS. AIRSPACE HAS THE STICK "
                          "WHILE IT IS OPEN."},
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

/* The band hz is in, else the nearest (by its edge): what is past a band's end carries, opens and wants an antenna
 * much as the band beside it */
static int band_near(int64_t hz, bool *in) {
    int32_t khz = (int32_t)(hz / 1000), best = -1, d = 0;

    *in = false;
    for (int i = 0; i < BANDS_N; i++) {
        int32_t e = khz < bands[i].lo ? bands[i].lo - khz : khz > bands[i].hi ? khz - bands[i].hi : 0;

        if (!e) {
            *in = true;
            return i;
        }
        if (best < 0 || e < d) best = i, d = e;
    }
    return best;
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

static bool a_old;          /* the first A's own 3D picture, shown no more: A is drawn on C's screen (show_screen) */

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
    if (!a_old) return;
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

static const char *titles[SCR_N] = {"MF/HF/VHF SPECTRUM", "SCAN CONFIGURATION",
                                    "BAND INTELLIGENCE", "DECODER SENTRY", "SYSTEM CONTROL", "HF/VHF/UHF SPECTRUM",
                                    "SIGNAL LOG", "WALKIE TALKIE"};

/* The battery's % as it runs down, in the 3D picture's own ramp (d3_pal, deep orange to yellow): the usual colour
 * over 30%, yellow at 30, orange at 20, the ramp's deep orange-red at 15, the theme's red under; 0 the usual */
static uint32_t batt_rgb(int cap) {
    if (cap > 30) return 0;
    if (cap < 15) return lv_color_to32(TC(err)) & 0xffffff;
    int i = cap >= 20 ? 128 + (cap - 20) * 127 / 10 : 64 + (cap - 15) * 64 / 5;

    return d3_pal[i < 0 ? 0 : i > 255 ? 255 : i] & 0xffffff;
}

static void show_head(void) {
    char       f[24], t[16], tc[32] = "";
    time_t     now = time(NULL);
    struct tm  tm;
    int32_t    fq  = cparam_i_get(cfg_fg_freq);
    int        c   = clock_cpu_temp();  /* the processor's, the kernel's cpu-thermal zone (sun4i-gpadc-iio) */

    localtime_r(&now, &tm);             /* the radio's time zone (Settings > General > DATE / TIME), named */
    strftime(t, sizeof(t), "%H:%M %Z", &tm);
    if (c != CLOCK_NO_TEMP)             /* after the time, as the other clocks show it; none read: none shown */
        snprintf(tc, sizeof(tc), " %dC", c);
    /* the battery's % after it, and its icon after that: the bolt charging, the plug on a supply not charging, the
     * battery's level on the battery (the base's last word kept: its reports come and go) */
    static float   l_bat, l_ext;
    static uint8_t l_cap;
    static bool    l_chg;
    {
        float   e, b;
        uint8_t cp;
        bool    ch;

        clock_power_get(&e, &b, &cp, &ch);
        if (b > 1 || e > 1) l_bat = b, l_ext = e, l_cap = cp, l_chg = ch;
    }
    const char *icon = "";
    uint32_t bc = 0;
    if (l_bat >= 3) {
        bc = batt_rgb(l_cap);
        if (bc) snprintf(tc + strlen(tc), sizeof(tc) - strlen(tc), " #%06X %d%%#", (unsigned)bc, l_cap);
        else snprintf(tc + strlen(tc), sizeof(tc) - strlen(tc), " %d%%", l_cap);
        icon = l_chg && l_cap < 100 ? LV_SYMBOL_CHARGE : l_ext >= 3 ? SYMBOL_PLUG
             : l_cap > 80 ? LV_SYMBOL_BATTERY_FULL : l_cap > 55 ? LV_SYMBOL_BATTERY_3 : l_cap > 30 ? LV_SYMBOL_BATTERY_2
             : l_cap > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    }
    snprintf(f, sizeof(f), "%d.%03d.%02d", fq / 1000000, (fq / 1000) % 1000, (fq % 1000) / 10);
    static const char *const modes[] = {"AUTO-SCAN", "SEMI-AUTO", "MAN-OVERRIDE"};

    /* The devices on USB, at the end in Xiegu's blue (its WR12 logo): the stick, an ADS-B receiver, the WR12 */
    char dev[96];
    dev[0] = 0;
    if (sdr_here) snprintf(dev + strlen(dev), sizeof(dev) - strlen(dev), "  #%s RTL#", tag_hex);
    if (adsb_here) snprintf(dev + strlen(dev), sizeof(dev) - strlen(dev), "  #%s ADS-B#", tag_hex);
    if (wr12_here) snprintf(dev + strlen(dev), sizeof(dev) - strlen(dev), "  #%s WR12#", tag_hex);

    char pre[160];

    if (scr == SCR_RTL || (scr == SCR_LOG && g_on >= 0 && hits[g_on].rtl)) {   /* the screen's radio: the stick */
        int64_t hz = scr == SCR_LOG ? g_hz : listen_hz;

        snprintf(f, sizeof(f), "RTL %lld.%03lld.%02lld", (long long)(hz / 1000000), (long long)(hz / 1000 % 1000),
                 (long long)(hz % 1000 / 10));
        snprintf(pre, sizeof(pre), "%s %s  %s  %s  %s%s", f, rtl_mode_names[rtl_mode_of(hz)],
                 rx_held ? "RX ONLY" : "TX NOT LOCKED", modes[smode], t, tc);
    } else {
        snprintf(pre, sizeof(pre), "%s %s%s%s  %s  %s  %s%s", f, mode_name(cparam_i_get(cfg_cur_mode)),
                 cparam_i_get(cfg_cur_pre) ? " PRE" : "", cparam_i_get(cfg_cur_att) ? " ATT" : "",
                 rx_held ? "RX ONLY" : "TX NOT LOCKED", modes[smode], t, tc);
    }
    /* the line, room for the icon after the %, then the devices; the icon where that room is */
    char line[240];
    snprintf(line, sizeof(line), "%s%s%s", pre, icon[0] ? "   " : "", dev);
    lv_label_set_text(status_l, line);
    lv_label_set_text(batt_l, icon);
    lv_obj_set_style_text_color(batt_l, bc ? lv_color_hex(bc) : amb, 0);
    if (icon[0]) {
        const lv_font_t *fn = &pixel_16;
        lv_coord_t       ls = lv_obj_get_style_text_letter_space(status_l, 0);
        lv_coord_t       w  = lv_txt_get_width(line, strlen(line), fn, ls, LV_TEXT_FLAG_RECOLOR);
        lv_coord_t       p  = lv_txt_get_width(pre, strlen(pre), fn, ls, LV_TEXT_FLAG_RECOLOR);

        lv_obj_set_pos(batt_l, 60 + (SW - 120 - w) / 2 + p + 6, 27);
    }
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
    opt_set(&b_rout, 0, "SCAN", routine == ROUT_SCAN);
    opt_set(&b_rout, 1, "SWEEP", routine == ROUT_SWEEP);
    {
        int n = 0;
        for (int i = 0; i < BANDS_N; i++) n += band_rt[i].sweep;
        if (smode == MODE_MAN)
            snprintf(s, sizeof(s), "KNOB HAS IT");
        else if (routine == ROUT_SWEEP)
            snprintf(s, sizeof(s), "%d BANDS", n);
        else if (routine == ROUT_SCAN && scan_a_band >= 0)
            snprintf(s, sizeof(s), "ALL OF %s", bands[scan_a_band].name);
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
    if (bi >= 0 && strcmp(bands[bi].name, "11 M CB") == 0) {     /* CB: the channels in the picture */
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
        opt_set(&b_prof, 1, "A'S MFK PUSH LOCKS ON", 2);
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

/* SCAN CFG's last lines: the latest the scans locked on, A's and C's (B LOG has them all) */
static void show_log(void) {
    char s[320] = "";

    for (int i = 0; i < nhits && i < 3; i++) {
        struct tm tm;
        char      t[8];

        localtime_r(&hits[i].last, &tm);
        strftime(t, sizeof(t), "%H:%M", &tm);
        snprintf(s + strlen(s), sizeof(s) - strlen(s), "%s %-5s %4lld.%03lld %.22s\n", t, hits[i].rtl ? "RTL" : "X6200",
                 (long long)(hits[i].hz / 1000000), (long long)(hits[i].hz / 1000 % 1000), hits[i].what);
    }
    lv_label_set_text(t_log_l, s);
}

/* A tap on the locked frequency (SCAN CFG from A): never locked on again until the Sentry closes */
static void ignore_tap_cb(lv_event_t *e) {
    (void)e;
    if (scr != SCR_TRACK || cfg_from != SCR_SCAN || !trk.on) return;
    if (n_ignored < IGNORED) ignored[n_ignored++] = trk.hz;
    osd("IGNORED", "NOT LOCKED ON AGAIN UNTIL THE SENTRY CLOSES.", -1);
    track_end();
}

static void show_cfg_rtl(void);
static void show_walkie(void);
static void walkie_step(void);
static void walkie_ch(int dir, int to);
static void walkie_show(bool on);
static void walkie_tx_note(void);
static bool g_portrait;         /* G held up: its own portrait screen shows (walkie_show) */
static bool g_armed;            /* G's transmitter armed (the A/B key): the Sentry's receive-only hold let go */
static int  w_at = -1;          /* G's channel (w_chs[]), -1 none chosen yet */
static bool w_rtl_now(void);    /* G shows a channel the stick serves */
static void walkie_group(void);
static void walkie_mode_next(void);
static void walkie_key_label(int k, char *s, size_t n);
static void walkie_arm(bool on);

static void show_track(void) {
    char s[96];

    if (cfg_from == SCR_RTL) {
        show_cfg_rtl();
        return;
    }
    lv_label_set_text(b_mode.cap, "SYSTEM MODE");
    lv_label_set_text(b_sig.cap, "SIGNAL STATUS");
    lv_label_set_text(b_rout.cap, "SCAN ROUTINE");
    lv_label_set_text(b_band.cap, "TARGET BAND");
    lv_label_set_text(b_tgts.cap, "TARGETS");
    show_scan();
    show_log();

    if (!trk.on) {
        lv_label_set_text(t_state_l, "NO TARGET");
        lv_label_set_text(t_crit_l, "SEARCHING");
        lv_label_set_text(t_big_l, "---");
        lv_label_set_text(t_sub_l, "");
        lv_label_set_text(t_time_l, "0.00");
        lv_label_set_text(t_id_l, "AUTO (F1) LOCKS ON WHAT A'S SCAN FINDS");
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
        char rx[24];                    /* which radio takes it: the stick (in the tags' orange) or the X6200 */
        if (band_uses_rtl(i)) snprintf(rx, sizeof(rx), sdr_here ? "#%s RTL#" : "RTL", tag_hex);
        else snprintf(rx, sizeof(rx), "%s", cfg_is_valid_hw_freq(bands[i].listen_hz) ? "X6200" : "");
        snprintf(s, sizeof(s), BAND_ROW, band_rt[i].sweep ? '*' : ' ',
                 bands[i].name, mhz, bands[i].hear, best_name(bands[i].when), bands[i].ant, now,
                 rx);
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
        char rx[24];                    /* the radio it is heard on: the X6200 (its apps), else the stick */
        if (cfg_is_valid_hw_freq(f)) snprintf(rx, sizeof(rx), "X6200");
        else snprintf(rx, sizeof(rx), f >= RTL_SPEC_MIN_HZ ? "#%s RTL#" : "", tag_hex);
        snprintf(s, sizeof(s), DEC_ROW, fs, bi >= 0 ? bands[bi].name : "", bi >= 0 ? best_name(bands[bi].when) : "",
                 bi >= 0 ? bands[bi].ant : "", now, rx);
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
    int32_t     lo, hi;         /* the band F1 (SCAN) sweeps, end to end */
    const char *about;
} rtl_presets[] = {
    {"NOAA WX", 162475000, 162400000, 162550000, "WEATHER RADIO 162.400-.550, AIS 161.975 / 162.025"},
    {"2 M", 145000000, 144000000, 148000000, "HAMS 144-148: APRS 144.390, THE ISS 145.800"},
    {"2 M RPT", 147000000, 146000000, 148000000, "HAMS 146-148: REPEATERS AND SIMPLEX"},
    {"AIRBAND", 121000000, 118000000, 137000000, "AIRCRAFT VOICE (AM) 118-137, GUARD 121.500"},
    {"FM BC", 98000000, 88000000, 108000000, "FM BROADCAST 88-108 MHZ"},
    {"MARINE", 156800000, 156000000, 162025000, "MARINE VHF, CALLING CHANNEL 16 156.800"},
    {"WX SAT", 137500000, 137000000, 138000000, "METEOR-M WEATHER SATELLITES 137.100 / 137.900"},
    {"SONDES", 403000000, 400000000, 406000000, "WEATHER BALLOONS 400-406 MHZ"},
    {"70 CM", 446000000, 420000000, 450000000, "HAMS 420-450: SIMPLEX 446.000"},
    {"ISM 433", 433920000, 433050000, 434790000, "SENSORS, KEY FOBS, WEATHER STATIONS"},
    {"GMRS", 462600000, 462550000, 467725000, "GMRS AND FRS RADIOS"},
    {"ISM 915", 915000000, 902000000, 928000000, "METERS AND SENSORS 902-928 MHZ"},
    {"UAT 978", 978000000, 977000000, 979000000, "AIRCRAFT UAT, 978 MHZ"},
    {"ADS-B", 1090000000, 1089000000, 1091000000, "AIRCRAFT, 1090 MHZ (AIRSPACE DECODES THEM)"},
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
static lv_obj_t     *hf_peak_l[RTL_PEAKS];     /* the X6200's half: A's signals, named */
static box_t         r_dev, r_tune, r_pre, r_sig;
static lv_timer_t   *rtl_timer;
static int           rtl_img_h, rtl_preset, rtl_step = 2, rtl_drop;

/* F's band: a preset (F2), or one of C's bands the stick takes (C's LISTEN): its name, what it is, what F1 scans */
static int f_band = -1;

static int64_t f_lo(void) {
    return f_band >= 0 ? (int64_t)bands[f_band].lo * 1000 : rtl_presets[rtl_preset].lo;
}

static int64_t f_hi(void) {
    return f_band >= 0 ? (int64_t)bands[f_band].hi * 1000 : rtl_presets[rtl_preset].hi;
}

static const char *f_name(void) {
    return f_band >= 0 ? bands[f_band].name : rtl_presets[rtl_preset].name;
}

static const char *f_about(void) {
    return f_band >= 0 ? bands[f_band].hear : rtl_presets[rtl_preset].about;
}
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
/* The view of the spectrum screen shown: A's (the X6200 alone at first) or C's (the stick alone at first) */
static int spec_view(void) {
    return param_i_get(scr == SCR_SCAN ? cfg_a_view : cfg_rtl_view);
}

static void rtl_layout(void) {
    int  view  = spec_view();
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

/* MHz without the zeros at the end: 118, 162.55, 467.725 */
static void mhz_short(char *s, size_t n, int64_t hz) {
    snprintf(s, n, "%lld.%03lld", (long long)(hz / 1000000), (long long)((hz / 1000) % 1000));
    for (char *e = s + strlen(s) - 1; e > s && (*e == '0' || *e == '.'); e--) {
        bool dot = *e == '.';
        *e       = 0;
        if (dot) break;
    }
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

/* The X6200's half: its strongest signals over the noise (A's targets), named as A names them, over the trace */
static void hf_labels(void) {
    int  iy = lv_obj_get_y(rtl_img), used[RTL_PEAKS], nl = 0;
    bool taken[MAX_TARGETS] = {false};

    for (int k = 0; k < RTL_PEAKS && spec_view() != VIEW_RTL; k++) {
        int b = -1;

        for (int i = 0; i < ntg; i++)
            if (!taken[i] && !tg[i].ignored && tg[i].con >= 8 && llabs(tg[i].hz - centre) < VIEW_HZ / 2 &&
                (b < 0 || tg[i].con > tg[b].con))
                b = i;
        if (b < 0) break;
        taken[b] = true;
        int x = (int)((tg[b].hz - (centre - VIEW_HZ / 2)) / HZ_PX), near = 0;

        for (int j = 0; j < nl; j++)
            if (abs(used[j] - x) < 70) near++;
        if (near > 1) continue;
        used[nl] = x;
        ident_t id;
        identify_ex(tg[b].hz, &id, false);
        lv_label_set_text(hf_peak_l[nl], id.label);
        lv_obj_set_pos(hf_peak_l[nl], IMG_X + x - 28, iy + p_hf.y0 + 4 + near * 16);
        lv_obj_clear_flag(hf_peak_l[nl], LV_OBJ_FLAG_HIDDEN);
        nl++;
    }
    for (int i = nl; i < RTL_PEAKS; i++) lv_obj_add_flag(hf_peak_l[i], LV_OBJ_FLAG_HIDDEN);
}

/* A's MFK: how far a click pans the X6200 (SCAN CFG's F3 when opened from A) */
static const int32_t     a_steps[]    = {1000, 5000, 10000, 25000, 100000};
static const char *const a_step_n[]   = {"1K", "5K", "10K", "25K", "100K"};
static int               a_step       = 2;

/* A's boxes, C's look: the X6200, its dial, its band, its signals */
static void show_a_boxes(void) {
    char    t[64], f[24];
    int32_t fq = cparam_i_get(cfg_fg_freq);
    bool    in;
    int     bi = band_near(fq, &in), b = -1;

    lv_label_set_text(r_pre.cap, "BAND");
    if (radio_get_state() == RADIO_TX) opt_set(&r_dev, 0, "X6200  ON AIR", 1);
    else if (g_armed) opt_set(&r_dev, 0, blink ? "X6200  TX ARMED" : "X6200", 1);
    else opt_set(&r_dev, 0, "XIEGU X6200", 0);
    snprintf(t, sizeof(t), "HEARD: %s", hear_names[heard]);
    opt_set(&r_dev, 1, t, listen_x62 ? 1 : 2);
    snprintf(f, sizeof(f), "%d.%03d.%02d", fq / 1000000, (fq / 1000) % 1000, (fq % 1000) / 10);
    snprintf(t, sizeof(t), "%s %s", f, mode_name(cparam_i_get(cfg_cur_mode)));
    opt_set(&r_tune, 0, t, 0);
    snprintf(t, sizeof(t), "360 KHZ / %s", a_step_n[a_step]);
    opt_set(&r_tune, 1, t, 2);
    if (routine == ROUT_SCAN && scan_a_band >= 0) bi = scan_a_band;
    {
        char bn[24];

        snprintf(bn, sizeof(bn), "%s%s", in || routine == ROUT_SCAN ? "" : "NEAR ", bi >= 0 ? bands[bi].name : "-");
        opt_set(&r_pre, 0, bn, 0);
    }
    if (routine == ROUT_SCAN && bi >= 0) {
        char lo[16], hi[16];

        mhz_short(lo, sizeof(lo), bands[bi].lo * 1000LL);
        mhz_short(hi, sizeof(hi), bands[bi].hi * 1000LL);
        snprintf(t, sizeof(t), "SCAN %s-%s MHZ", lo, hi);
        if (strlen(t) > 17) t[strlen(t) - 4] = 0;
        opt_set(&r_pre, 1, t, 1);
    } else {
        opt_set(&r_pre, 1, bi >= 0 ? bands[bi].hear : "", 2);
    }
    for (int i = 0; i < ntg; i++)
        if (!tg[i].ignored && (b < 0 || tg[i].con > tg[b].con)) b = i;
    snprintf(t, sizeof(t), "SIGNALS: %d", nlive);
    lv_label_set_text(r_sig.cap, t);
    if (b >= 0 && tg[b].con >= 8) {
        ident_t id;

        identify_ex(tg[b].hz, &id, false);
        snprintf(t, sizeof(t), "%d.%03d %s", tg[b].hz / 1000000, (tg[b].hz / 1000) % 1000, id.label);
        opt_set(&r_sig, 0, t, 0);
        snprintf(t, sizeof(t), "+%.0f DB  %s", tg[b].con, smode == MODE_AUTO ? "AUTO" : smode == MODE_SEMI ? "SEMI" : "MANUAL");
    } else {
        opt_set(&r_sig, 0, "ONLY NOISE", 0);
        snprintf(t, sizeof(t), "%s", smode == MODE_AUTO ? "AUTO-SCAN" : smode == MODE_SEMI ? "SEMI-AUTO" : "MAN-OVERRIDE");
    }
    opt_set(&r_sig, 1, t, 2);
    /* the stick's half, split: why it is empty, as C says */
    rtl_spec_state_t st = rtl_spectrum_state();
    static const char *const why[] = {"", "THE STICK'S DAEMON DOES NOT START\n\n/USR/BIN/X6X00-RTLSPEC", "NO RTL-SDR ON USB\n\nPLUG ONE INTO THE HOST PORT",
                                      "THE STICK IS BUSY\n\nWAITING FOR IT", "OPENING THE RTL-SDR", "",
                                      "THE RTL-SDR SENDS NOTHING\n\nUNPLUG IT AND PLUG IT IN AGAIN"};
    lv_label_set_text(rtl_why_l, why[st]);
    if (spec_view() == VIEW_X6200 || st == RTL_SPEC_ON || st == RTL_SPEC_OFF) lv_obj_add_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
}

static int cmp_f(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

/* The stick's strongest signal off its own picture (F's scan running behind another screen, SCAN CFG): the bin highest
 * over the row's median within the view, 10 dB over at least; 0 none */
static void rtl_pick_bg(const float *db, uint16_t n, uint32_t rate, int64_t c) {
    static float sorted[8192];
    int64_t      half = rtl_view_hz() / 2;
    int          k0   = (int)(n / 2 - (double)half * n / rate), k1 = (int)(n / 2 + (double)half * n / rate), b = -1;

    if (!n || !rate) return;
    memcpy(sorted, db, (size_t)n * sizeof(float));
    qsort(sorted, n, sizeof(float), cmp_f);
    float med = sorted[n / 2];

    for (int k = k0 < 0 ? 0 : k0; k < k1 && k < n; k++)
        if (abs(k - n / 2) > 2 && (b < 0 || db[k] > db[b])) b = k;
    if (b >= 0 && db[b] - med >= 10) {
        rtl_best    = c - rate / 2 + (int64_t)((b + 0.5) * rate / n);
        rtl_best_db = db[b];
    } else {
        rtl_best = 0;
    }
}

static void show_rtl(void) {
    if (scr == SCR_SCAN) {
        show_a_boxes();
        return;
    }
    lv_label_set_text(r_pre.cap, "PRESET");
    if (spec_view() == VIEW_X6200) {        /* its peaks not drawn: the strongest the stick has all the same */
        char b[40], bf[16];

        fmt_mhz(bf, sizeof(bf), rtl_best, 3);
        snprintf(b, sizeof(b), "%s %s", bf, rtl_name(rtl_best));
        opt_set(&r_sig, 0, rtl_best ? b : "ONLY NOISE", 0);
        lv_label_set_text(r_sig.cap, "SIGNALS");
    }
    static const char *const st[] = {"OFF", "ITS DAEMON DOES NOT START", "NO RTL-SDR ON USB", "STICK BUSY: WAITING",
                                     "OPENING THE STICK", "RECEIVING", "STICK SENDS NOTHING"};
    rtl_spec_state_t s = rtl_spectrum_state();
    char             t[48], tn[16], f[16];
    int              g = param_i_get(cfg_rtl_gain);

    /* No stick for a moment (the first second of a start, a replug) changes nothing: its audio is silent meanwhile,
     * RECEIVER says why, and it is heard again as soon as it is back (the daemon is told what to play as the GUI
     * connects) */
    rtl_spectrum_tuner(tn, sizeof(tn));
    snprintf(t, sizeof(t), "RTL-SDR %s", tn[0] ? tn : "");
    opt_set(&r_dev, 0, t, 0);
    if (s == RTL_SPEC_ON) snprintf(t, sizeof(t), "HEARD: %s", hear_names[heard]);
    opt_set(&r_dev, 1, s == RTL_SPEC_ON ? t : st[s], s == RTL_SPEC_ON ? (listen_rtl ? 1 : 2) : 0);
    fmt_mhz(f, sizeof(f), rtl_hz, 3);
    snprintf(t, sizeof(t), "%s MHZ", f);
    opt_set(&r_tune, 0, t, 0);
    static const char *const steps[] = {"12.5K", "25K", "100K", "500K", "1M"};
    snprintf(t, sizeof(t), "%.1f MHZ / %s", rtl_view_hz() / 1e6, steps[rtl_step]);
    opt_set(&r_tune, 1, t, 2);
    opt_set(&r_pre, 0, f_name(), 0);
    if (rtl_scan) {
        char lo[16], hi[16];

        mhz_short(lo, sizeof(lo), f_lo());
        mhz_short(hi, sizeof(hi), f_hi());
        snprintf(t, sizeof(t), "SCAN %s-%s MHZ", lo, hi);
        if (strlen(t) > 17) t[strlen(t) - 4] = 0;      /* the box's width: the unit goes first */
        opt_set(&r_pre, 1, t, 1);
    } else {
        opt_set(&r_pre, 1, f_about(), 2);
    }
    if (g < 0) snprintf(t, sizeof(t), "GAIN AUTO");
    else snprintf(t, sizeof(t), "GAIN %d.%d DB", g / 10, g % 10);
    opt_set(&r_sig, 1, t, 2);
    /* No rows: why, in the picture */
    static const char *const why[] = {"", "THE STICK'S DAEMON DOES NOT START\n\n/USR/BIN/X6X00-RTLSPEC", "NO RTL-SDR ON USB\n\nPLUG ONE INTO THE HOST PORT",
                                      "THE STICK IS BUSY\n\nWAITING FOR IT", "OPENING THE RTL-SDR", "",
                                      "THE RTL-SDR SENDS NOTHING\n\nUNPLUG IT AND PLUG IT IN AGAIN"};
    lv_label_set_text(rtl_why_l, why[s]);
    if (s == RTL_SPEC_ON || s == RTL_SPEC_OFF) lv_obj_add_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
}

/* The stick's tuning, sent, and kept in the settings when keep (a scan's steps are not) */
static void rtl_tune_to(int64_t hz, bool keep) {
    if (hz < RTL_SPEC_MIN_HZ) hz = RTL_SPEC_MIN_HZ;
    if (hz > RTL_SPEC_MAX_HZ) hz = RTL_SPEC_MAX_HZ;
    rtl_hz = hz;
    stick_at = hz;
    if (keep) param_i_set(cfg_rtl_freq, (int32_t)hz);
    rtl_spectrum_tune(hz);
    show_rtl();
}

static void rtl_tune(int64_t hz) {
    rtl_tune_to(hz, true);
}


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

/* Each receiver at its own volume: the X6200's (cfg_volume) and the stick's (cfg_rtl_vol). The base's speaker is at the
 * one heard, BOTH at the louder of the two with the other scaled down in the mix (rtl_spectrum_levels) */
static void hear_vol_refresh(void) {
    int x = param_i_get(cfg_volume), r = param_i_get(cfg_rtl_vol), m = x > r ? x : r;

    if (heard == HEAR_RTL) {
        radio_set_vol_override(r);
        rtl_spectrum_levels(1, 0);
    } else if (heard == HEAR_BOTH) {
        radio_set_vol_override(m);
        rtl_spectrum_levels(m ? powf((float)r / m, 1.5f) : 0, m ? powf((float)x / m, 1.5f) : 0);
    } else {
        radio_set_vol_override(-1);
        rtl_spectrum_levels(1, 1);
    }
}

/* The speaker: the X6200's receiver, the stick at F's channel (listen_hz), or both mixed (rtl_spectrum_mux). The stick
 * goes back to F's place for it if G moved it; no stick, the X6200 for now */
static void hear_apply(int h) {
    if ((h == HEAR_RTL || h == HEAR_BOTH) && !sdr_here && rtl_spectrum_state() != RTL_SPEC_ON) h = HEAR_X6200;
    heard = h;
    rtl_spectrum_mute(h == HEAR_OFF);
    if (h == HEAR_X6200 || h == HEAR_OFF) {
        rtl_spectrum_mux(false);
        rtl_spectrum_listen(0, 0);
    } else {
        if (!rtl_hz) rtl_hz = param_i_get(cfg_rtl_freq);
        if (stick_at != rtl_hz) {
            rtl_spectrum_tune(rtl_hz);
            stick_at = rtl_hz;
        }
        if (!listen_hz || llabs(listen_hz - rtl_hz) > rtl_view_hz() / 2) listen_hz = rtl_snap(rtl_best ? rtl_best : rtl_hz);
        rtl_spectrum_mux(h == HEAR_BOTH);
        rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
    }
    hear_vol_refresh();
    show_head();
}

/* F5 on A or F: that screen's LISTEN, the next one, heard at once */
static void hear_next(void) {
    ParamInt *c = scr == SCR_RTL ? cfg_hear_f : cfg_hear_a;
    int       h = (param_i_get(c) + 1) % HEAR_N;
    char      t[160];
    static const char *const how[] = {"THE X6200'S RECEIVER ON THE SPEAKER.",
                                      "THE RTL-SDR ON THE SPEAKER, AT B'S CHANNEL: %s %s.",
                                      "THE X6200 AND THE RTL-SDR TOGETHER (B'S CHANNEL: %s %s).",
                                      "THE SPEAKER SILENT: NEITHER RADIO IS HEARD (THEIR VOLUME KEPT)."};
    char f[16];

    if ((h == HEAR_RTL || h == HEAR_BOTH) && !sdr_here) {
        osd("NO RTL-SDR", "PLUG AN RTL-SDR INTO THE HOST PORT TO HEAR IT.", -1);
        h = HEAR_OFF;                   /* past RTL and BOTH: on to OFF */
    }
    param_i_set(c, h);
    hear_apply(h);
    fmt_mhz(f, sizeof(f), listen_hz ? listen_hz : rtl_hz, 3);
    snprintf(t, sizeof(t), how[h], f, rtl_mode_names[rtl_mode_of(listen_hz ? listen_hz : rtl_hz)]);
    char title[32];
    snprintf(title, sizeof(title), "%s LISTEN: %s", scr == SCR_RTL ? "B" : "A", hear_names[h]);
    osd(title, t, -1);
}

/* The VFO knob while the stick is listened to: a channel a click; the stick follows when it nears an edge */
static void rtl_listen_turn(int32_t diff) {
    rtl_scan_set(false);                /* a channel chosen by hand: the scan stops there */
    int64_t c = rtl_hz, half = rtl_view_hz() / 2;

    listen_hz = rtl_snap(listen_hz + diff * rtl_step_of(listen_hz));
    if (listen_hz < RTL_SPEC_MIN_HZ) listen_hz = RTL_SPEC_MIN_HZ;
    if (listen_hz > RTL_SPEC_MAX_HZ) listen_hz = RTL_SPEC_MAX_HZ;
    if (llabs(listen_hz - c) > half * 85 / 100) rtl_tune(listen_hz);
    if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));     /* muted: the bracket moves alone */
    show_head();
    show_rtl();
}

/* The VFO knob is the screen's radio's: on F the stick's channel always (heard or not), on G a stick entry playing */
static bool knob_taken(void) {
    return scr == SCR_RTL || (scr == SCR_LOG && g_on >= 0 && hits[g_on].rtl) || (w_rtl_now() && heard == HEAR_RTL);
}

/* F shown (on) or left: its picture again, at its own tuning (G may have moved the stick); F left as it is otherwise:
 * band, tuning, channel, the scan (paused). What is heard is go()'s business (hear_apply) */
static void rtl_want(bool on) {
    if (on == rtl_wanted) return;
    rtl_wanted = on;
    if (!rtl_hz) rtl_hz = param_i_get(cfg_rtl_freq);
    rtl_spectrum_squelch(param_i_get(cfg_rtl_sql));
    if (on) {
        if (stick_at != rtl_hz) {
            rtl_spectrum_tune(rtl_hz);
            stick_at = rtl_hz;
        }
        if (!listen_hz) listen_hz = rtl_snap(rtl_hz);
        rtl_seq = 0;
        rtl_drop = 2;
        rtl_layout();
        if (rtl_scan) scan_next = lv_tick_get() + SCAN_DWELL_MS;     /* a picture where it was, then on */
    }
}

/* F1 on F: the preset's band swept end to end, low to high and round again, each picture SCAN_DWELL_MS, the next one
 * nine tenths of a picture on so nothing falls between two; the last one ends at the band's top. A band within one
 * picture is just shown. Listening, the speaker goes to the middle of each new picture and then to its strongest
 * signal, once its rows are in */
static void rtl_scan_set(bool on) {
    if (on == rtl_scan) return;
    rtl_scan = on;
    if (on) {
        scan_next = 0;                  /* at once: to the band's low end */
        scan_pick = 0;
    } else if (rtl_hz) {
        param_i_set(cfg_rtl_freq, (int32_t)rtl_hz);     /* where it stopped, kept */
    }
    if (scr == SCR_RTL) buttons_refresh(&btn[0]);       /* F1's On or Off at once (a pan stops it too) */
    show_rtl();
}

static void rtl_scan_step(void) {
    uint32_t now = lv_tick_get();

    /* on whatever screen (B's log and SCAN CFG see it), but not while G plays a stick entry of its own */
    if (!rtl_scan || rtl_spectrum_state() != RTL_SPEC_ON || (scr == SCR_LOG && g_on >= 0 && hits[g_on].rtl) ||
        w_rtl_now())
        return;
    if (scan_pick && now >= scan_pick) {
        scan_pick = 0;
        if (rtl_best) {                 /* the picture's strongest: locked on, into G's log */
            int64_t hz = rtl_snap(rtl_best);

            hit_add(hz, rtl_mode_of(hz), true, rtl_name(hz), rtl_best_db);
        }
        if (rtl_best) {                 /* F's channel to it; heard if the stick is */
            listen_hz = rtl_snap(rtl_best);
            if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
            show_head();
        }
    }
    if (scan_next && now < scan_next) return;

    int64_t lo = f_lo(), hi = f_hi(), half = rtl_view_hz() / 2, next;

    if (hi - lo <= 2 * half)
        next = (lo + hi) / 2;
    else if (!scan_next || rtl_hz < lo + half || rtl_hz + half >= hi)
        next = lo + half;               /* the start, or past the top: from the low end again */
    else if ((next = rtl_hz + half * 9 / 5) + half > hi)
        next = hi - half;
    scan_next = now + SCAN_DWELL_MS;
    if (next == rtl_hz) return;
    rtl_tune_to(next, false);
    scan_pick = now + SCAN_PICK_MS;     /* its strongest, once its rows are in */
    listen_hz = rtl_snap(next);
    if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
    show_head();
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
    rtl_scan_step();
    if (scr == SCR_WALKIE) walkie_step();
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
    if (scr != SCR_RTL && scr != SCR_SCAN) {     /* the stick's rows read all the same: F's scan, SCAN CFG */
        bool got = false;

        while (rtl_spectrum_frame(rtl_db, N_OF(rtl_db), &n, &rate, &c, &rtl_seq, &fresh)) {
            if (fresh) rtl_drop = 2;
            if (rtl_drop > 0) {
                rtl_drop--;
                continue;
            }
            rtl_n = n, rtl_rate = rate, rtl_centre = c, got = true;
        }
        if (got) rtl_pick_bg(rtl_db, rtl_n, rtl_rate, rtl_centre);
        return;
    }
    static uint32_t shown_ms;
    if (lv_tick_elaps(shown_ms) > 1000) {   /* the boxes, once a second */
        shown_ms = lv_tick_get();
        show_rtl();
    }
    /* The X6200 above, split: A's own average, a row each frame of it, its signals named, the dial bracketed */
    int view = spec_view();

    if (view != VIEW_RTL && centre && nbins && last_seq != hf_seen) {
        hf_seen = last_seq;
        to_cols(avg_db, nbins, centre - span_hz / 2.0, bin_hz, centre - VIEW_HZ / 2, HZ_PX, hf_cols);
        panel_trace(&p_hf, hf_cols);
        {   /* the dial: the X6200's channel, as the stick's is bracketed below */
            uint32_t *px = (uint32_t *)rtl_frame->data, line = argb(amb);
            int       x  = RTL_COLS / 2;

            for (int y = p_hf.y0 + 2; y < p_hf.y0 + p_hf.trace_h - 1; y++)
                for (int dx = -10; dx <= 10; dx += 20) px[y * RTL_COLS + x + dx] = line;
            for (int dx = -10; dx <= 10; dx++)
                if (dx < -6 || dx > 6)
                    px[(p_hf.y0 + 2) * RTL_COLS + x + dx] = px[(p_hf.y0 + p_hf.trace_h - 2) * RTL_COLS + x + dx] = line;
        }
        panel_row(&p_hf, hf_cols);
        rtl_ticks(hf_tick_l, &p_hf, centre - VIEW_HZ / 2, HZ_PX, 50000, false);
        hf_labels();
        fmt_freq(f, sizeof(f), centre);
        snprintf(s, sizeof(s), "X6200  %s  %s", f, listen_x62 ? "HEARD" : "MUTED");
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
        if (view == VIEW_X6200) {           /* the stick's half not shown: its strongest found all the same */
            rtl_pick_bg(rtl_db, n, rate, c);
            continue;
        }
        panel_trace(&p_rtl, rtl_cols);
        if (listen_hz) {                    /* F's channel (the VFO knob's): a bracket on the trace */
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

        rtl_best    = best_hz;
        rtl_best_db = best_db;
        rtl_spectrum_watch(wx);             /* SAME alerts from the strongest NOAA channel in view */
        rtl_ticks(rtl_tick_l, &p_rtl, lo, px_hz, every, true);
        fmt_mhz(f, sizeof(f), rtl_centre, 3);
        if (listen_hz) {
            char lf[16];
            fmt_mhz(lf, sizeof(lf), listen_hz, 3);
            snprintf(s, sizeof(s), "RTL  %s %s  %s", lf, rtl_mode_names[rtl_mode_of(listen_hz)],
                     !listen_rtl ? "MUTED" : rtl_spectrum_squelch_open() ? "HEARD" : "SQUELCHED");
        } else {
            snprintf(s, sizeof(s), "RTL-SDR  %s MHZ", f);
        }
        lv_label_set_text(rtl_src_l[1], s);
        if (scr == SCR_RTL) {               /* C's box (A's shows the X6200's: show_a_boxes) */
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
    }
    if (drew) lv_obj_invalidate(rtl_img);
}

/* A tap on the stick's half: that frequency to the middle */
static void rtl_touch_cb(lv_event_t *e) {
    lv_point_t p;
    lv_area_t  a;

    (void)e;
    if (!lv_indev_get_act()) return;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_obj_get_coords(rtl_img, &a);
    if (scr == SCR_SCAN) {                  /* A: the X6200's half only. A signal tapped locked on, else tuned there */
        if (p.y - a.y1 >= p_rtl.y0 || !centre) return;
        int32_t         hz = (int32_t)(centre - VIEW_HZ / 2 + (p.x - a.x1 + 0.5) * HZ_PX);
        const target_t *t  = target_near(hz, 3000);

        if (routine == ROUT_SCAN) set_routine(ROUT_BAND);
        if (t) track_start(t->hz, false);
        else tune(hz / 100 * 100, -1);
        return;
    }
    if (!rtl_rate) return;
    if (p.y - a.y1 < p_rtl.y0) {             /* the X6200's half: C hears the X6200 */
        param_i_set(cfg_hear_f, HEAR_X6200);
        hear_apply(HEAR_X6200);
        show_rtl();
        return;
    }
    /* the stick's: that channel F's, heard (with the X6200 too if F hears both) */
    listen_hz = rtl_snap(rtl_centre - rtl_view_hz() / 2 + (int64_t)((p.x - a.x1 + 0.5) * rtl_px_hz()));
    if (param_i_get(cfg_hear_f) == HEAR_X6200) param_i_set(cfg_hear_f, HEAR_RTL);
    hear_apply(param_i_get(cfg_hear_f));
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
        hf_peak_l[i]  = label(p, &pixel_16, amb, 0, 0, 0, 0, "");
        lv_obj_add_flag(rtl_peak_l[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hf_peak_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    rtl_why_l = label(p, &pixel_24, amb, IMG_X, iy + rtl_img_h / 2 - 40, RTL_COLS, LV_TEXT_ALIGN_CENTER, "");
    /* which half is which, at the right of each trace */
    rtl_src_l[0] = label(p, &pixel_16, amb, IMG_X + RTL_COLS - 250, iy + 6, 244, LV_TEXT_ALIGN_RIGHT, "");
    rtl_src_l[1] = label(p, &pixel_16, amb, IMG_X + RTL_COLS - 250, iy + 6, 244, LV_TEXT_ALIGN_RIGHT, "");
    rtl_hz = param_i_get(cfg_rtl_freq);
    for (int i = RTL_PRESETS - 1; i >= 0; i--)  /* the preset the tuning is in (at, first), if one */
        if (rtl_hz >= rtl_presets[i].lo && rtl_hz <= rtl_presets[i].hi) rtl_preset = i;
    for (int i = 0; i < RTL_PRESETS; i++)
        if (rtl_presets[i].hz == rtl_hz) rtl_preset = i;
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
            if (f_band >= 0) f_band = -1;           /* from C's band back to the presets, at the one it was on */
            else rtl_preset = (rtl_preset + 1) % RTL_PRESETS;
            rtl_tune(rtl_presets[rtl_preset].hz);
            scan_next = 0;              /* scanning: the new band, from its low end */
            break;
        case 2:
            rtl_step = (rtl_step + 1) % N_OF(rtl_steps);
            break;
        case 3:
            /* A: X6200, SPLIT, RTL; C: RTL, SPLIT, X6200 (each its own radio first) */
            param_i_set(scr == SCR_SCAN ? cfg_a_view : cfg_rtl_view, (spec_view() + (scr == SCR_SCAN ? 2 : 1)) % 3);
            rtl_layout();
            rtl_labels_place();
            break;
        case 4:
            hear_next();
            break;
        case 5: {                       /* the MFK's push: the gain */
            int g = param_i_get(cfg_rtl_gain), i = 0;

            while (i < N_OF(rtl_gains) && rtl_gains[i] <= g) i++;      /* the next one up (the VOL knob may have */
            g = rtl_gains[i % N_OF(rtl_gains)];                         /* left it between), AUTO after the top */
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

/* On F the knob's SQL and RF GAIN are the stick's: its squelch (dB over the noise of its picture) and its tuner's gain;
 * the other screens', the X6200's */
static bool vol_stick(void) {
    return scr == SCR_RTL || (scr != SCR_SCAN && scr != SCR_WALKIE && heard == HEAR_RTL);
}

/* The R820T's gains in tenths of dB, after -1 (its AGC): the knob's RF GAIN on F walks them one a detent */
static const int r820t_gains[] = {-1,  0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229,
                                  254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};

static int vol_value(void) {
    switch (vol_fn) {
        case VF_SQL: return param_i_get(vol_stick() ? cfg_rtl_sql : cfg_squelch);
        case VF_RFG: return param_i_get(vol_stick() ? cfg_rtl_gain : cfg_rfgain);
        default: return vol_stick() ? param_i_get(cfg_rtl_vol) : radio_change_vol(0);
    }
}

/* The value as said: the X6200's a number; the stick's "12 DB" or "OFF", "28.0 DB" or "AUTO" */
static void vol_text(char *s, size_t n) {
    int v = vol_value();

    if (vol_stick() && vol_fn == VF_SQL)
        snprintf(s, n, v ? "%d DB" : "OFF", v);
    else if (vol_stick() && vol_fn == VF_RFG && v < 0)
        snprintf(s, n, "AUTO");
    else if (vol_stick() && vol_fn == VF_RFG)
        snprintf(s, n, "%d.%d DB", v / 10, v % 10);
    else if (vol_fn == VF_VOL && heard == HEAR_BOTH)        /* both heard: this one's, and the other's to match it */
        snprintf(s, n, "%d (%s %d)", v, vol_stick() ? "X6200" : "RTL",
                 vol_stick() ? param_i_get(cfg_volume) : param_i_get(cfg_rtl_vol));
    else
        snprintf(s, n, "%d", v);
}

/* Its bar, 0-100 */
static int vol_bar(void) {
    int v = vol_value();

    if (vol_stick() && vol_fn == VF_SQL) return v * 100 / 40;
    if (vol_stick() && vol_fn == VF_RFG) return v < 0 ? 0 : v * 100 / 496;
    if (vol_fn == VF_VOL) return v * 100 / BOARD_VOL_MAX;
    return v;
}

static void show_legend(void) {
    static const char *const mfk[SCR_N] = {"PAN, PUSH STRONGEST", "NEXT TARGET, PUSH HOLD", "BAND, PUSH LISTEN",
                                           "FREQ, PUSH OPEN", "ITEM, PUSH OPEN", "PAN, PUSH STRONGEST",
                                           "SIGNAL, PUSH LISTEN", "CHANNEL, PUSH CH 19"};

    char v[24];

    vol_text(v, sizeof(v));
    lv_label_set_text_fmt(legend_l, "VFO: TUNE | MFK: %s | RING: SCREEN | VOL: %s %s, PUSH %s | GEN: HELP",
                          scr == SCR_TRACK && cfg_from == SCR_RTL ? "-" : mfk[scr], vol_short[vol_fn], v,
                          vol_short[(vol_fn + 1) % 3]);
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
    static const char *const stick[] = {
        "THE STICK'S OWN, WHEREVER IT IS HEARD (THE X6200 KEEPS ITS OWN). PUSH: ITS SQUELCH.",
        "THE STICK'S: QUIET UNTIL A SIGNAL STANDS THIS MUCH OVER THE NOISE. OFF: ALWAYS OPEN. PUSH: RF GAIN.",
        "THE STICK'S TUNER: UP FOR WEAK SIGNALS, DOWN IF STRONG ONES SWAMP IT. AUTO: ITS AGC. PUSH: VOLUME."};
    char t[48], v[24];

    vol_text(v, sizeof(v));
    snprintf(t, sizeof(t), "%s%s %s", vol_fn == VF_VOL ? (vol_stick() ? "RTL " : "X6200 ") : "", vol_names[vol_fn], v);
    osd(t, (vol_stick() ? stick : how)[vol_fn], vol_bar());
    show_legend();
}

static void vol_turn(int d) {
    int v;

    switch (vol_fn) {
        case VF_SQL:
            if (vol_stick()) {
                v = param_i_get(cfg_rtl_sql) + d;
                v = v < 0 ? 0 : v > 40 ? 40 : v;
                param_i_set(cfg_rtl_sql, v);
                rtl_spectrum_squelch(v);
                break;
            }
            v = param_i_get(cfg_squelch) + 2 * d;
            param_i_set(cfg_squelch, v < 0 ? 0 : v > BOARD_SQL_MAX ? BOARD_SQL_MAX : v);
            break;
        case VF_RFG:
            if (vol_stick()) {
                int i = 0, g = param_i_get(cfg_rtl_gain);

                while (i < (int)N_OF(r820t_gains) - 1 && r820t_gains[i] < g) i++;
                i += d;
                i = i < 0 ? 0 : i >= (int)N_OF(r820t_gains) ? (int)N_OF(r820t_gains) - 1 : i;
                param_i_set(cfg_rtl_gain, r820t_gains[i]);
                rtl_spectrum_gain(r820t_gains[i]);
                p_rtl.floor = -300;
                show_rtl();
                break;
            }
            v = param_i_get(cfg_rfgain) + 2 * d;
            param_i_set(cfg_rfgain, v < 0 ? 0 : v > BOARD_RFG_MAX ? BOARD_RFG_MAX : v);
            break;
        default:
            if (vol_stick()) {          /* the stick's own volume, kept for wherever it is heard */
                v = param_i_get(cfg_rtl_vol) + d;
                param_i_set(cfg_rtl_vol, v < 0 ? 0 : v > BOARD_VOL_MAX ? BOARD_VOL_MAX : v);
            } else {
                radio_change_vol(d);
            }
            break;
    }
    hear_vol_refresh();
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

/* SCAN CFG opened from C: the stick as A's boxes show the X6200, and C's scan as the lock below them */
static void show_cfg_rtl(void) {
    char             t[64], tn[16], f[16];
    int              g = param_i_get(cfg_rtl_gain), q = param_i_get(cfg_rtl_sql), seen = 0;
    static int64_t   pick;
    static uint32_t  pick_ms;
    static const char *const steps[] = {"12.5 KHZ", "25 KHZ", "100 KHZ", "500 KHZ", "1 MHZ"};

    lv_label_set_text(b_mode.cap, "THE STICK");
    lv_label_set_text(b_sig.cap, "SIGNAL STATUS");
    lv_label_set_text(b_rout.cap, "SCAN ROUTINE");
    lv_label_set_text(b_band.cap, "TARGET BAND");
    lv_label_set_text(b_prof.cap, "SIGNAL PROFILE");
    lv_label_set_text(b_tgts.cap, "TARGETS");
    rtl_spectrum_tuner(tn, sizeof(tn));
    snprintf(t, sizeof(t), "RTL-SDR %s", tn);
    opt_set(&b_mode, 0, t, 0);
    if (g < 0) snprintf(t, sizeof(t), "GAIN AUTO");
    else snprintf(t, sizeof(t), "GAIN %d.%d DB", g / 10, g % 10);
    opt_set(&b_mode, 1, t, 2);
    if (q) snprintf(t, sizeof(t), "SQUELCH %d DB", q);
    else snprintf(t, sizeof(t), "SQUELCH OFF");
    opt_set(&b_mode, 2, t, 2);
    cap_set(&b_mode, true);
    opt_set(&b_tx, 0, rx_held ? "SAFE" : "NOT LOCKED", 1);
    opt_set(&b_tx, 1, "ARMED", 2);
    opt_set(&b_tx, 2, rx_held ? "(LOCKED)" : "", 2);
    opt_set(&b_sig, 0, rtl_scan ? (blink ? "SCANNING_" : "SCANNING") : "HOLDING", rtl_scan);
    opt_set(&b_sig, 1, listen_rtl ? "HEARD" : "MUTED", listen_rtl);
    opt_set(&b_sig, 2, listen_rtl && !rtl_spectrum_squelch_open() ? "SQUELCHED" : "", 2);
    opt_set(&b_rout, 0, "SCAN", rtl_scan);
    snprintf(t, sizeof(t), "DWELL %d S", scan_dwell_ms / 1000);
    opt_set(&b_rout, 1, t, 2);
    snprintf(t, sizeof(t), "STEP %s", steps[rtl_step]);
    opt_set(&b_rout, 2, t, 2);
    {
        char lo[16], hi[16];

        mhz_short(lo, sizeof(lo), f_lo());
        mhz_short(hi, sizeof(hi), f_hi());
        snprintf(t, sizeof(t), "%s  %s - %s", f_name(), lo, hi);
        opt_set(&b_band, 0, t, 0);
        opt_set(&b_band, 1, f_about(), 2);
    }
    if (rtl_best) {
        fmt_mhz(f, sizeof(f), rtl_best, 3);
        snprintf(t, sizeof(t), "%s %s", rtl_name(rtl_best), rtl_mode_names[rtl_mode_of(rtl_best)]);
        opt_set(&b_prof, 0, t, 1);
        snprintf(t, sizeof(t), "%s  %.0f DB", f, rtl_best_db);
        opt_set(&b_prof, 1, t, 0);
    } else {
        opt_set(&b_prof, 0, "NO SIGNAL", 2);
        opt_set(&b_prof, 1, "", 0);
    }
    for (int i = 0; i < nhits; i++) seen += hits[i].rtl;
    snprintf(t, sizeof(t), "%02d LOGGED ON THE STICK", seen);
    opt_set(&b_tgts, 0, t, 0);
    opt_set(&b_tgts, 1, "C LOG HAS THEM", 2);
    /* the lock: C's channel and its scan's pick */
    if (rtl_best && llabs(rtl_best - pick) > rtl_step_of(rtl_best)) {
        pick    = rtl_best;
        pick_ms = lv_tick_get();
    }
    lv_label_set_text(t_state_l, rtl_best ? "SIGNAL\nPICKED" : "NO\nSIGNAL");
    lv_label_set_text(t_crit_l, rtl_scan ? "SCANNING" : listen_rtl ? "LISTENING" : "HOLDING");
    lv_obj_set_style_bg_opa(t_crit_l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(t_crit_l, amb, 0);
    fmt_mhz(f, sizeof(f), listen_hz ? listen_hz : rtl_hz, 3);
    lv_label_set_text(t_big_l, f);
    snprintf(t, sizeof(t), "B'S CHANNEL, %s %s", rtl_mode_names[rtl_mode_of(listen_hz ? listen_hz : rtl_hz)],
             listen_rtl ? "HEARD" : "MUTED");
    lv_label_set_text(t_sub_l, t);
    snprintf(t, sizeof(t), "%.1f", rtl_best ? (lv_tick_get() - pick_ms) / 1000.0 : 0.0);
    lv_label_set_text(t_time_l, t);
    lv_label_set_text(t_id_l, rtl_scan ? "B'S SCAN GOES ON BEHIND THE OTHER SCREENS" : "B'S F1 SCANS ITS BAND");
    snprintf(t, sizeof(t), rtl_best ? "LEVEL %.0f DB" : "", rtl_best_db);
    lv_label_set_text(t_db_l, t);
    float sig = rtl_best ? (rtl_best_db + 100) / 60 : 0;
    sig = sig < 0 ? 0 : sig > 1 ? 1 : sig;
    lv_obj_set_height(t_sig_fill, (int)(sig * t_bar_h));
    lv_obj_set_y(t_sig_fill, t_bar_y + t_bar_h - (int)(sig * t_bar_h));
    lv_obj_set_height(t_noise_fill, 0);
    show_log();
}

static char scr_letter(scr_t s) {
    static const char letters[SCR_N] = {'A', 'A', 'D', 'E', 'F', 'B', 'C', 'G'};  /* by scr_t; SCR_TRACK: its screen's */

    return s == SCR_TRACK ? letters[cfg_from] : letters[s];
}

static void show_screen(void) {
    scr_t shown = scr == SCR_SCAN ? SCR_RTL : scr;        /* A is C's screen, set up for the X6200 */

    for (int i = 0; i < SCR_N; i++) {
        if (i == (int)shown)
            lv_obj_clear_flag(scr_obj[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(scr_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    char l[2] = {scr_letter(scr), 0};

    lv_label_set_text(badge_l[0], l);
    lv_label_set_text(badge_l[1], l);
    show_title();
    for (int i = 0; i < 5; i++) buttons_refresh(&btn[i]);
    show_keys();
    show_legend();
    switch (scr) {
        case SCR_SCAN: show_rtl(); rtl_labels_place(); break;
        case SCR_TRACK: show_track(); break;
        case SCR_BANDS: show_bands(); break;
        case SCR_DECODE: show_decode(); break;
        case SCR_SYSTEM: show_system(); break;
        case SCR_RTL: show_rtl(); rtl_labels_place(); break;
        case SCR_LOG: show_glog(); break;
        case SCR_WALKIE: show_walkie(); break;
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

/* ---- G LOG: what the scans locked on ---- */

#define LOG_ROW "%c%-10.10s %-8.8s %-22.22s %-5.5s %-12.12s %-13.13s %-5.5s %4.4s %s"
#define G_STICK_OFF 200000      /* the stick tuned this far under what it plays: its middle is not used */

static void hits_save(void) {
    char  tmp[sizeof(HITS_FILE) + 4];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.new", HITS_FILE);
    if (!(f = fopen(tmp, "w"))) return;
    for (int i = 0; i < nhits; i++)
        fprintf(f, "%lld\t%d\t%d\t%lld\t%lld\t%d\t%.1f\t%s\n", (long long)hits[i].hz, hits[i].mode, hits[i].rtl,
                (long long)hits[i].first, (long long)hits[i].last, hits[i].times, hits[i].peak, hits[i].what);
    /* on the card before the rename: DATA is FAT, and a power cut after a rename of data still in memory left the
     * log empty (2026-11-14, a radio switched off at its battery's end) */
    fflush(f);
    fsync(fileno(f));
    if (fclose(f) == 0 && rename(tmp, HITS_FILE) == 0) hits_dirty = false;
}

static void hits_load(void) {
    char  line[160];
    FILE *f = fopen(HITS_FILE, "r");

    hits_loaded = true;
    if (!f) return;
    while (nhits < HITS && fgets(line, sizeof(line), f)) {
        hit_t     h = {0};
        long long hz, first, last;
        int       rtl, n = 0;

        if (sscanf(line, "%lld\t%d\t%d\t%lld\t%lld\t%d\t%f\t%n", &hz, &h.mode, &rtl, &first, &last, &h.times, &h.peak,
                   &n) < 7 || !n)
            continue;
        h.hz    = hz;
        h.rtl   = rtl != 0;
        h.first = (time_t)first;
        h.last  = (time_t)last;
        snprintf(h.what, sizeof(h.what), "%s", line + n);
        h.what[strcspn(h.what, "\r\n")] = 0;
        hits[nhits++] = h;
    }
    fclose(f);
}

/* A lock: a new entry, or the same signal again (2 kHz on the X6200, half a channel on the stick) brought to the top;
 * the choice and the one heard stay on their entries */
static void hit_add(int64_t hz, int mode, bool rtl, const char *what, float peak) {
    int64_t near = rtl ? rtl_step_of(hz) / 2 : 2000;
    time_t  now  = time(NULL);
    hit_t   h;
    int     j;

    for (j = 0; j < nhits; j++)
        if (hits[j].rtl == rtl && llabs(hits[j].hz - hz) <= near) break;
    if (j < nhits) {
        h = hits[j];
        h.times++;
        h.last = now;
        if (peak > h.peak) h.peak = peak;
    } else {
        memset(&h, 0, sizeof(h));
        h.hz    = hz;
        h.mode  = mode;
        h.rtl   = rtl;
        h.first = h.last = now;
        h.times = 1;
        h.peak  = peak;
        snprintf(h.what, sizeof(h.what), "%s", what ? what : "");
        if (nhits < HITS) nhits++;
        j = nhits - 1;                  /* full: the oldest goes */
        if (g_on == j) g_on = -1;
    }
    memmove(&hits[1], &hits[0], (size_t)j * sizeof(hit_t));
    hits[0] = h;
    if (g_sel == j) g_sel = 0;
    else if (g_sel < j) g_sel++;
    if (g_on == j) g_on = 0;
    else if (g_on >= 0 && g_on < j) g_on++;
    hits_dirty = true;
    if (scr == SCR_LOG) show_glog();
}

/* The stick (running for the whole Sentry: rtl_spectrum.h) tuned for G's entry hz */
static void g_stick_on(int64_t hz) {
    if (!g_stick) rtl_spectrum_squelch(param_i_get(cfg_rtl_sql));
    rtl_spectrum_tune(hz + G_STICK_OFF);
    stick_at = hz + G_STICK_OFF;
    g_stick  = true;
}

/* Entry i on the speaker from its own receiver, -1 none: the stick's on the stick, the X6200's on the X6200 */
static void g_listen(int i) {
    if (i < 0 || i >= nhits) {
        if (g_on >= 0 && hits[g_on].rtl) hear_apply(HEAR_X6200);
        g_on = -1;
        show_screen();
        return;
    }
    const hit_t *h = &hits[i];

    if (h->rtl) {
        if (!sdr_here) {
            osd("NO RTL-SDR", "THIS ONE WAS HEARD ON THE STICK: PLUG IT INTO THE HOST PORT.", -1);
            return;
        }
        g_hz = h->hz;
        g_stick_on(g_hz);
        heard = HEAR_RTL;               /* the log plays one radio at a time: the entry's */
        hear_vol_refresh();
        rtl_spectrum_mux(false);
        rtl_spectrum_listen(g_hz, h->mode ? h->mode : rtl_mode_of(g_hz));
    } else {
        if (listen_rtl) hear_apply(HEAR_X6200);      /* the X6200 on the speaker again */
        if (!tune((int32_t)h->hz, h->mode)) return;
    }
    g_on = i;
    show_screen();
}

/* The VFO while G plays the stick: a channel a click, the entry kept as it was */
static void g_turn(int32_t diff) {
    g_hz = rtl_snap(g_hz + diff * rtl_step_of(g_hz));
    if (g_hz < RTL_SPEC_MIN_HZ + G_STICK_OFF) g_hz = RTL_SPEC_MIN_HZ + G_STICK_OFF;
    if (g_hz > RTL_SPEC_MAX_HZ - G_STICK_OFF) g_hz = RTL_SPEC_MAX_HZ - G_STICK_OFF;
    rtl_spectrum_tune(g_hz + G_STICK_OFF);
    rtl_spectrum_listen(g_hz, rtl_mode_of(g_hz));
    show_head();
}

/* G left: its scan stops; what it played goes on until A or F is shown (they set the speaker: hear_apply), as on
 * every screen that is not a spectrum. The stick runs on; F retunes it when shown */
static void g_leave(bool to_f) {
    (void)to_f;
    g_scan  = false;
    g_stick = false;
    g_on    = -1;
}

/* G opened: the stick tuned to its first entry of the stick's, so that one plays without waiting */
static void g_enter(void) {
    if (!sdr_here) return;
    for (int i = 0; i < nhits; i++)
        if (hits[i].rtl) {
            g_stick_on(hits[i].hz);
            return;
        }
}

/* F4's scan: the next one (a stick entry skipped with no stick), held while it is busy: the X6200's over HEARD_DB in
 * the picture, the stick's with its squelch open (with a squelch set; none, it only dwells) */
static bool g_busy(void) {
    if (g_on < 0 || g_on >= nhits) return false;
    if (hits[g_on].rtl) return param_i_get(cfg_rtl_sql) > 0 && rtl_spectrum_squelch_open();
    return nbins && con_at(hits[g_on].hz, 2) >= HEARD_DB;
}

static void g_scan_step(void) {
    uint32_t now = lv_tick_get();

    if (!g_scan || scr != SCR_LOG || !nhits || now < g_scan_next) return;
    if (g_busy()) {
        g_scan_next = now + 1500;       /* someone there: stay, and look again */
        return;
    }
    for (int k = 1; k <= nhits; k++) {
        int i = ((g_on >= 0 ? g_on : g_sel) + k) % nhits;

        if (hits[i].rtl && !sdr_here) continue;
        g_sel = i;
        g_listen(i);
        break;
    }
    g_scan_next = lv_tick_get() + g_dwells[g_dwell] * 1000;
}

static void g_scan_set(bool on) {
    g_scan = on && nhits;
    if (g_scan) {
        if (g_on != g_sel) g_listen(g_sel);
        g_scan_next = lv_tick_get() + g_dwells[g_dwell] * 1000;
    }
    show_screen();
}

static void g_remove(int i) {
    if (i < 0 || i >= nhits) return;
    if (g_on == i) g_listen(-1);
    memmove(&hits[i], &hits[i + 1], (size_t)(nhits - i - 1) * sizeof(hit_t));
    nhits--;
    if (g_on > i) g_on--;
    if (g_sel >= nhits) g_sel = nhits ? nhits - 1 : 0;
    hits_dirty = true;
}

static void show_glog(void) {
    char s[160];
    int  top = g_sel - lg_rows / 2;

    if (top > nhits - lg_rows) top = nhits - lg_rows;
    if (top < 0) top = 0;
    for (int r = 0; r < lg_rows; r++) {
        int i = top + r;

        lv_obj_set_style_bg_opa(lg_row_l[r], i == g_sel && i < nhits ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lg_row_l[r], i == g_sel ? ink : amb, 0);
        if (i >= nhits) {
            lv_label_set_text(lg_row_l[r], "");
            continue;
        }
        const hit_t *h = &hits[i];
        bool         in;
        int          bi = band_near(h->hz, &in);
        char         f[16], now[16] = "", last[8], bn[12];
        struct tm    tm;

        snprintf(f, sizeof(f), "%lld.%03lld.%02lld", (long long)(h->hz / 1000000), (long long)(h->hz / 1000 % 1000),
                 (long long)(h->hz % 1000 / 10));
        if (bi >= 0) band_now(bi, now, sizeof(now));
        localtime_r(&h->last, &tm);
        strftime(last, sizeof(last), "%H:%M", &tm);
        char n[8];
        snprintf(n, sizeof(n), "%d", h->times);
        snprintf(bn, sizeof(bn), "%s%s", in ? "" : "~", bi >= 0 ? bands[bi].name : "-");
        char rxs[24];
        if (h->rtl) snprintf(rxs, sizeof(rxs), sdr_here ? "#%s RTL#" : "RTL", tag_hex);
        else snprintf(rxs, sizeof(rxs), "X6200");
        snprintf(s, sizeof(s), LOG_ROW, i == g_on ? '>' : ' ', f, bn,
                 h->what, bi >= 0 ? best_name(bands[bi].when) : "", bi >= 0 ? bands[bi].ant : "", now, last, n,
                 rxs);
        lv_label_set_text(lg_row_l[r], s);
    }
    if (!nhits) {
        lv_label_set_text(lg_about_l, "NOTHING LOCKED ON YET: A'S SCAN (F1, WITH AUTO) AND B'S (F1) PUT WHAT THEY STOP "
                                      "ON HERE.");
        return;
    }
    const hit_t *h = &hits[g_sel];
    char         a[16], b[16];
    struct tm    tm;

    localtime_r(&h->first, &tm);
    strftime(a, sizeof(a), "%H:%M %b %d", &tm);
    localtime_r(&h->last, &tm);
    strftime(b, sizeof(b), "%H:%M %b %d", &tm);
    snprintf(s, sizeof(s), h->rtl ? "%s: FIRST %s, LAST %s, %d TIME%s, %.0f DB. %s" : "%s: FIRST %s, LAST %s, %d TIME%s, +%.0f DB. %s",
             h->what[0] ? h->what : "THIS ONE", a, b, h->times, h->times == 1 ? "" : "S", h->peak,
             g_scan ? "SCANNING THE LIST (F4: STOP)" : g_sel == g_on ? "HEARD NOW (PUSH: STOP)"
             : h->rtl ? "PUSH: HEAR IT ON THE STICK" : "PUSH: HEAR IT ON THE X6200");
    for (char *c = s; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    lv_label_set_text(lg_about_l, s);
}

static void go(scr_t s) {
    if (scr == SCR_LOG && s != SCR_LOG) g_leave(s == SCR_RTL);
    scr_t was = scr;

    scr = s;
    rtl_want(s == SCR_RTL);     /* C's own state; the stick itself runs for the whole Sentry */
    if ((s == SCR_SCAN || s == SCR_RTL) && was != s) {      /* one picture, A's view or C's */
        rtl_seq  = 0;
        rtl_drop = 2;
        rtl_layout();
    }
    if (s == SCR_SCAN) hear_apply(param_i_get(cfg_hear_a));    /* A and F each put their own LISTEN on the speaker; */
    if (s == SCR_RTL) hear_apply(param_i_get(cfg_hear_f));     /* the others keep what was heard last */
    if (s == SCR_LOG) g_enter();
    audio_scope(s == SCR_WALKIE);       /* G's voiceprint: the receiver's audio only while it shows */
    rtl_spectrum_scope(s == SCR_WALKIE);    /* and the stick's, for its channels */
    if (was != s && (was == SCR_WALKIE || s == SCR_WALKIE)) walkie_show(s == SCR_WALKIE);
    if (g_armed && s != SCR_SCAN && s != SCR_WALKIE) walkie_arm(false);    /* armed only on A and G */
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
    {   /* into G's log */
        const target_t *tg_ = target_near(hz, 2000);

        hit_add(id.tune_hz, id.mode, false, id.name[0] ? id.name : id.label, tg_ ? tg_->con : 0);
    }
    show_screen();              /* SCAN CFG has the lock's details; nothing jumps there */
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
    if (smode == MODE_AUTO && routine == ROUT_SCAN) scan_a_next = lv_tick_get();           /* on along the band */
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

/* SCAN: the next 360 kHz of the band, a little overlapped, from its low end again past the top (as F's scan); a band
 * narrower than the picture stays in its middle */
static void a_scan_hop(uint32_t now) {
    const band_t *b    = &bands[scan_a_band];
    int64_t       lo   = b->lo * 1000LL, hi = b->hi * 1000LL, half = VIEW_HZ / 2, next;

    if (hi - lo <= VIEW_HZ)
        next = (lo + hi) / 2;
    else if (!scan_a_next || anchor < lo + half || anchor + half >= hi)
        next = lo + half;
    else if ((next = anchor + half * 9 / 5) + half > hi)
        next = hi - half;
    scan_a_next = now + A_SCAN_MS;
    if (next == anchor && centre && llabs(centre - next) < 1000) return;
    anchor = next;
    tune((int32_t)next, -1);
    show_screen();
}

/* F1 on A: the band the dial is in, end to end (the Sentry has the dial again: SEMI from MAN-OVERRIDE), or not */
static void a_scan_set(bool on) {
    if (!on) {
        if (routine == ROUT_SCAN) set_routine(ROUT_BAND);
        osd("SCAN OFF", "THE X6200 STAYS ON THE 360 KHZ AT THE DIAL.", -1);
        return;
    }
    int i = band_of(centre ? centre : dial_centre());

    if (i < 0 || !cfg_is_valid_hw_freq(bands[i].listen_hz)) {
        osd("NO BAND HERE", "F2 OR THE BAND KEYS CHOOSE ONE; F1 THEN SCANS IT END TO END.", -1);
        return;
    }
    scan_a_band = i;
    if (smode == MODE_MAN) smode = MODE_SEMI;
    set_routine(ROUT_SCAN);
    char t[24];
    snprintf(t, sizeof(t), "SCAN: %s", bands[i].name);
    osd(t, smode == MODE_AUTO ? "THE BAND END TO END, 360 KHZ AT A TIME, LOCKING ON WHAT IT FINDS (AUTO)."
                              : "THE BAND END TO END, 360 KHZ AT A TIME, MARKING WHAT IT FINDS (SEMI: SCAN CFG F1 "
                                "FOR AUTO).", -1);
}

/* AUTO: lock on the strongest target not ignored nor seen lately, in the routine's place; SWEEP hops on */
static void sentry_step(void) {
    uint32_t now = lv_tick_get();

    if (trk.on) {
        track_step();
        return;
    }
    if (smode == MODE_MAN) return;
    if (scr == SCR_LOG && g_on >= 0) return;      /* the log is playing one: the dial is its meanwhile */
    if (g_armed) return;                          /* armed to transmit: the dial is the operator's */
    if (routine == ROUT_SCAN && scan_a_band >= 0 && (scan_a_next == 0 || now > scan_a_next)) {
        a_scan_hop(now);
        return;
    }
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
        if (routine != ROUT_SWEEP && anchor && llabs(tg[i].hz - anchor) > VIEW_HZ / 2) continue;
        if (routine == ROUT_SCAN && scan_a_band >= 0 &&     /* the band scanned, not what the picture shows past it */
            (tg[i].hz / 1000 < bands[scan_a_band].lo || tg[i].hz / 1000 > bands[scan_a_band].hi))
            continue;
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

/* SCAN CFG's F1 (from A): AUTO and SEMI by turns; MAN-OVERRIDE comes only from turning the VFO, and goes to SEMI.
 * Each said in words as it is chosen */
static void a_mode_next(void) {
    set_mode(smode == MODE_SEMI ? MODE_AUTO : MODE_SEMI);
    if (smode == MODE_AUTO)
        osd("AUTO-SCAN", "LOCKS ON EACH SIGNAL IT FINDS BY ITSELF, LISTENS A WHILE, THEN GOES ON (WITH A'S SCAN, ALONG "
                         "THE BAND).", -1);
    else
        osd("SEMI-AUTO", "MARKS THE SIGNALS IT FINDS AND WAITS: A'S MFK PUSH OR A TAP LOCKS ON ONE.", -1);
}

static void set_routine(routine_t r) {
    routine     = r;
    sweep_next  = 0;
    scan_a_next = 0;
    if (r != ROUT_SWEEP) anchor = centre ? centre : dial_centre();
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
    if (a_old) {
        draw_spectrum();
        lv_obj_invalidate(img);
        show_labels();
    } else if (scr == SCR_TRACK) {
        show_track();
    }
}

static void blink_cb(lv_timer_t *t) {
    static int n;

    (void)t;
    if (hits_dirty && n % 120 == 0) hits_save();     /* G's log on DATA, a minute at most behind */
    g_scan_step();
    if (scr == SCR_LOG && g_clear_ms && lv_tick_get() - g_clear_ms > 3000) {
        g_clear_ms = 0;
        buttons_refresh(&btn[2]);
    }
    if (n++ % 4 == 0) {
        hear_vol_refresh();             /* the X6200's volume may have changed elsewhere (BOTH's shares) */
        bool here = adsb_rx_sdr_present();
        bool ads  = adsb_rx_receiver_present() || adsb_rx_state(NULL, 0) == ADSB_RX_ON;
#ifdef BOARD_X6200
        bool wr   = param_i_get(cfg_wr12) && wr12_ready();      /* wr12.c: the X6200's only */
#else
        bool wr   = false;
#endif

        if (here != sdr_here && scr == SCR_BANDS) {
            sdr_here = here;
            show_bands();
        }
        sdr_here  = here;
        adsb_here = ads;
        wr12_here = wr;
    }
    blink = !blink;
    show_head();
    if (scr == SCR_TRACK) show_track();
    if (scr == SCR_BANDS && (lv_tick_get() / 500) % 20 == 0) show_bands();
}

/* ---- The keys ---- */

static const char *f_label(int k) {
    static char s[24];

    switch (scr) {
        case SCR_SCAN: {        /* C's keys, for the X6200: SCAN its band, the BAND, VIEW, LISTEN, SCAN CFG */
            static const char *const views[] = {"RTL", "Split", "X6200"};
            int bi = routine == ROUT_SCAN ? scan_a_band : band_of(centre ? centre : dial_centre());

            if (k == 0) snprintf(s, sizeof(s), "Scan:\n%s", routine == ROUT_SCAN ? "On" : "Off");
            else if (k == 1) snprintf(s, sizeof(s), "Band:\n%s", bi >= 0 ? bands[bi].name : "-");
            else if (k == 2) snprintf(s, sizeof(s), "View:\n%s", views[spec_view() % 3]);
            else if (k == 3) snprintf(s, sizeof(s), "Listen:\n%s", hear_names[param_i_get(cfg_hear_a)]);
            else snprintf(s, sizeof(s), "Scan Cfg");
            return s;
        }
        case SCR_WALKIE: {
            if (k == 2 || k == 3) walkie_key_label(k, s, sizeof(s));
            else if (k == 4) snprintf(s, sizeof(s), "%s", w_rtl_now() ? "Tx:\nRX only" : g_armed ? "Tx:\nArmed" : "Tx:\nLocked");
            else snprintf(s, sizeof(s), "%s", k ? "Ch +" : "Ch -");
            return s;
        }
        case SCR_TRACK:         /* SCAN CFG: A's settings and its lock, or C's stick */
            if (cfg_from == SCR_SCAN) {
                static const char *const m[] = {"Mode:\nAuto", "Mode:\nSemi", "Mode:\nManual"};

                if (k == 0) return m[smode];
                if (k == 1) snprintf(s, sizeof(s), "Sweep:\n%s", routine == ROUT_SWEEP ? "On" : "Off");
                else if (k == 2) snprintf(s, sizeof(s), "Step:\n%s", a_step_n[a_step]);
                else if (k == 3) snprintf(s, sizeof(s), "%s", recorder_is_on() ? "Rec:\nOn" : "Rec");
                else snprintf(s, sizeof(s), "Back");
            } else {
                static const char *const steps[] = {"12.5K", "25K", "100K", "500K", "1M"};
                int                      g       = param_i_get(cfg_rtl_gain), q = param_i_get(cfg_rtl_sql);

                if (k == 0 && g < 0) snprintf(s, sizeof(s), "Gain:\nAuto");
                else if (k == 0) snprintf(s, sizeof(s), "Gain:\n%d.%d dB", g / 10, g % 10);
                else if (k == 1) snprintf(s, sizeof(s), "Dwell:\n%d s", scan_dwell_ms / 1000);
                else if (k == 2) snprintf(s, sizeof(s), "Step:\n%s", steps[rtl_step]);
                else if (k == 3 && !q) snprintf(s, sizeof(s), "Sql:\nOff");
                else if (k == 3) snprintf(s, sizeof(s), "Sql:\n%d dB", q);
                else snprintf(s, sizeof(s), "Back");
            }
            return s;
        case SCR_BANDS:
            if (k == 2) {
                snprintf(s, sizeof(s), "Sweep:\n%s", band_rt[band_sel].sweep ? "In" : "Out");
                return s;
            } else {
                const char *const l[] = {"Scan", "Listen", "", "", ""};    /* F4, F5: nothing here */
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
            if (k == 1) snprintf(s, sizeof(s), "Preset:\n%s", f_name());
            else if (k == 2) snprintf(s, sizeof(s), "View:\n%s", views[spec_view() % 3]);
            else if (k == 3) snprintf(s, sizeof(s), "Listen:\n%s", hear_names[param_i_get(cfg_hear_f)]);
            else if (k == 4) snprintf(s, sizeof(s), "Scan Cfg");
            else snprintf(s, sizeof(s), "Scan:\n%s", rtl_scan ? "On" : "Off");
            return s;
        }
        case SCR_LOG: {
            if (k == 3) snprintf(s, sizeof(s), "Scan:\n%s", g_scan ? "On" : "Off");
            else if (k == 4) snprintf(s, sizeof(s), "Dwell:\n%d s", g_dwells[g_dwell]);
            else if (k == 2) snprintf(s, sizeof(s), g_clear_ms && lv_tick_get() - g_clear_ms < 3000 ? "Clear:\nSure?" : "Clear");
            else if (k == 1) snprintf(s, sizeof(s), "Remove");
            else snprintf(s, sizeof(s), "Listen:\n%s", g_on >= 0 ? "On" : "Off");
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

static void listen_band_x6200(int i) {
    const band_t *b = &bands[i];

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

/* D's SCAN (F1): the band chosen set on its radio first, then scanned end to end there: the X6200's on A (its F1
 * scan, on this band), the stick's on B (its scan, from the band's low end, heard as LISTEN would have it) */
static void scan_band(void) {
    const band_t *b = &bands[band_sel];

    if (band_uses_rtl(band_sel)) {
        f_band = band_sel;
        rtl_scan_set(false);
        rtl_tune(b->listen_hz);
        listen_hz = rtl_snap(b->listen_hz);
        if (param_i_get(cfg_hear_f) == HEAR_X6200 || param_i_get(cfg_hear_f) == HEAR_OFF) param_i_set(cfg_hear_f, HEAR_RTL);
        go(SCR_RTL);
        rtl_scan_set(true);
        show_screen();
        return;
    }
    if (!cfg_is_valid_hw_freq(b->listen_hz)) {
        msg_update_text_fmt("The radio can't tune %s", b->name);
        return;
    }
    listen_band_x6200(band_sel);        /* tuned there, A shown */
    scan_a_band = band_sel;             /* this band, not the one the old picture still shows */
    if (smode == MODE_MAN) smode = MODE_SEMI;
    set_routine(ROUT_SCAN);
    {
        char t[24];

        snprintf(t, sizeof(t), "SCAN: %s", b->name);
        osd(t, "THE BAND END TO END, 360 KHZ AT A TIME, LOCKING ON WHAT IT FINDS (AUTO).", -1);
    }
}

/* C's LISTEN (F2 or a press): F for a band the stick takes, its listening frequency on the speaker, the scan off */
static void listen_band(void) {
    const band_t *b = &bands[band_sel];

    if (!band_uses_rtl(band_sel)) {
        listen_band_x6200(band_sel);
        return;
    }
    f_band = band_sel;
    rtl_scan_set(false);
    rtl_tune(b->listen_hz);
    listen_hz = rtl_snap(b->listen_hz);
    if (param_i_get(cfg_hear_f) == HEAR_X6200) param_i_set(cfg_hear_f, HEAR_RTL);     /* LISTEN: to hear it */
    if (scr == SCR_RTL) hear_apply(param_i_get(cfg_hear_f));
    go(SCR_RTL);
}

static void fkey(int k);

static void key_tap_cb(lv_event_t *e) {
    fkey((int)(intptr_t)lv_event_get_user_data(e));
}

static void fkey(int k) {
    switch (scr) {
        case SCR_SCAN:
            if (k == 0) a_scan_set(routine != ROUT_SCAN);
            if (k == 1) band_key(true);
            if (k == 2) {
                rtl_fkey(3);            /* VIEW: A's own */
                show_screen();
            }
            if (k == 3) {
                hear_next();
                show_screen();
            }
            if (k == 4) {
                cfg_from = SCR_SCAN;
                go(SCR_TRACK);
            }
            break;
        case SCR_WALKIE:
            if (k == 0) walkie_ch(-1, 0);
            if (k == 1) walkie_ch(1, 0);
            if (k == 2) walkie_group();
            if (k == 3) walkie_mode_next();
            if (k == 4) walkie_tx_note();
            break;
        case SCR_TRACK:
            if (k == 4) {
                go(cfg_from);           /* BACK */
                break;
            }
            if (cfg_from == SCR_SCAN) {
                if (k == 0) a_mode_next();
                if (k == 1) {
                    set_routine(routine == ROUT_SWEEP ? ROUT_BAND : ROUT_SWEEP);
                    osd(routine == ROUT_SWEEP ? "SWEEP ON" : "SWEEP OFF",
                        routine == ROUT_SWEEP ? "THE X6200 HOPS THROUGH THE BANDS MARKED * IN D BANDS (F3 THERE), A FEW "
                                                "SECONDS EACH, LOCKING ON WHAT IT FINDS WITH AUTO."
                                              : "THE X6200 STAYS ON THE 360 KHZ AT THE DIAL (A'S F1 SCANS ITS BAND).", -1);
                }
                if (k == 2) a_step = (a_step + 1) % (int)N_OF(a_steps);
                if (k == 3) {
                    recorder_set_on(!recorder_is_on());
                    rec_ours = recorder_is_on();
                }
            } else {
                static const int sqls[] = {0, 6, 10, 15, 20};

                if (k == 0) rtl_fkey(5);        /* the gain, the next one up */
                if (k == 1) scan_dwell_ms = scan_dwell_ms >= 4000 ? 1000 : scan_dwell_ms * 2;
                if (k == 2) rtl_fkey(2);        /* the step */
                if (k == 3) {
                    int q = param_i_get(cfg_rtl_sql), i = 0;

                    while (i < (int)N_OF(sqls) && sqls[i] <= q) i++;
                    q = sqls[i % (int)N_OF(sqls)];
                    param_i_set(cfg_rtl_sql, q);
                    rtl_spectrum_squelch(q);
                }
            }
            if (scr == SCR_TRACK) show_screen();
            break;
        case SCR_BANDS:
            if (k == 0) scan_band();
            if (k == 1) listen_band();
            if (k == 2) {
                if (!cfg_is_valid_hw_freq(bands[band_sel].listen_hz)) {
                    osd("THE STICK'S BAND", "THE X6200'S SWEEP CANNOT TUNE IT. F2 HERE OPENS IT ON B VHF/UHF, WHERE F1 "
                        "SCANS IT END TO END.", -1);
                    break;
                }
                band_rt[band_sel].sweep = !band_rt[band_sel].sweep;
                show_screen();
            }
            break;                      /* F4, F5: nothing */
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
            if (k == 0) rtl_scan_set(!rtl_scan);     /* the preset's band swept here, not A's scan */
            if (k == 1) rtl_fkey(1);                 /* PRESET */
            if (k == 2) rtl_fkey(3);                 /* VIEW */
            if (k == 3) rtl_fkey(4);                 /* LISTEN */
            if (k == 4) {
                cfg_from = SCR_RTL;
                go(SCR_TRACK);
                break;
            }
            if (scr == SCR_RTL) show_screen();
            break;
        case SCR_LOG:
            if (k == 0) {
                g_scan = false;
                g_listen(g_on >= 0 ? -1 : g_sel);
            }
            if (k == 1) {
                g_remove(g_sel);
                show_screen();
            }
            if (k == 2) {
                if (g_clear_ms && lv_tick_get() - g_clear_ms < 3000) {
                    g_listen(-1);
                    nhits = g_sel = 0;
                    hits_dirty = true;
                    g_clear_ms = 0;
                } else {
                    g_clear_ms = lv_tick_get();
                    osd("CLEAR THE LOG?", "F3 AGAIN WITHIN 3 SECONDS CLEARS ALL OF IT.", -1);
                }
                show_screen();
            }
            if (k == 3) g_scan_set(!g_scan);
            if (k == 4) {
                g_dwell = (g_dwell + 1) % (int)N_OF(g_dwells);
                show_screen();
            }
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
        case SCR_SCAN: {                /* as on C: it pans, by SCAN CFG's step; a scan stops there */
            int32_t st = a_steps[a_step], fq = cparam_i_get(cfg_fg_freq);

            if (routine == ROUT_SCAN) set_routine(ROUT_BAND);
            tune(fq / st * st + dir * st, -1);
            show_screen();
            break;
        }
        case SCR_TRACK:
            if (cfg_from == SCR_SCAN && trk.on) {
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
        case SCR_WALKIE: walkie_ch(dir, 0); break;
        case SCR_LOG:                   /* the next one; while one plays, it plays the next one; a scan stops */
            if (!nhits) break;
            g_scan = false;
            g_sel = (g_sel + dir + nhits) % nhits;
            if (g_on >= 0) g_listen(g_sel);
            else show_screen();
            break;
        case SCR_RTL:
            rtl_scan_set(false);        /* panned by hand: the scan stops where it was */
            rtl_tune(rtl_hz / rtl_steps[rtl_step] * rtl_steps[rtl_step] + (int64_t)dir * rtl_steps[rtl_step]);
            break;
        default:
            break;
    }
}

static void press(void) {
    switch (scr) {
        case SCR_SCAN: {                /* the strongest signal in view, locked on (as C's push goes to its strongest) */
            int b = -1;

            for (int i = 0; i < ntg; i++)
                if (!tg[i].ignored && llabs(tg[i].hz - centre) < VIEW_HZ / 2 && (b < 0 || tg[i].con > tg[b].con)) b = i;
            if (b >= 0 && tg[b].con >= 8) {
                if (routine == ROUT_SCAN) set_routine(ROUT_BAND);
                track_start(tg[b].hz, false);
            } else {
                osd("ONLY NOISE", "NO SIGNAL IN THE PICTURE TO LOCK ON.", -1);
            }
            break;
        }
        case SCR_TRACK:
            if (cfg_from == SCR_SCAN && trk.on) trk.held = !trk.held;
            show_screen();
            break;
        case SCR_BANDS: listen_band(); break;
        case SCR_DECODE: open_decoder(); break;
        case SCR_SYSTEM: sys_open(sys_sel); break;
        case SCR_RTL:                   /* the strongest signal in view: C's channel to it */
            if (rtl_best) {
                rtl_scan_set(false);
                listen_hz = rtl_snap(rtl_best);
                if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
                show_head();
                show_rtl();
            } else {
                osd("ONLY NOISE", "NO SIGNAL IN THE PICTURE TO GO TO.", -1);
            }
            break;
        case SCR_LOG:
            g_scan = false;
            g_listen(g_on == g_sel ? -1 : g_sel);
            break;
        case SCR_WALKIE: walkie_ch(0, 19); break;
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
        case KEY_MFK_NEXT: {
            scr_t at = scr == SCR_TRACK ? cfg_from : scr;
            int   i  = 0;

            while (i < RING_N && ring[i] != at) i++;
            i = (i % RING_N + (key == KEY_MFK_NEXT ? 1 : RING_N)) % (RING_N + 1);
            if (i == RING_N) main_screen_start_app(ACTION_APP_ADSB);   /* H: AIRSPACE, in the Sentry's place */
            else go(ring[i]);
            return;
        }
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
        if (scr == SCR_LOG) g_turn(diff);
        else if (scr == SCR_WALKIE) walkie_ch(diff > 0 ? 1 : -1, 0);
        else rtl_listen_turn(diff);
        return;
    }
    if (smode != MODE_MAN) {
        smode = MODE_MAN;
        if (routine == ROUT_SCAN) routine = ROUT_BAND;      /* tuned by hand: the scan stops there, as F's */
        if (trk.on) trk.held = true;
        show_screen();
        osd("MAN-OVERRIDE", "YOU TURNED THE VFO: THE SENTRY STOPS MOVING THE X6200'S DIAL AND ONLY MARKS. SCAN CFG F1: "
                            "BACK TO SEMI OR AUTO.", -1);
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
    if (scr == SCR_RTL) {               /* F: its presets, not the X6200's bands (and never A) */
        f_band     = -1;
        rtl_preset = (rtl_preset + (up ? 1 : RTL_PRESETS - 1)) % RTL_PRESETS;
        rtl_tune(rtl_presets[rtl_preset].hz);
        scan_next = 0;
        osd(rtl_presets[rtl_preset].name, rtl_presets[rtl_preset].about, -1);
        show_screen();
        return;
    }
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
    bool scanning = routine == ROUT_SCAN;

    band_sel = i;
    listen_band_x6200(i);
    if (scanning) {                     /* F2 or a BAND key while scanning: the new band, from its low end */
        scan_a_band = i;
        routine     = ROUT_SCAN;
        scan_a_next = 0;
        show_screen();
    }
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
            if (tap && !g_portrait) help_show(true);    /* drawn on the landscape screen: none held up */
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
            if (scr == SCR_WALKIE || scr == SCR_SCAN) {     /* A and G, the X6200's: the transmitter armed or locked */
                if (tap) walkie_arm(!g_armed);
                return true;
            }
            if (tap || hold)
                osd("A/B", scr == SCR_RTL ? "THE RTL-SDR ONLY RECEIVES: A/B ARMS THE X6200'S TRANSMITTER ON A AND G."
                                          : "ON A AND G, A/B ARMS THE X6200'S TRANSMITTER FOR PTT.", -1);
            return true;
        case KEYPAD_KEY:
        case KEYPAD_MSG:
            if (tap || hold) {
                snprintf(t, sizeof(t), "%s", key == KEYPAD_KEY ? "KEY: CW KEYER" : "MSG: MESSAGES");
                osd(t, "THEY TRANSMIT: LOCKED OUT IN THE SENTRY. F SYSTEM, CLASSIC RADIO LEAVES IT.", -1);
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
        case KEYPAD_PTT:
            if (scr == SCR_WALKIE && !g_armed && state != KEYPAD_RELEASE) walkie_ptt();
            return false;       /* refused (radio_rx_only), unless G armed it */
        default:
            return false;       /* POWER, F1-F5: as ever */
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

    if (scr == SCR_TRACK && cfg_from == SCR_RTL && box <= 4) return;     /* the stick's view: shown, not chosen */
    switch (box) {
        case 0: set_mode((sysmode_t)i); break;                     /* SYSTEM MODE */
        case 1:                                                    /* SCAN ROUTINE: SCAN or SWEEP, on or off */
            if (i == 0) a_scan_set(routine != ROUT_SCAN);
            if (i == 1) set_routine(routine == ROUT_SWEEP ? ROUT_BAND : ROUT_SWEEP);
            break;
        case 4: break;                                             /* SIGNAL STATUS: shown, no more */
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
    } else if (scr == SCR_LOG) {
        int top = g_sel - lg_rows / 2;
        if (top > nhits - lg_rows) top = nhits - lg_rows;
        if (top < 0) top = 0;
        int i = top + tag;
        if (i >= nhits) return;
        if (i == g_sel)
            g_listen(g_on == i ? -1 : i);
        else {
            g_sel = i;
            if (g_on >= 0) g_listen(i);
            else show_screen();
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
    lv_label_set_recolor(status_l, true);       /* the devices in blue */
    batt_l = lv_label_create(root);             /* the charge's icon, after the battery's % (show_head) */
    lv_obj_set_style_text_font(batt_l, &power_sym_14, 0);
    lv_obj_set_style_text_color(batt_l, amb, 0);
    lv_label_set_text(batt_l, "");
}

static void build_scan(lv_obj_t *p) {
    int iy = BODY_Y + BOX_H + 6;        /* the first A's own picture: kept built, shown no more (a_old) */

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

}

/* SCAN CFG: A's boxes as A had them (or the stick's, opened from C), and the lock under them */
static void build_track(lv_obj_t *p) {
    int w = (SW - 12 - 18) / 4;

    box_make(&b_mode, p, 6, BODY_Y, w, BOX_H, "SYSTEM MODE", 3, 0);
    box_make(&b_tx, p, 6 + (w + 6), BODY_Y, w, BOX_H, "TRANSMITTER", 3, -1);
    box_make(&b_sig, p, 6 + 2 * (w + 6), BODY_Y, w, BOX_H, "SIGNAL STATUS", 3, 4);
    box_make(&b_rout, p, 6 + 3 * (w + 6), BODY_Y, w, BOX_H, "SCAN ROUTINE", 3, 1);

    int ly = BODY_Y + BOX_H + 6, w1 = 300, w2 = 200, w3 = SW - 12 - w1 - w2 - 12;

    box_make(&b_band, p, 6, ly, w1, LOW_H, "TARGET BAND", 2, -1);
    box_make(&b_prof, p, 6 + w1 + 6, ly, w2, LOW_H, "SIGNAL PROFILE", 2, -1);
    box_make(&b_tgts, p, 6 + w1 + w2 + 12, ly, w3, LOW_H, "TARGETS", 2, -1);

    int y0 = ly + LOW_H + 6, h = scr_h - y0 - 6;

    t_state_l    = label(p, &pixel_24, amb, 6, y0 + 2, 230, LV_TEXT_ALIGN_CENTER, "");
    t_crit_frame = rect(p, 12, y0 + 56, 218, 40, 3, false);
    t_crit_l     = label(p, &pixel_20, amb, 18, y0 + 60, 206, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_height(t_crit_l, 32);
    lv_obj_set_style_pad_top(t_crit_l, 6, 0);
    lv_obj_set_style_bg_color(t_crit_l, amb, 0);
    t_time_cap_l = label(p, &pixel_16, amb, 6, y0 + 104, 230, LV_TEXT_ALIGN_CENTER, "TIME ON TARGET (SECS)");
    t_time_l     = label(p, &pixel_32, amb, 6, y0 + 124, 230, LV_TEXT_ALIGN_CENTER, "");

    int mx = 244, mw = 390;
    rect(p, mx, y0, mw, 72, 3, false);
    t_big_l = label(p, &pixel_32, amb, mx + 3, y0 + 6, mw - 6, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_add_flag(t_big_l, LV_OBJ_FLAG_CLICKABLE);                 /* a tap: IGNORE it */
    lv_obj_add_event_cb(t_big_l, ignore_tap_cb, LV_EVENT_CLICKED, NULL);
    t_sub_l = label(p, &pixel_20, amb, mx + 3, y0 + 44, mw - 6, LV_TEXT_ALIGN_CENTER, "");
    t_id_l  = label(p, &pixel_16, amb, mx, y0 + 78, mw, LV_TEXT_ALIGN_CENTER, "");
    lv_label_set_long_mode(t_id_l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(t_id_l, 18);
    t_db_l  = label(p, &pixel_16, dim, mx, y0 + 98, mw, LV_TEXT_ALIGN_CENTER, "");
    t_log_l = label(p, &pixel_16, amb, mx, y0 + 118, mw, LV_TEXT_ALIGN_LEFT, "");

    int bx = 650;
    label(p, &pixel_16, amb, bx, y0, 64, LV_TEXT_ALIGN_CENTER, "SIG");
    label(p, &pixel_16, amb, bx + 74, y0, 70, LV_TEXT_ALIGN_CENTER, "NOISE");
    t_bar_y = y0 + 20;
    t_bar_h = h - 22;
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

    snprintf(h, sizeof(h), BAND_ROW, ' ', "BAND", "MHZ", "WHAT YOU'LL HEAR", "BEST", "ANTENNA", "NOW", "RX");
    bd_head_l = label(p, &pixel_16, amb, 9, y + 2, 0, 0, h);
    rect(p, 6, y + 20, SW - 12, 3, 0, true);
    bd_rows = (scr_h - (y + 26) - 50) / rh;
    if (bd_rows > 20) bd_rows = 20;
    for (int r = 0; r < bd_rows; r++) {
        bd_row_l[r] = label(p, &pixel_16, amb, 9, y + 26 + r * rh, SW - 18, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(bd_row_l[r], true);        /* RX: RTL in the stick's blue */
        lv_obj_set_height(bd_row_l[r], rh);
        lv_label_set_long_mode(bd_row_l[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(bd_row_l[r], amb, 0);
        lv_obj_add_flag(bd_row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(bd_row_l[r], row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    }
    rect(p, 6, scr_h - 50, SW - 12, 3, 0, true);
    bd_about_l = label(p, &pixel_16, amb, 14, scr_h - 44, SW - 28, LV_TEXT_ALIGN_LEFT, "");
}

static void build_log(lv_obj_t *p) {
    int y = BODY_Y + 2, rh = 18;

    rect(p, 6, y - 2, SW - 12, scr_h - y - 2, 3, false);
    char h[128];

    snprintf(h, sizeof(h), LOG_ROW, ' ', "FREQ", "BAND", "WHAT IT WAS", "BEST", "ANTENNA", "NOW", "LAST", "SEEN", "RX");
    lg_head_l = label(p, &pixel_16, amb, 9, y + 2, 0, 0, h);
    rect(p, 6, y + 20, SW - 12, 3, 0, true);
    lg_rows = (scr_h - (y + 26) - 50) / rh;
    if (lg_rows > 20) lg_rows = 20;
    for (int r = 0; r < lg_rows; r++) {
        lg_row_l[r] = label(p, &pixel_16, amb, 9, y + 26 + r * rh, SW - 18, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(lg_row_l[r], true);        /* RX: RTL in the stick's blue */
        lv_obj_set_height(lg_row_l[r], rh);
        lv_label_set_long_mode(lg_row_l[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(lg_row_l[r], amb, 0);
        lv_obj_add_flag(lg_row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(lg_row_l[r], row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
    }
    rect(p, 6, scr_h - 50, SW - 12, 3, 0, true);
    lg_about_l = label(p, &pixel_16, amb, 14, scr_h - 44, SW - 28, LV_TEXT_ALIGN_LEFT, "");
}

static void build_decode(lv_obj_t *p) {
    int y = BODY_Y, h = scr_h - y - DLOW_H - 10, lw = 196;

    box_make(&d_sel, p, 6, y, lw, h, "TYPE SELECT", DECODERS_N < 8 ? DECODERS_N : 8, 3);
    rect(p, lw + 12, y, SW - lw - 18, h, 3, false);
    char hd[96];

    snprintf(hd, sizeof(hd), DEC_ROW, "KNOWN FREQ", "BAND", "BEST", "ANTENNA", "NOW", "RX");
    d_head_l = label(p, &pixel_16, amb, lw + 15, y + 5, 0, 0, hd);
    rect(p, lw + 12, y + 25, SW - lw - 18, 3, 0, true);
    for (int r = 0; r < 12; r++) {
        d_row_l[r] = label(p, &pixel_16, amb, lw + 15, y + 30 + r * 18, SW - lw - 24, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(d_row_l[r], true);         /* RX: RTL in the tags' orange */
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

/* ---- G WALKIE: the radio as a walkie-talkie on CB, its voice drawn as it is heard ----
 *
 * Held up as a walkie-talkie: in portrait (main_display_portrait, as the Walkie app goes, the panel 480x800), a
 * screen of its own with the F keys' names down the left edge where the keys are, the channel, a lamp, the voiceprint,
 * the meters and the radio's state under each other. A build without portrait (fbdev, a PC) has it in landscape in
 * the Sentry's own look. The knobs, keys and timers are the Sentry's either way */

#define VP_FFT  1024
#define VP_HOP  512
#define VP_HZ   4000            /* the voiceprint: 0-4 kHz up, the last seconds across */
#define VP_MAXH 300

typedef struct {
    box_t         ch, link, pow, sys;                 /* landscape: four boxes */
    lv_obj_t     *freq_l, *info_l, *msg_l;            /* portrait: the frequency, the state in lines, a word */
    lv_obj_t     *via_l;                              /* the radio serving the channel: VIA X6200, VIA RTL-SDR */
    lv_obj_t     *num_l, *band_l, *lamp, *lamp_l, *vp_img, *s_fill, *a_fill, *s_l, *a_l, *cap_l;
    lv_img_dsc_t *vp_frame;
    int           vp_w, vp_h, bar_w;
} wui_t;

static wui_t      w_land, w_port, *wui = &w_land;
static lv_obj_t  *g_scr, *g_prev_scr;           /* the portrait screen, and the one it took the place of */
static uint32_t   vp_pal[256];
static int16_t    vp_in[VP_FFT];
static int        vp_have;
static float      vp_floor = 20, vp_rms;
static uint32_t   ptt_ms;

/* In place, radix 2 */
static void vp_fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a  = -2 * M_PI / len;
        float  wr = (float)cos(a), wi = (float)sin(a);

        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;

            for (int k = 0; k < len / 2; k++) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;

                re[i + k] = ur + vr, im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr, im[i + k + len / 2] = ui - vi;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr, cr = nr;
            }
        }
    }
}

/* One column of the voiceprint from the last VP_FFT samples, shifted in at the right */
static void vp_column(void) {
    static float re[VP_FFT], im[VP_FFT];
    float        col[VP_MAXH], sorted[VP_MAXH], bin_hz = 44100.0f / VP_FFT;
    int          w = wui->vp_w, h = wui->vp_h;
    uint32_t    *px = (uint32_t *)wui->vp_frame->data;

    for (int i = 0; i < VP_FFT; i++) {
        float win = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (VP_FFT - 1));
        re[i] = vp_in[i] * win, im[i] = 0;
    }
    vp_fft(re, im, VP_FFT);
    for (int r = 0; r < h; r++) {
        float f  = (r + 0.5f) * VP_HZ / h, b = f / bin_hz;
        int   b0 = (int)b;
        float t  = b - b0;
        float m0 = re[b0] * re[b0] + im[b0] * im[b0], m1 = re[b0 + 1] * re[b0 + 1] + im[b0 + 1] * im[b0 + 1];

        col[r] = 10 * log10f(m0 * (1 - t) + m1 * t + 1);
    }
    memcpy(sorted, col, (size_t)h * sizeof(float));
    qsort(sorted, h, sizeof(float), cmp_f);
    vp_floor = 0.98f * vp_floor + 0.02f * sorted[h / 3];       /* the hiss, followed slowly */
    for (int y = 0; y < h; y++) {
        uint32_t *row = px + y * w;
        float     v   = (col[h - 1 - y] - vp_floor - 2) / 30;
        int       i   = v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255);

        memmove(row, row + 1, (size_t)(w - 1) * sizeof(uint32_t));
        row[w - 1] = vp_pal[i];
    }
}

/* G's 50 ms: the voiceprint's new columns, the bars, the lamp */
static void walkie_step(void) {
    int16_t buf[2048];
    size_t  n, got = 0;
    int     cols = 0;
    double  e    = 0;

    if (!wui->vp_frame) return;
    bool from_rtl = w_rtl_now() && heard == HEAR_RTL;

    while ((n = from_rtl ? rtl_spectrum_scope_read(buf, N_OF(buf)) : audio_scope_read(buf, N_OF(buf))) > 0) {
        for (size_t i = 0; i < n; i++) {
            e += (double)buf[i] * buf[i];
            vp_in[vp_have++] = buf[i];
            if (vp_have == VP_FFT) {
                if (cols < 16) vp_column();     /* behind: the oldest skipped, not the screen held up */
                cols++;
                memmove(vp_in, vp_in + VP_HOP, (VP_FFT - VP_HOP) * sizeof(int16_t));
                vp_have = VP_FFT - VP_HOP;
            }
        }
        got += n;
    }
    if (cols) lv_obj_invalidate(wui->vp_img);
    if (got) vp_rms = 0.7f * vp_rms + 0.3f * (float)sqrt(e / got);

    /* the S-meter: S0 -127 dBm to S9+40 -33 dBm; the audio: -60 to 0 dBFS */
    float db = meter_get_db(), s = (db + 127) / 94, a = vp_rms > 1 ? (20 * log10f(vp_rms / 32768) + 60) / 60 : 0;
    char  t[24];

    if (from_rtl) {                     /* the stick's: its channel over the noise, as its squelch measures it */
        float o = rtl_spectrum_level();

        s  = o < 0 ? 0 : o / 40;
        db = o < 0 ? -127 : -121 + o;
    }

    s = s < 0 ? 0 : s > 1 ? 1 : s;
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    lv_obj_set_width(wui->s_fill, (int)(s * wui->bar_w) + 1);
    lv_obj_set_width(wui->a_fill, (int)(a * wui->bar_w) + 1);
    if (from_rtl) snprintf(t, sizeof(t), "RTL +%.0f DB", rtl_spectrum_level() < 0 ? 0 : rtl_spectrum_level());
    else if (db >= -73) snprintf(t, sizeof(t), "S9+%d  %.0f DBM", (int)(db + 73), db);
    else snprintf(t, sizeof(t), "S%d  %.0f DBM", db <= -127 ? 0 : (int)((db + 127) / 6), db);
    lv_label_set_text(wui->s_l, t);
    snprintf(t, sizeof(t), "%d DBFS", vp_rms > 1 ? (int)(20 * log10f(vp_rms / 32768)) : -90);
    lv_label_set_text(wui->a_l, t);
    /* the lamp: a voice (the audio up) or a carrier (the meter up); PTT refused, said in it */
    /* a signal: the audio 6 dB over its own floor (followed slowly: the hiss, the band's noise), or the stick's
     * squelch open where one is set */
    static float floor_rms = 1000;
    if (got) floor_rms = vp_rms < floor_rms ? 0.9f * floor_rms + 0.1f * vp_rms : 0.998f * floor_rms + 0.002f * vp_rms;
    bool on = from_rtl && param_i_get(cfg_rtl_sql) > 0 ? rtl_spectrum_squelch_open() : vp_rms > 2 * floor_rms + 100;
    bool ptt = ptt_ms && lv_tick_get() - ptt_ms < 1500;
    bool air = radio_get_state() == RADIO_TX;

    lv_obj_set_style_bg_opa(wui->lamp, air || ptt || on || (g_armed && blink) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(wui->lamp_l, air || ptt || on || (g_armed && blink) ? ink : amb, 0);
    lv_label_set_text(wui->lamp_l, air ? (g_portrait ? "TX  ON AIR" : "TX\nON AIR")
                                   : ptt ? (g_portrait ? "TX LOCKED" : "TX\nLOCKED")
                                   : g_armed ? (g_portrait ? "TX  ARMED" : "TX\nARMED")
                                   : on ? (g_portrait ? "RX  SIGNAL" : "RX\nSIGNAL")
                                        : (g_portrait ? "RX  QUIET" : "RX\nQUIET"));
    lv_label_set_text(wui->cap_l, air ? "VOICEPRINT  TX  0-4 KHZ" : from_rtl ? "VOICEPRINT  RTL  0-4 KHZ"
                                                                           : "VOICEPRINT  RX  0-4 KHZ");
    if (g_portrait && wui->msg_l && ptt_ms && lv_tick_get() - ptt_ms > 4000) lv_label_set_text(wui->msg_l, "");
}

/* F5 on G: the transmitter's lock, said where it is looked at */
static void walkie_tx_note(void) {
    if (g_armed) {
        walkie_arm(false);              /* F5 locks it again */
        return;
    }
    if (g_portrait) lv_label_set_text(wui->msg_l, "TX LOCKED. A/B ARMS IT FOR PTT");
    else osd("TRANSMIT LOCKED", "THE SENTRY IS RECEIVE ONLY. ON G, THE A/B KEY ARMS THE TRANSMITTER FOR PTT.", -1);
}

/* The A/B (VFO) key on G: the transmitter armed for PTT (the Sentry's receive-only hold let go: the radio's own TX
 * rules again, its power and band limits), or locked again. A/B again, F5 or leaving G locks it */
static void walkie_arm(bool on) {
    char t[64];

    if (on == g_armed) return;
    if (on && w_rtl_now()) {            /* G on a stick channel: nothing to transmit with */
        if (g_portrait) lv_label_set_text(wui->msg_l, "THE RTL-SDR ONLY RECEIVES: NO TX HERE");
        else osd("RECEIVE ONLY", "THIS CHANNEL IS THE RTL-SDR'S: IT CANNOT TRANSMIT.", -1);
        return;
    }
    if (on) {
        if (rx_held) radio_rx_only(false);
        rx_held = false;
        g_armed = true;
        if (routine == ROUT_SCAN || routine == ROUT_SWEEP) set_routine(ROUT_BAND);     /* the dial stays put */
        snprintf(t, sizeof(t), "TX ARMED: PTT TRANSMITS (%.0f W)", param_f_get(cfg_pwr));
    } else {
        rx_held = radio_rx_only(true);
        g_armed = false;
        snprintf(t, sizeof(t), "TX LOCKED AGAIN");
    }
    if (g_portrait) lv_label_set_text(wui->msg_l, t);
    else osd(on ? "TX ARMED" : "TX LOCKED",
             on ? "PTT TRANSMITS ON THE X6200'S DIAL; THE SENTRY'S SCANS AND AUTO WAIT. A/B AGAIN, OR A SCREEN BUT A AND "
                  "G, LOCKS IT."
                : "THE SENTRY IS RECEIVE ONLY AGAIN.", -1);
    for (int i = 0; i < 5; i++) buttons_refresh(&btn[i]);
    show_walkie();
}

/* PTT pressed on G: refused (the Sentry is receive only), and said so where it is looked at */
static void walkie_ptt(void) {
    ptt_ms = lv_tick_get() | 1;
    if (g_portrait) lv_label_set_text(wui->msg_l, "PTT REFUSED: THE SENTRY IS RECEIVE ONLY");
    else osd("PTT REFUSED", "THE SENTRY IS RECEIVE ONLY: NOTHING IS TRANSMITTED FROM HERE.", -1);
}

/* ---- G's channels: a plan across both radios, each served by the one that takes it ----
 *
 * The groups in order: CB 1-40 (AM), 10 m and 6 m FM calling (the X6200's: it can transmit there, armed with A/B),
 * then MURS, 2 m simplex, marine VHF, NOAA weather and FRS/GMRS (the stick's: receive only). CH- and CH+ go through
 * the whole list, the receiver changing by itself; GROUP goes to the next group's home channel. The modes are a table:
 * another (DMR, say) is a row with what each radio does for it */

typedef struct {
    const char *grp, *name;     /* the group and the channel's name in it ("19", "WX1", "22A") */
    int32_t     hz;
    uint8_t     mode;           /* w_modes[]: the channel's own */
    bool        home;           /* where GROUP lands */
} wch_t;

static const struct {
    const char *name;
    int         x62;            /* the X6200's radio_mode_t, -2 SSB (USB from 10 MHz up, LSB under), -1 none */
    int         rtl;            /* the stick's demodulator (1 NFM, 2 AM, 3 WFM), 0 none */
} w_modes[] = {
    {"AM", radio_mode_am, 2},
    {"FM", radio_mode_nfm, 1},
    {"SSB", -2, 0},
};
#define W_MODES (int)(sizeof(w_modes) / sizeof(w_modes[0]))
enum { WM_AM, WM_FM, WM_SSB };

static wch_t w_chs[100];
static int   w_n, w_mode = WM_AM;

static void w_add(const char *grp, const char *name, int32_t hz, int mode, bool home) {
    if (w_n < (int)N_OF(w_chs)) w_chs[w_n++] = (wch_t){grp, name, hz, (uint8_t)mode, home};
}

static void w_plan(void) {
    static const char *const n40[] = {"01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13",
                                      "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26",
                                      "27", "28", "29", "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
                                      "40"};
    static const int32_t frs[] = {462562500, 462587500, 462612500, 462637500, 462662500, 462687500, 462712500,
                                  467562500, 467587500, 467612500, 467637500, 467662500, 467687500, 467712500,
                                  462550000, 462575000, 462600000, 462625000, 462650000, 462675000, 462700000,
                                  462725000};
    static const int32_t wx[] = {162550000, 162400000, 162475000, 162425000, 162450000, 162500000, 162525000};
    static const char *const wxn[] = {"WX1", "WX2", "WX3", "WX4", "WX5", "WX6", "WX7"};

    if (w_n) return;
    for (int i = 0; i < 40; i++) w_add("CB", n40[i], cb_khz[i] * 1000, WM_AM, i == 18);
    w_add("10 M FM", "S", 29600000, WM_FM, true);
    w_add("6 M FM", "S", 52525000, WM_FM, true);
    w_add("MURS", "1", 151820000, WM_FM, true);
    w_add("MURS", "2", 151880000, WM_FM, false);
    w_add("MURS", "3", 151940000, WM_FM, false);
    w_add("MURS", "4", 154570000, WM_FM, false);
    w_add("MURS", "5", 154600000, WM_FM, false);
    w_add("2 M", "S1", 146520000, WM_FM, true);
    w_add("2 M", "S2", 146550000, WM_FM, false);
    w_add("2 M", "S3", 146580000, WM_FM, false);
    w_add("MARINE", "16", 156800000, WM_FM, true);
    w_add("MARINE", "09", 156450000, WM_FM, false);
    w_add("MARINE", "13", 156650000, WM_FM, false);
    w_add("MARINE", "22A", 157100000, WM_FM, false);
    for (int i = 0; i < 7; i++) w_add("NOAA WX", wxn[i], wx[i], WM_FM, i == 0);
    for (int i = 0; i < 22; i++) w_add("FRS/GMRS", n40[i], frs[i], WM_FM, i == 0);
}

/* The stick serves a channel the X6200 cannot tune, and the VHF/UHF ones when it is there */
static bool w_is_rtl(int i) {
    return i >= 0 && i < w_n && (!cfg_is_valid_hw_freq(w_chs[i].hz) || w_chs[i].hz >= 60000000);
}

static bool w_rtl_now(void) {
    return scr == SCR_WALKIE && w_is_rtl(w_at);
}

static bool w_mode_ok(int i, int m) {
    return w_is_rtl(i) ? w_modes[m].rtl > 0 : w_modes[m].x62 != -1;
}

/* Channel i heard on its own radio, in G's mode (the channel's own if the mode is not the radio's) */
static void walkie_go(int i) {
    const wch_t *c = &w_chs[i];

    if (!w_mode_ok(i, w_mode)) w_mode = c->mode;
    if (w_is_rtl(i)) {
        if (!sdr_here && rtl_spectrum_state() != RTL_SPEC_ON) {
            if (g_portrait) lv_label_set_text(wui->msg_l, "NO RTL-SDR: THIS CHANNEL IS THE STICK'S");
            else osd("NO RTL-SDR", "THIS CHANNEL IS THE STICK'S: PLUG AN RTL-SDR INTO THE HOST PORT.", -1);
            return;
        }
        if (g_armed) walkie_arm(false);         /* the stick only receives */
        w_at = i;
        rtl_spectrum_squelch(param_i_get(cfg_rtl_sql));
        rtl_spectrum_tune(c->hz + G_STICK_OFF);
        stick_at = c->hz + G_STICK_OFF;
        rtl_spectrum_mute(false);
        rtl_spectrum_mux(false);
        heard = HEAR_RTL;
        hear_vol_refresh();
        rtl_spectrum_listen(c->hz, w_modes[w_mode].rtl);
    } else {
        int m = w_modes[w_mode].x62;

        if (m == -2) m = c->hz >= 10000000 ? radio_mode_usb : radio_mode_lsb;
        w_at = i;
        if (heard != HEAR_X6200) hear_apply(HEAR_X6200);     /* the X6200 on the speaker again */
        tune(c->hz, m);
    }
    show_walkie();
    for (int k = 0; k < 5; k++) buttons_refresh(&btn[k]);
}

/* The channel the X6200's dial is on, for a G that has none yet (-1 off the plan) */
static int w_from_dial(void) {
    int32_t fq = cparam_i_get(cfg_fg_freq);

    for (int i = 0; i < w_n; i++)
        if (!w_is_rtl(i) && labs(w_chs[i].hz - fq) <= 3000) return i;
    return -1;
}

static void walkie_group(void) {
    int         i = w_at < 0 ? 0 : w_at;
    const char *g;

    w_plan();
    g = w_chs[i].grp;
    while (i < w_n && !strcmp(w_chs[i].grp, g)) i++;    /* the next group */
    if (i >= w_n) i = 0;
    for (int k = i; k < w_n && !strcmp(w_chs[k].grp, w_chs[i].grp); k++)
        if (w_chs[k].home) {
            i = k;
            break;
        }
    w_mode = w_chs[i].mode;
    walkie_go(i);
}

static void walkie_mode_next(void) {
    int i;

    w_plan();
    i = w_at < 0 ? w_from_dial() : w_at;
    if (i < 0) i = 18;
    for (int k = 1; k <= W_MODES; k++)
        if (w_mode_ok(i, (w_mode + k) % W_MODES)) {
            w_mode = (w_mode + k) % W_MODES;
            break;
        }
    walkie_go(i);
}

/* F3 and F4's names on G: the group and the mode */
static void walkie_key_label(int k, char *s, size_t n) {
    int wi;

    w_plan();
    wi = w_at >= 0 ? w_at : w_from_dial();
    if (k == 2) snprintf(s, n, "Group:\n%s", wi >= 0 ? w_chs[wi].grp : "CB");
    else snprintf(s, n, "Mode:\n%s", w_modes[w_mode].name);
}

/* CH- / CH+ (and the MFK, the VFO on a stick channel): the next channel of the plan, its radio taken by itself;
 * to: CB channel to (the MFK's push: 19) */
static void walkie_ch(int dir, int to) {
    int i;

    w_plan();
    i = w_at < 0 ? w_from_dial() : w_at;
    if (to) i = to - 1;
    else if (i < 0) i = 18;
    else i = (i + dir + w_n) % w_n;
    if (w_at < 0 || strcmp(w_chs[i].grp, w_chs[w_at].grp)) w_mode = w_chs[i].mode;     /* another group */
    walkie_go(i);
}

static void show_walkie(void) {
    char           fr[40], nm[80], lk[40], tx[24], pw1[40], pw2[40], sy1[40], sy2[40], f[24];
    int32_t        fq = cparam_i_get(cfg_fg_freq);
    int            ch = -1, bi = band_of(fq);
    float          ext = 0, bat = 0;
    uint8_t        cap = 0;
    bool           chg = false;
    ident_t        id;
    static float   l_bat, l_ext;            /* the base's last word: its reports come and go between reads */
    static uint8_t l_cap;
    static bool    l_chg;

    if (!wui->num_l) return;
    w_plan();
    int  wi  = w_at >= 0 ? w_at : w_from_dial();
    bool rtl = w_is_rtl(wi) && heard == HEAR_RTL;
    char via[64];

    (void)ch;
    if (rtl) {                          /* the stick serves it: its frequency, its mode */
        const wch_t *c = &w_chs[wi];
        snprintf(fr, sizeof(fr), "%d.%04d %s", c->hz / 1000000, (c->hz % 1000000) / 100, w_modes[w_mode].name);
        snprintf(nm, sizeof(nm), "%s %s", c->grp, c->name);
        snprintf(via, sizeof(via), "VIA #%s RTL-SDR#  RECEIVE ONLY", tag_hex);
    } else {
        snprintf(f, sizeof(f), "%d.%03d.%02d", fq / 1000000, (fq / 1000) % 1000, (fq % 1000) / 10);
        snprintf(fr, sizeof(fr), "%s %s", f, mode_name(cparam_i_get(cfg_cur_mode)));
        identify_ex(fq, &id, false);
        snprintf(nm, sizeof(nm), "%s", id.name);
        snprintf(via, sizeof(via), "VIA X6200");
    }
    if (wi >= 0 && (rtl || labs(w_chs[wi].hz - fq) <= 3000)) {
        const wch_t *c = &w_chs[wi];
        bool         cb = !strcmp(c->grp, "CB");

        lv_label_set_text(wui->num_l, c->name);
        lv_label_set_text(wui->band_l, cb && !strcmp(c->name, "09") ? "CB EMERGENCY" : cb && !strcmp(c->name, "19")
                                       ? "CB TRUCKERS" : c->grp);
    } else {
        lv_label_set_text(wui->num_l, "--");
        lv_label_set_text(wui->band_l, bi >= 0 ? bands[bi].name : "OFF THE PLAN");
    }
    if (wui->via_l) lv_label_set_text(wui->via_l, via);
    snprintf(lk, sizeof(lk), "SQL %d  VOL %d", param_i_get(cfg_squelch), radio_change_vol(0));
    snprintf(tx, sizeof(tx), "%s", g_armed ? "TX ARMED" : rx_held ? "TX LOCKED" : "TX NOT LOCKED");
    clock_power_get(&ext, &bat, &cap, &chg);
    if (bat > 1 || ext > 1) l_bat = bat, l_ext = ext, l_cap = cap, l_chg = chg;
    if (l_bat > 1) snprintf(pw1, sizeof(pw1), "BAT %.1f V %d%%%s", l_bat, l_cap, l_chg ? " CHARGING" : "");
    else snprintf(pw1, sizeof(pw1), "NO BATTERY");
    snprintf(pw2, sizeof(pw2), "EXT %.1f V  TX %.0f W", l_ext, param_f_get(cfg_pwr));
    {
        int    c  = clock_cpu_temp();
        double ld = 0;
        FILE  *fl = fopen("/proc/loadavg", "r");

        if (fl) {
            if (fscanf(fl, "%lf", &ld) != 1) ld = 0;
            fclose(fl);
        }
        if (c != CLOCK_NO_TEMP) snprintf(sy1, sizeof(sy1), "CPU %dC  LD %.1f", c, ld);
        else snprintf(sy1, sizeof(sy1), "LOAD %.1f", ld);
    }
    const char *ver = radio_base_ver_str();

    snprintf(sy2, sizeof(sy2), "BASE %s", ver && ver[0] ? ver : "?");
    if (g_portrait) {
        time_t    now = time(NULL);
        struct tm tm;
        char      hm[16];

        lv_label_set_text(wui->freq_l, fr);
        localtime_r(&now, &tm);
        strftime(hm, sizeof(hm), "%H:%M %Z", &tm);
        uint32_t bc = l_bat > 1 ? batt_rgb(l_cap) : 0;     /* the title bar's colours as it runs down */
        char     pwc[64];

        if (bc) snprintf(pwc, sizeof(pwc), "#%06X %s#", (unsigned)bc, pw1);
        else snprintf(pwc, sizeof(pwc), "%s", pw1);
        lv_label_set_text_fmt(wui->info_l, "LINK    %s  %s\nPOWER   %s\n        %s\nSYSTEM  %s  %s\nTIME    %s",
                              lk, tx, pwc, pw2, sy1, sy2, hm);
        lv_label_set_text(wui->ch.opt[0], nm);
    } else {
        opt_set(&wui->ch, 0, fr, 0);
        opt_set(&wui->ch, 1, nm, 2);
        opt_set(&wui->link, 0, lk, 0);
        opt_set(&wui->link, 1, tx, rx_held ? 2 : 1);
        opt_set(&wui->pow, 0, pw1, 0);
        opt_set(&wui->pow, 1, pw2, 2);
        opt_set(&wui->sys, 0, sy1, 0);
        opt_set(&wui->sys, 1, sy2, 2);
    }
}

/* The voiceprint's picture, its kHz marks and its caption, at x, y in p */
static void vp_make(wui_t *u, lv_obj_t *p, int x, int y, int w, int h) {
    rect(p, x - 4, y - 3, w + 8, h + 6, 3, false);
    u->vp_w     = w;
    u->vp_h     = h;
    u->vp_frame = lv_img_buf_alloc(w, h, LV_IMG_CF_TRUE_COLOR);
    u->vp_img   = lv_img_create(p);
    lv_img_set_src(u->vp_img, u->vp_frame);
    lv_obj_set_pos(u->vp_img, x, y);
    {
        uint32_t *px = (uint32_t *)u->vp_frame->data;
        for (int i = 0; i < w * h; i++) px[i] = vp_pal[0];
    }
    for (int k = 1; k <= 3; k++) {      /* 1, 2, 3 kHz */
        char t[8];

        snprintf(t, sizeof(t), "%dK", k);
        label(p, &pixel_16, dim, x + 4, y + h - k * h * 1000 / VP_HZ - 8, 30, LV_TEXT_ALIGN_LEFT, t);
    }
    u->cap_l = label(p, &pixel_16, amb, x + w - 260, y + 3, 252, LV_TEXT_ALIGN_RIGHT, "VOICEPRINT  RX  0-4 KHZ");
}

/* A bar: its name, the scale, the fill and its value */
static void bar_make(wui_t *u, lv_obj_t *p, int x, int y, int w, int h, bool audio) {
    rect(p, x, y, w, h, 3, false);
    label(p, &pixel_16, amb, x + 8, y + 10, 80, LV_TEXT_ALIGN_LEFT, audio ? "AUDIO" : "S-METER");
    u->bar_w = w - 100;
    rect(p, x + 88, y + 6, u->bar_w + 6, 24, 2, false);
    for (int k = 1; k < 10; k++) rect(p, x + 91 + k * u->bar_w / 10, y + 9, 2, 18, 0, true);
    lv_obj_t *fl = rect(p, x + 91, y + 9, 1, 18, 0, true);
    lv_obj_t *vl = label(p, &pixel_16, amb, x + 88, y + 34, u->bar_w + 6, LV_TEXT_ALIGN_RIGHT, "");

    if (audio) u->a_fill = fl, u->a_l = vl;
    else u->s_fill = fl, u->s_l = vl;
}

static void vp_palette(void) {
    for (int i = 0; i < 256; i++) {     /* ink, dim, amber, then amber whitened at the loudest */
        lv_color_t c = i < 96    ? lv_color_mix(dim, ink, (uint8_t)(i * 255 / 96))
                       : i < 200 ? lv_color_mix(amb, dim, (uint8_t)((i - 96) * 255 / 104))
                                 : lv_color_mix(lv_color_white(), amb, (uint8_t)((i - 200) * 120 / 55));
        vp_pal[i] = argb(c);
    }
}

/* Landscape, the Sentry's look: four boxes, the channel and lamp beside the voiceprint, the bars */
static void build_walkie(lv_obj_t *p) {
    wui_t *u  = &w_land;
    int    w1 = 226, w2 = 150, w3 = 206, w4 = SW - 12 - w1 - w2 - w3 - 18, y = BODY_Y, vh = 176;

    vp_palette();
    box_make(&u->ch, p, 6, y, w1, LOW_H, "CHANNEL", 2, -1);
    box_make(&u->link, p, 6 + w1 + 6, y, w2, LOW_H, "LINK", 2, -1);
    box_make(&u->pow, p, 6 + w1 + w2 + 12, y, w3, LOW_H, "POWER", 2, -1);
    box_make(&u->sys, p, 6 + w1 + w2 + w3 + 18, y, w4, LOW_H, "SYSTEM", 2, -1);
    lv_label_set_long_mode(u->ch.opt[1], LV_LABEL_LONG_SCROLL_CIRCULAR);

    int my = y + LOW_H + 6;

    rect(p, 6, my, 160, vh + 6, 3, false);
    label(p, &pixel_16, amb, 6, my + 6, 160, LV_TEXT_ALIGN_CENTER, "CH");
    u->num_l  = label(p, &pixel_44, amb, 6, my + 22, 160, LV_TEXT_ALIGN_CENTER, "--");
    u->band_l = label(p, &pixel_16, amb, 6, my + 72, 160, LV_TEXT_ALIGN_CENTER, "");
    u->via_l  = label(p, &pixel_16, amb, 6, my + 88, 160, LV_TEXT_ALIGN_CENTER, "");     /* over the lamp */
    lv_label_set_recolor(u->via_l, true);
    lv_label_set_long_mode(u->via_l, LV_LABEL_LONG_SCROLL_CIRCULAR);
    u->lamp   = rect(p, 20, my + 108, 132, 62, 3, false);
    lv_obj_set_style_bg_color(u->lamp, amb, 0);
    u->lamp_l = label(p, &pixel_24, amb, 20, my + 114, 132, LV_TEXT_ALIGN_CENTER, "RX\nQUIET");
    vp_make(u, p, 176, my + 3, 612, vh);

    int by = my + vh + 12, bw = (SW - 12 - 6) / 2;

    bar_make(u, p, 6, by, bw, scr_h - by - 6, false);
    bar_make(u, p, 6 + bw + 6, by, bw, scr_h - by - 6, true);
}

/* Portrait, held up: 480 x 800, the F keys' names in a column on the left where the keys are (160 px apart, F1 at
 * the top, as the Walkie app has them), the rest under each other */
#define PW      480
#define PH      800
#define P_SIDE  104
#define P_X     (P_SIDE + 8)
#define P_W     (PW - P_X - 6)

/* The radio's keys and its main knob go to the screen shown: held up, G's. They go on to the main screen, where the
 * Sentry has them (its front_key, its rotary_cb), as the Walkie app sends them on. A knob's turn is the receiver's to
 * free */
static void g_fwd_cb(lv_event_t *e) {
    if (g_prev_scr && lv_obj_is_valid(g_prev_scr))
        lv_event_send(g_prev_scr, lv_event_get_code(e), lv_event_get_param(e));
    else if (lv_event_get_code(e) == EVENT_ROTARY)
        free(lv_event_get_param(e));
}

static void build_walkie_portrait(void) {
    wui_t *u = &w_port;
    g_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_size(g_scr, PW, PH);
    lv_obj_set_style_bg_color(g_scr, ink, 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_scr, g_fwd_cb, EVENT_KEYPAD, NULL);
    lv_obj_add_event_cb(g_scr, g_fwd_cb, EVENT_ROTARY, NULL);
    /* the left column: the radio's own F buttons, moved here while G shows (walkie_show), as the Walkie app does */
    /* the head: G, the name, the time */
    rect(g_scr, P_X, 6, P_W, 44, 3, false);
    label(g_scr, &pixel_24, amb, P_X, 16, P_W - 44, LV_TEXT_ALIGN_CENTER, "WALKIE TALKIE");
    /* its letter where every other screen has its own, at the display's top left: held up, the top right */
    {
        lv_obj_t *c = rect(g_scr, PW - 43, 12, 36, 36, 3, false);

        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        label(g_scr, &pixel_28, amb, PW - 43, 15, 36, LV_TEXT_ALIGN_CENTER, "G");
    }
    /* the channel */
    rect(g_scr, P_X, 56, P_W, 168, 3, false);
    u->num_l  = label(g_scr, &pixel_44, amb, P_X, 82, P_W, LV_TEXT_ALIGN_CENTER, "--");
    u->band_l = label(g_scr, &pixel_20, amb, P_X, 132, P_W, LV_TEXT_ALIGN_CENTER, "");
    u->freq_l = label(g_scr, &pixel_20, amb, P_X, 158, P_W, LV_TEXT_ALIGN_CENTER, "");
    u->via_l  = label(g_scr, &pixel_16, amb, P_X, 62, P_W, LV_TEXT_ALIGN_CENTER, "");   /* the channel box's head */
    lv_label_set_recolor(u->via_l, true);
    u->info_l = NULL;
    u->ch.opt[0] = label(g_scr, &pixel_16, dim, P_X + 8, 196, P_W - 16, LV_TEXT_ALIGN_CENTER, "");  /* what it is */
    lv_label_set_long_mode(u->ch.opt[0], LV_LABEL_LONG_SCROLL_CIRCULAR);
    u->ch.n = 1;
    /* the lamp */
    u->lamp = rect(g_scr, P_X, 230, P_W, 48, 3, false);
    lv_obj_set_style_bg_color(u->lamp, amb, 0);
    u->lamp_l = label(g_scr, &pixel_28, amb, P_X, 240, P_W, LV_TEXT_ALIGN_CENTER, "RX  QUIET");
    /* the voiceprint, the meters */
    vp_make(u, g_scr, P_X + 4, 288, P_W - 8, 268);
    bar_make(u, g_scr, P_X, 566, P_W, 56, false);
    bar_make(u, g_scr, P_X, 628, P_W, 56, true);
    /* the radio's state, a word for what was refused */
    rect(g_scr, P_X, 690, P_W, 104, 3, false);
    u->info_l = label(g_scr, &pixel_16, amb, P_X + 8, 696, P_W - 16, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_recolor(u->info_l, true);      /* the battery's line coloured as it runs down */
    u->msg_l  = label(g_scr, &pixel_16, amb, P_X + 8, 776, P_W - 16, LV_TEXT_ALIGN_LEFT, "");
}

/* G shown (on) or left: held up in portrait where the build can, its own screen in place of the Sentry's */
static void walkie_show(bool on) {
    if (on && !g_portrait && g_scr && main_display_portrait(true)) {
        g_portrait = true;
        wui        = &w_port;
        g_prev_scr = lv_scr_act();
        buttons_vertical(g_scr, P_SIDE, 160);      /* the F keys reach only buttons on the screen shown */
        for (int i = 0; i < 5; i++) buttons_refresh(&btn[i]);
        lv_scr_load(g_scr);
        show_walkie();
    } else if (!on && g_portrait) {
        if (g_prev_scr && lv_obj_is_valid(g_prev_scr)) buttons_horizontal(g_prev_scr);   /* under the Sentry again */
        main_display_portrait(false);
        g_portrait = false;
        wui        = &w_land;
        if (g_prev_scr && lv_obj_is_valid(g_prev_scr)) lv_scr_load(g_prev_scr);
    } else if (on) {
        wui = &w_land;
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
                   "VFO KNOB       THE SCREEN'S RADIO: A'S X6200 (MAN-OVERRIDE), B'S STICK CHANNEL\n"
                   "MFK KNOB       CHOOSES A TARGET, A BAND, A FREQUENCY. PUSH: TRACK, LISTEN, OPEN\n"
                   "MFK RING       A X6200  B VHF/UHF  C LOG  D BANDS  E DECODE  F SYSTEM  G WALKIE\n"
                   "A AND B        F1 SCAN  F2 BAND  F3 VIEW  F4 LISTEN  F5 SCAN CFG; MFK PANS\n"
                   "VOL KNOB       VOLUME, PUSH: SQUELCH, PUSH: RF GAIN (ON B THE STICK'S)\n"
                   "LISTEN         A AND B EACH: X6200, RTL, BOTH OR OFF; OTHER SCREENS KEEP IT\n"
                   "BAND UP/DOWN   THE NEXT BAND, AS F2 (ON B: THE NEXT PRESET)\n"
                   "AM  CW  SSB    THE MODE: AM BROADCAST AND CB, SSB HAM VOICE, CW MORSE\n"
                   "DFN            NOISE REDUCTION ON/OFF. HOLD: NOISE BLANKER (CLICKS AND POPS)\n"
                   "DFL            FILTER WIDE, NORMAL, NARROW (NARROW: LESS NOISE, MUFFLED)\n"
                   "FST            HOW FAR ONE CLICK OF THE VFO KNOB MOVES\n"
                   "ATU            ANTENNA TUNER IN/OUT. TUNING ITSELF TRANSMITS: NOT HERE\n"
                   "A/B            ON G: ARMS THE TRANSMITTER FOR PTT (AGAIN, F5: LOCKED)\n"
                   "PTT  KEY  MSG  TRANSMIT: LOCKED OUT IN THE SENTRY (PTT: G WITH A/B)\n"
                   "APP            F SYSTEM: SETTINGS, APPS, MODES, THE CLASSIC RADIO SCREEN\n"
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
        snprintf(tag_hex, sizeof(tag_hex), "%06X", (unsigned)(lv_color_to32(orange) & 0xffffff));    /* the tags */
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
    build_log(scr_obj[SCR_LOG]);
    build_walkie(scr_obj[SCR_WALKIE]);
    build_walkie_portrait();
    build_keys();
    build_overlays();

    static bool sweep_set;
    if (!sweep_set) {
        for (int i = 0; i < BANDS_N; i++) band_rt[i].sweep = bands[i].sweep;
        sweep_set = true;
    }
    if (!hits_loaded) hits_load();
    if (!rtl_hz) rtl_hz = param_i_get(cfg_rtl_freq);      /* the stick from now on, at F's tuning (rtl_spectrum.h) */
    rtl_spectrum_squelch(param_i_get(cfg_rtl_sql));
    rtl_spectrum_want(true, rtl_hz, param_i_get(cfg_rtl_gain));
    g_on = -1;
    g_stick = false;
    if (g_sel >= nhits) g_sel = 0;
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

    {
        scr_t s = open_at;

        open_at = SCR_SCAN;
        go(SCR_SCAN);
        if (s != SCR_SCAN) go(s);       /* from A, as a turn of the ring would (G held up, B's picture) */
    }
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
    if (scr == SCR_LOG) g_leave(false);
    audio_scope(false);
    walkie_show(false);                 /* landscape again before anything else draws */
    walkie_arm(false);                  /* the transmitter locked (the hold let go below with the rest) */
    radio_set_vol_override(-1);         /* the speaker at the X6200's volume again */
    for (int i = 0; i < 2; i++) {
        wui_t *u = i ? &w_port : &w_land;

        if (u->vp_frame) {
            lv_img_cache_invalidate_src(u->vp_frame);
            lv_img_buf_free(u->vp_frame);
        }
        memset(u, 0, sizeof(*u));
    }
    if (g_scr) lv_obj_del(g_scr);
    g_scr = NULL;
    wui   = &w_land;
    rtl_want(false);
    if (hits_dirty) hits_save();
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

/* The Sentry on one of its screens by letter ("A" to "G"; "H" is AIRSPACE): AIRSPACE's ring comes back so */
void app_sentry_open_at(char letter) {
    static const scr_t by[] = {SCR_SCAN, SCR_RTL, SCR_LOG, SCR_BANDS, SCR_DECODE, SCR_SYSTEM, SCR_WALKIE};

    if (letter == 'H') {
        main_screen_start_app(ACTION_APP_ADSB);
        return;
    }
    if (letter < 'A' || letter > 'G') return;
    if (dialog_type_is_run(dialog_sentry)) {
        go(by[letter - 'A']);
        return;
    }
    open_at = by[letter - 'A'];
    main_screen_start_app(ACTION_APP_SENTRY);
}
