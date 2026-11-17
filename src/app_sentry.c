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
 *              the X6200 (MANUAL). SCAN CFG's style says what its scans do on a signal: stop on it (AUTO: the strongest
 *              target not seen lately, tracked) or only mark it (SEMI). Routines: BAND, the 360 kHz at the dial; SCAN, the band end to end,
 *              360 kHz at a time; SWEEP, the bands marked in D BANDS, a few seconds each.
 *    B VHF/UHF an RTL-SDR stick's spectrum (rtl_spectrum.c): its presets or D's bands, scanned end to end (F1); the
 *              VFO tunes its channel (a bracket), the MFK pans, its push goes to the strongest.
 *    C LOG     the signals A's and B's scans locked on, newest first, kept on DATA (sentry_log.txt): the MFK goes
 *              through them and each is heard on its own receiver, the X6200 or the stick; F4 scans the list.
 *    G WALKIE  the radio as a CB walkie-talkie, held up (portrait): the channel, a voiceprint, the meters. Its G
 *              where the others have their letter (the portrait screen's top right, the display's top left).
 *    H AIRSPACE ADS-B aircraft: the AIRSPACE app, opened by the ring past G (or before A), and by itself when a GNS
 *              5890 or a microADSB is or becomes the only ADS-B receiver on USB (screen_main.c). Its ring comes back
 *              here.
 *    A/B       on A and G arms the transmitter for PTT (the Sentry is receive only otherwise).
 *    D BANDS   what each band carries, when, with which antenna, and what the Sentry found there; LISTEN tunes it
 *              and scans it (the stick's on C), SWEEP marks it for the sweep.
 *    E DECODE  the decoders and the frequencies they work on; OPEN takes the radio there in the radio's own app.
 *    F SYSTEM  the rest of the radio, in this look: its settings, the other apps, the digital modes menu, the
 *              classic radio screen (with transmit), and where the radio starts. The APP key comes here.
 *    SCAN CFG  (on no ring: F5 on A or B) how the scans behave, A's and B's alike, as rows the MFK goes through and
 *              its push changes: the SCAN STYLE (BROWSE, SCANNER, SAMPLER, STOP & STAY, SURVEY) or its three settings
 *              (STEP TIME, STAY ON A SIGNAL, GO ON AFTER), A's SWEEP and knob step, B's knob step, gain and squelch;
 *              what the chosen row does, and each radio's scan now. F1 STYLE, F2 HOLD / GO ON and F3 SKIP for the
 *              radio it was opened from, F4 REC (A), F5 back.
 *
 *  On the X6200 the Sentry is the home screen (cfg_sentry_home): the radio starts in it, and an app or a menu opened
 *  from it comes back to it when it closes (screen_main.c main_screen_dialog_closed). CLASSIC RADIO leaves it till it
 *  is opened again.
 *
 *  The picture's bins: bin n/2 is the dial (the base's FFT shifted, its DC in the middle), checked on the night AM
 *  band where the stations sit on their 10 kHz channels.
 */

#include "app_sentry.h"
#include "app_adsb.h"

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
#include "cfg/tx_ranges.h"     /* the SDR walkie's bands: tx_ranges_sdr_check */
#include "dsp.h"
#include "events.h"
#include "gui_styles.h"
#include "keyboard.h"
#include "main.h"             /* VERSION, the release in the title */
#include "waterfall_view.h"   /* direct_hole_cb */
#include "gpu.h"
#include "kth.h"
#include "lv_drivers/display/drm.h"
#include "main_screen.h"
#include "msg_box.h"
#include "params/params.h"
#include "qth/qth.h"
#include "radio.h"
#include "rtl_spectrum.h"
#include "rtl_watch.h"
#include "sdr_pool.h"
#include "df_rx.h"
#include "remote_server.h"
#include "control_owner.h"
#include "adsb_rx.h"
#include "wr12.h"
#include "recorder.h"
#include "styles.h"
#include "theme.h"
#include "gui_state.h"
#include "btaudio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>

extern const lv_font_t pixel_12, pixel_16, pixel_20, pixel_24, pixel_28, pixel_32, pixel_44;

/* A label's text set only when it is another: LVGL 8.3 lays a label out again and redraws it (with all under it) for
 * any text set, the same one too, and the Sentry sets its boxes' texts at every tick. On G the meters' two numbers
 * 20 times a second were a frame of the GUI each, 20 frames a second for a screen standing still (2026-11-17).
 * Every lv_label_set_text below is this one; NULL or the label's own text (a refresh) still go through. The text is
 * translated here, as i18n_hook.h's lv_label_set_text does, and compared translated */
static void label_set_text(lv_obj_t *l, const char *t) {
    const char *now = l ? lv_label_get_text(l) : NULL;

    t = i18n_tr(t);
    if (t && now && t != now && !strcmp(now, t)) return;
    (lv_label_set_text)(l, t);          /* LVGL's own (the name in brackets: no macro) */
}
#undef lv_label_set_text
#define lv_label_set_text label_set_text

/* The same for the styles set at every tick: a style set restyles and redraws the object even when the value is the
 * one it has (G's lamp, a box a third of the screen, 20 times a second; the title bar's battery each half second on
 * every screen; G's turned one, drawn through LVGL's slow turn). Set as LVGL's own setter does (lv_obj_style_gen.c),
 * only when the object's own value for that selector is another */
static void style_set(lv_obj_t *o, lv_style_prop_t p, lv_style_value_t v, lv_style_selector_t sel, bool color) {
    lv_style_value_t now;

    if (lv_obj_get_local_style_prop(o, p, &now, sel) == LV_RES_OK &&
        (color ? lv_color_to32(now.color) == lv_color_to32(v.color) : now.num == v.num))
        return;
    lv_obj_set_local_style_prop(o, p, v, sel);
}
#define lv_obj_set_style_text_color(o, c, sel)   style_set(o, LV_STYLE_TEXT_COLOR, (lv_style_value_t){.color = (c)}, sel, true)
#define lv_obj_set_style_bg_color(o, c, sel)     style_set(o, LV_STYLE_BG_COLOR, (lv_style_value_t){.color = (c)}, sel, true)
#define lv_obj_set_style_border_color(o, c, sel) style_set(o, LV_STYLE_BORDER_COLOR, (lv_style_value_t){.color = (c)}, sel, true)
#define lv_obj_set_style_bg_opa(o, a, sel)       style_set(o, LV_STYLE_BG_OPA, (lv_style_value_t){.num = (a)}, sel, false)

/* And flags: showing a shown object (or any flag cleared that is not set) redraws it all the same */
static void flag_add(lv_obj_t *o, lv_obj_flag_t f) {
    if (!lv_obj_has_flag(o, f)) lv_obj_add_flag(o, f);
}

static void flag_clear(lv_obj_t *o, lv_obj_flag_t f) {
    if (lv_obj_has_flag_any(o, f)) lv_obj_clear_flag(o, f);
}
#define lv_obj_add_flag   flag_add
#define lv_obj_clear_flag flag_clear

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
static bool  open_pool;                    /* and the SDR pool's panel over it (app_sentry_open_pool: from H) */
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
    /* Above the X6200's own receiver: the RTL-SDR's (F VHF/UHF), or a WR12's. Their antenna: a dipole (the telescopic
     * one in an RTL-SDR's kit does them all) with each arm the length given, a quarter wave at the band's middle less
     * 5 %; WX SAT's the V-dipole of 53.4 cm arms, 120 degrees apart, flat */
    {"AIR", 118000, 137000, "AIRCRAFT VOICE (AM)", "DIPOLE 2X56 CM", 'L', 121500000, radio_mode_am, false,
     "Pilots and controllers in AM, 25 kHz apart; 121.500 is the emergency guard. Airliners' ACARS texts on E."},
    {"WX SAT", 137000, 138000, "METEOR WEATHER PICTURES", "V-DIPOLE 2X53 CM", 'A', 137100000, radio_mode_nfm,
     false, "Meteor-M weather satellites on 137.1 and 137.9, a few passes a day: B decodes their pictures (/mnt/meteor)."},
    {"2 M", 144000, 148000, "HAMS: REPEATERS, APRS", "DIPOLE 2X49 CM", 'L', 146520000, radio_mode_nfm, false,
     "Local hams in narrow FM: repeaters 145-148, simplex 146.520, APRS packets on 144.390."},
    {"MARINE", 156000, 162025, "BOATS; CHANNEL 16 CALLS", "DIPOLE 2X45 CM", 'L', 156800000, radio_mode_nfm, false,
     "Marine VHF in narrow FM: channel 16 (156.800) for calls and distress, AIS data on 161.975 / 162.025."},
    {"NOAA WX", 162400, 162550, "WEATHER RADIO, ALERTS", "DIPOLE 2X44 CM", 'L', 162550000, radio_mode_nfm, false,
     "NOAA Weather Radio around the clock, with SAME alerts the Sentry shows in its title."},
    {"70 CM", 420000, 450000, "HAMS: REPEATERS, SIMPLEX, DMR", "DIPOLE 2X16 CM", 'L', 446000000, radio_mode_nfm, false,
     "Local hams in narrow FM: repeaters 442-450, simplex 446.000."},
    {"GMRS", 462550, 467725, "FAMILY RADIOS: FRS, GMRS", "DIPOLE 2X15 CM", 'L', 462562500, radio_mode_nfm, false,
     "Handheld family radios, FRS and GMRS, 12.5 kHz apart: a few miles at most."},
    {"ADS-B", 1089000, 1091000, "AIRCRAFT ON A MAP", "DIPOLE 2X65 MM", 'L', 1090000000, radio_mode_nfm, false,
     "Aircraft send their position, height and flight on 1090 MHz. LISTEN opens H AIRSPACE, which draws them."},
};

#define BANDS_N ((int)(sizeof(bands) / sizeof(bands[0])))

static struct {
    bool     sweep;
    int      seen;          /* targets the last time the picture was on it */
    float    best;
    uint32_t ms;            /* lv_tick of that, 0 never */
} band_rt[BANDS_N];

/* The bands the SDR takes (rx_of): VHF and UHF when one is plugged in (D's RX column says its name, LISTEN opens B on
 * it), and those only it reaches (above the X6200's own receiver, without a WR12) always */
static char tag_hex[8] = "E07020";      /* the tags' colour (RTL, ADS-B, WR12, RX): the 3D picture's orange, set from
                                          * the theme at the Sentry's start (construct_cb) */
static bool sdr_here;           /* an RTL-SDR on USB (adsb_rx_sdr_present), looked at every 2 s */
static bool adsb_here;          /* an ADS-B receiver on USB, or AIRSPACE decoding (the stick's dump1090) */
static bool wr12_here;          /* the WR12 on HOST, found and set up */

/* Within what the stick tunes: an RTL-SDR's 24 MHz-1.7 GHz, a HackRF's 1 MHz-6 GHz, a bladeRF's (B's F2 goes through
 * these) */
static bool band_in_rtl(int i) {
    return (int64_t)bands[i].lo * 1000 >= rtl_spectrum_min_hz() && (int64_t)bands[i].hi * 1000 <= rtl_spectrum_max_hz();
}

/* Which radio receives a frequency, on D and E alike (the user, 2026-11-17: "the radio required to receive or transmit
 * them is represented"): HF and 6 m the X6200's, whose receiver and antennas they are (with an SDR plugged in, 12 m,
 * CB, 10 m and 6 m went to it before, on its VHF antenna, while their rows said CB WHIP); above, the SDR's when one is
 * plugged in, the X6200's where it tunes alone (FM broadcast, the airband, a WR12), the SDR's where only it reaches */
#define HF_TOP_HZ 60000000
enum { RX_NONE, RX_X6200, RX_SDR };

static int rx_of(int64_t hz) {
    bool x = hz <= INT32_MAX && cfg_is_valid_hw_freq((int32_t)hz);
    bool s = hz >= rtl_spectrum_min_hz() && hz <= rtl_spectrum_max_hz();

    if (x && hz <= HF_TOP_HZ) return RX_X6200;
    if (s && (sdr_here || !x)) return RX_SDR;
    return x ? RX_X6200 : RX_NONE;
}

static bool band_uses_rtl(int i) {
    return band_in_rtl(i) && rx_of(bands[i].listen_hz) == RX_SDR;
}

/* What a choice on D (LISTEN, SCAN) or E (OPEN) sets besides the frequency and the mode, one rule for both screens and
 * shown on both (the user, 2026-11-17: "pre-amp defaults for each selection, and any other settings permutations that
 * are applicable"):
 *   the X6200: the preamp off under 8 MHz (the band's own noise is louder than the receiver's there: the preamp only
 *     adds overload from the big signals) and on AM and FM broadcast (strong locals), on from 8 MHz up and on the
 *     airband; the attenuator off; AGC FAST for CW, SLOW for a decoder app, AUTO (the mode's own) for listening
 *   the SDR: its gain: 19.7 dB on FM broadcast, 49.6 on the weather satellites and from 960 MHz up (weak), 37.2 on the
 *     airband, marine and NOAA (far transmitters), 28.0 elsewhere (local repeaters and handhelds) */
typedef struct {
    int pre, att, agc;          /* the X6200's (radio_pre_t, radio_att_t, radio_agc_t), -1 none */
    int gain;                   /* the SDR's, tenths of a dB; -2 none; -3 AIRSPACE's own (ADS-B) */
} rxset_t;

#define ADSB_HZ 1090000000             /* D's ADS-B band: LISTEN opens H AIRSPACE (dump1090 or the stick: their gain) */

/* sdr: on the SDR (E's types say which), else on the radio that receives hz (rx_of) */
static rxset_t rx_setup_on(int64_t hz, int agc, bool sdr) {
    rxset_t s = {radio_pre_off, radio_att_off, agc, -2};
    int64_t k = hz / 1000;
    bool    bc = (k >= 530 && k <= 1700) || (k >= 87500 && k <= 108000);

    if (sdr) {
        s.pre = s.att = s.agc = -1;
        s.gain = bc                                             ? 197
               : (k >= 137000 && k <= 138000) || k >= 960000    ? 496
               : (k >= 118000 && k < 137000) || (k >= 156000 && k <= 162550) ? 372
                                                                : 280;
        return s;
    }
    if (!bc && hz >= 8000000) s.pre = radio_pre_on;
    return s;
}

static rxset_t rx_setup(int64_t hz, int agc) {
    return rx_setup_on(hz, agc, rx_of(hz) == RX_SDR);
}

static void rx_setup_text(const rxset_t *s, char *t, size_t n) {
    static const char *const agc[] = {"OFF", "SLOW", "FAST", "AUTO"};

    if (s->gain == -3)
        snprintf(t, n, "AIRSPACE'S OWN");
    else if (s->gain > -2)
        snprintf(t, n, "GAIN %d.%d DB", s->gain / 10, s->gain % 10);
    else
        snprintf(t, n, "PRE %s%s, AGC %s", s->pre == radio_pre_on ? "ON" : "OFF", s->att == radio_att_on ? ", ATT" : "",
                 agc[s->agc & 3]);
}

/* Who may send there: the X6200 where its TX ranges let it (Settings > TX Cal), a transmitting SDR (a HackRF's, G
 * WALKIE's FM) on the amateur bands only, no one else (and no one for a mode only received: NAVTEX, weather fax) */
static void tx_text(int64_t hz, bool two_way, char *t, size_t n) {
    bool i32 = hz <= INT32_MAX;

    if (two_way && i32 && cfg_is_tx_freq((int32_t)hz) && !cfg_tx_refusal((int32_t)hz))
        snprintf(t, n, "TX X6200");
    else if (two_way && i32 && hz > HF_TOP_HZ && rtl_spectrum_can_tx() &&
             tx_ranges_sdr_check((tx_ranges_t)param_i_get(cfg_tx_ranges), (int32_t)hz) == HAM_TX_OK)
        snprintf(t, n, "TX %s ON G", rtl_spectrum_dev_tag());
    else
        snprintf(t, n, "RX ONLY");
}

/* ---- The decoders and where they work ---- */

/* E holds every receiving function of the radio, the X6200's apps and the SDR's decoders (the user, 2026-11-17: "E & D
 * fully in sync with all the available functions"); each frequency's band is one of D's, and D names E's types in
 * each band. Not here: JTTY (no calling frequencies yet), On the Air (a list of its own), the Scanner */
static const int32_t f_ft8[] = {1840000, 3573000, 5357000, 7074000, 10136000, 14074000, 18100000, 21074000,
                                24915000, 28074000, 50313000};
static const int32_t f_ft4[] = {3575000, 7047500, 10140000, 14080000, 18104000, 21140000, 24919000, 28180000,
                                50318000};
static const int32_t f_wspr[] = {1836600, 3568600, 5364700, 7038600, 10138700, 14095600, 18104600, 21094600,
                                 24924600, 28124600, 50293000};
static const int32_t f_js8[] = {3578000, 7078000, 10130000, 14078000, 18104000, 21078000, 28078000};
static const int32_t f_psk[] = {3580000, 7070000, 10142000, 14070000, 18100000, 21070000, 24920000, 28120000};
static const int32_t f_rtty[] = {3585000, 7085000, 14085000, 21085000, 28085000};
static const int32_t f_olivia[] = {3583000, 7072500, 10141000, 14072500, 18102000, 21072500, 28075000};
static const int32_t f_sstv[] = {3845000, 7171000, 14230000, 21340000, 28680000};
static const int32_t f_cw[] = {3530000, 7030000, 10115000, 14030000, 18080000, 21030000, 24900000, 28030000};
static const int32_t f_rade[] = {3625000, 7177000, 14236000, 21313000, 28330000};
static const int32_t f_navtex[] = {518000, 490000, 4209500};
static const int32_t f_dsc[] = {2187500, 4207500, 6312000, 8414500, 12577000, 16804500};
static const int32_t f_wefax[] = {4235000, 6340500, 9110000, 12750000, 4317900, 8503900, 12789900, 4346000,
                                  8682000, 12786000, 17151200};      /* NOAA's: Boston, New Orleans, Pt Reyes */
static const int32_t f_time[] = {2500000, 5000000, 10000000, 15000000, 20000000, 25000000};  /* WWV (dialog_timesig.c) */
static const int32_t f_beacon[] = {14100000, 18110000, 21150000, 24930000, 28200000};      /* NCDXF's */
static const int32_t f_same[] = {162400000, 162425000, 162450000, 162475000, 162500000, 162525000, 162550000};
static const int32_t f_rds[] = {92300000};
static const int32_t f_acars[] = {129125000, 130025000, 130425000, 130450000, 131125000, 131550000, 131725000};
static const int32_t f_adsb[] = {1090000000};
static const int32_t f_dv[] = {441000000, 446500000, 446075000};       /* DMR's simplex channels (B's DMR 70 CM) */
static const int32_t f_meteor[] = {137100000, 137900000};

#define N_OF(a) ((int)(sizeof(a) / sizeof(a[0])))
#define HZ(a)   a, N_OF(a)

typedef struct {
    const char     *name;
    const char     *mem;        /* the app's memory (app_window.c app_mem_save): it opens where that says; "": none */
    press_action_t  app;        /* 0: none (the main screen; B or H for the SDR's) */
    int             mode, proto;
    const int32_t  *hz;
    int             n;
    const char     *about;
    int             audio;      /* where the app's decoder listens, Hz: the dial that far under the station (USB) */
    char            rx;         /* X the X6200, B the SDR on B, H AIRSPACE (ADS-B), C AIRSPACE's ACARS on the SDR */
    int             agc;        /* the X6200's AGC for it (rx_setup) */
    bool            two_way;    /* hams send it too: the X6200 may, where its TX ranges let it */
    const char     *opens;      /* E's OPENS IN */
} decoder_t;

static const decoder_t decoders[] = {
    {"FT8", "FT8", ACTION_APP_FT8, radio_mode_usb_dig, 1, HZ(f_ft8),
     "15-SECOND DIGITAL MESSAGES FROM HAMS WORLDWIDE: CALLS, GRIDS, SIGNAL REPORTS. THE BUSIEST MODE ON THE AIR.", 0,
     'X', radio_agc_slow, true, "THE FT8 APP"},
    {"FT4", "FT4", ACTION_APP_FT8, radio_mode_usb_dig, 0, HZ(f_ft4),
     "FT8'S FASTER COUSIN, 7.5-SECOND SLOTS, USED IN CONTESTS. THE FT8 APP DECODES IT.", 0,
     'X', radio_agc_slow, true, "THE FT8 APP"},
    {"WSPR", "WSPR", ACTION_APP_WSPR, radio_mode_usb_dig, -1, HZ(f_wspr),
     "WEAK BEACONS: WHO IS HEARD WHERE, EVERY 2 MINUTES. THE WSPR APP CHOOSES ITS OWN BAND.", 0,
     'X', radio_agc_slow, true, "THE WSPR APP"},
    {"JS8", "JS8", ACTION_APP_JS8, radio_mode_usb_dig, -1, HZ(f_js8),
     "KEYBOARD CHAT BUILT ON FT8: SLOW, BUT IT GETS THROUGH. FEW STATIONS ON THE AIR.", 0,
     'X', radio_agc_slow, true, "THE JS8 APP"},
    {"PSK", "PSK", ACTION_APP_PSK, radio_mode_usb_dig, -1, HZ(f_psk),
     "PSK31: KEYBOARD CHAT, A LINE OF TEXT AT A TIME, THE CLASSIC DIGITAL CHAT. 14.070 IS THE BUSIEST.", 0,
     'X', radio_agc_slow, true, "THE PSK APP"},
    {"RTTY", "RTTY", ACTION_APP_RTTY, radio_mode_usb_dig, -1, HZ(f_rtty),
     "RADIOTELETYPE: TEXT IN TWO TONES, HAMS' CONTESTS AND CHATS. BUSIEST ON CONTEST WEEKENDS.", 0,
     'X', radio_agc_slow, true, "THE RTTY PANEL"},
    {"OLIVIA", "Olivia", ACTION_APP_OLIVIA, radio_mode_usb_dig, -1, HZ(f_olivia),
     "SLOW, VERY ROBUST TEXT THAT GETS THROUGH NOISE AND FADING. FEW STATIONS; WEEKENDS ARE BEST.", 0,
     'X', radio_agc_slow, true, "THE OLIVIA APP"},
    {"SSTV", "SSTV", ACTION_APP_SSTV, -1, -1, HZ(f_sstv),
     "SLOW-SCAN TV: PICTURES SENT BY HAMS, ONE EVERY MINUTE OR TWO. 14.230 IS THE PLACE.", 0,
     'X', radio_agc_slow, true, "THE SSTV APP"},
    {"CW", "", 0, radio_mode_cw, -1, HZ(f_cw),
     "MORSE CODE. THE MAIN SCREEN DECODES IT IN CW MODE; THE SLOWER, THE BETTER IT COPIES.", 0,
     'X', radio_agc_fast, true, "THE MAIN SCREEN"},
    {"RADE", "RADE", ACTION_APP_FREEDV, radio_mode_usb, -1, HZ(f_rade),
     "FREEDV'S DIGITAL VOICE: CLEAR SPEECH, NO HISS. IT NEEDS BLUETOOTH HEADPHONES, TCI OR THE BROWSER.", 0,
     'X', radio_agc_slow, true, "THE RADE APP"},
    {"NAVTEX", "NavTex", ACTION_APP_NAVTEX, radio_mode_usb, -1, HZ(f_navtex),
     "WEATHER AND NAVIGATION WARNINGS FOR SHIPS, AS TEXT. NEEDS A LONG WIRE FOR 518 KHZ.", 1000,
     'X', radio_agc_slow, false, "THE NAVTEX APP"},
    {"DSC", "NavTex", ACTION_APP_NAVTEX, radio_mode_usb, -1, HZ(f_dsc),
     "SHIPS' DIGITAL SELECTIVE CALLS: DISTRESS AND SAFETY, WITH THEIR NUMBERS AND POSITIONS.", 1000,
     'X', radio_agc_slow, false, "THE NAVTEX APP"},
    {"WEFAX", "WeFax", ACTION_APP_WEFAX, radio_mode_usb, -1, HZ(f_wefax),
     "WEATHER MAPS BY RADIO FROM NOAA: BOSTON, NEW ORLEANS, PT REYES. A MAP TAKES 10 MINUTES.", 1500,
     'X', radio_agc_slow, false, "THE WEFAX APP"},
    {"TIME SIGNAL", "", ACTION_APP_TIMESIG, radio_mode_am, -1, HZ(f_time),
     "WWV'S TIME FROM COLORADO: THE APP SETS THE CLOCK FROM ITS SECOND TICKS. 10 MHZ DAY AND NIGHT.", 0,
     'X', radio_agc_slow, false, "TIME SIGNAL"},
    {"BEACONS", "", ACTION_APP_PROPAGATION, radio_mode_cw, -1, HZ(f_beacon),
     "THE NCDXF BEACONS: 18 STATIONS ROUND THE WORLD IN TURN, SHOWING WHERE EACH BAND IS OPEN.", 0,
     'X', radio_agc_fast, false, "PROPAGATION"},
    {"NOAA ALERTS", "", 0, -1, -1, HZ(f_same),
     "NOAA WEATHER RADIO ON B'S SDR: THE FORECAST, AND ITS SAME ALERTS (WARNINGS) IN THE TITLE.", 0,
     'B', -1, false, "B ON THE SDR"},
    {"FM RDS", "", 0, -1, -1, HZ(f_rds),
     "AN FM STATION'S NAME, SONG AND CLOCK (RDS) ON B'S SDR. TURN B'S DIAL TO ANY STRONG STATION.", 0,
     'B', -1, false, "B ON THE SDR"},
    {"ACARS", "", 0, -1, -1, HZ(f_acars),
     "AIRLINERS' TEXT MESSAGES ON 129-132 MHZ: FLIGHT, REGISTRATION, TEXT. ALL CHANNELS AT ONCE.", 0,
     'C', -1, false, "H AIRSPACE"},
    {"ADS-B", "", 0, -1, -1, HZ(f_adsb),
     "AIRCRAFT POSITIONS, HEIGHTS AND FLIGHTS ON H AIRSPACE'S MAP: FROM AN SDR OR A GNS 5890 STICK.", 0,
     'H', -1, false, "H AIRSPACE"},
    {"DIGITAL VOICE", "", 0, -1, -1, HZ(f_dv),
     "DMR, P25, NXDN, D-STAR AND FUSION ON B'S SDR: WHO TALKS, ON WHICH TALKGROUP. ANY NARROW FM CHANNEL.", 0,
     'B', -1, false, "B ON THE SDR"},
    {"METEOR", "", 0, -1, -1, HZ(f_meteor),
     "WEATHER SATELLITES' PICTURES (LRPT) ON B'S SDR WHILE ONE PASSES: 10 MINUTES, A FEW TIMES A DAY.", 0,
     'B', -1, false, "B ON THE SDR"},
};

#define DECODERS_N N_OF(decoders)

/* The tables' rows, in the monospaced font: their heads take the same format */
#define BAND_ROW "%c%-8.8s %-11.11s %-24.24s %-5.5s %-24.24s %-13.13s %s"   /* 97 of the row's 97 at most: RX 5 wide (X6200) */
#define DEC_ROW  " %-12s %-9s %-5s %-20.20s %-13.13s %s"   /* D's band, then what OPEN sets, RX last: 70 of 72 */

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
static const char *const hear_names[] = {"X6200", "SDR", "BOTH", "OFF"};     /* the SDR's: its own name (hear_name) */

/* LISTEN's choice as said: the stick's by its kind (RTL, HACKRF, BLADERF) */
static bool hear_many(void);
/* BOTH is ALL with more than two receivers (the X6200 and two or more SDRs that tune B's channel: a KrakenSDR's): the
 * user, 2026-11-16, "B should say BOTH if only two are present, but ALL if more are" */
static const char *hear_name(int h) {
    if (h == HEAR_BOTH && hear_many()) return "ALL";
    return h == HEAR_RTL ? rtl_spectrum_dev_tag() : h >= 0 && h < HEAR_N ? hear_names[h] : "";
}
static int     heard = HEAR_X6200;
/* What is to be heard: A's or B's LISTEN, C's entry's radio, G's channel's (or what the others kept). heard is that,
 * or the X6200 standing in while it needs a stick that is not there: the stick taken as soon as it is (hear_follow) */
static int     hear_want = HEAR_X6200;
static bool    to_airspace;     /* the Sentry closing for AIRSPACE, its H (app_sentry_open_at) */
static uint32_t opened_ms;      /* the Sentry's start: the stick found in its first seconds joins without an OSD */
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
/* The scans' dwell, one for all (C's F5, SCAN CFG's DWELL): A's on each 360 kHz of its band, B's on each picture of
 * the stick's (40 rows of the waterfall at 2 s), C's on each entry of its log that is quiet. 1, 2, 4 or 8 s */
static int scan_dwell_ms = 2000;
static void dwell_next(void) {
    scan_dwell_ms = scan_dwell_ms >= 8000 ? 1000 : scan_dwell_ms * 2;
}
#define SCAN_DWELL_MS scan_dwell_ms
#define SCAN_PICK_MS  1200      /* listening: the strongest of the new picture, once its rows are in */
static void rtl_scan_set(bool on);
static bool g_stick_side(void);
static void set_mode(sysmode_t m);
static void set_routine(routine_t r);
static void a_scan_hop(uint32_t now);
static void band_key(bool up);
static void a_tuned(void);
static void b_tuned(void);
static void hit_add(int64_t hz, int mode, bool rtl, const char *what, float peak);
static void show_glog(void);
static bool tune(int32_t hz, int mode);
static void track_start(int32_t hz, bool auto_);
static void track_end(void);
static const target_t *target_near(int32_t hz, int32_t within);
static const char *const rtl_mode_names[] = {"", "NFM", "AM", "WFM"};

/* How the scans behave on a signal, A's X6200 and B's SDR alike (SCAN CFG; the user, 2026-11-17: "default algorithms
 * that mimic normal scanner behaviors hop and dwell permutations", and "as simple and idiot proof user friendly for a
 * new operator"). Three settings, and the styles that set them at once:
 *   STEP TIME         scan_dwell_ms: each step of a band (A's 360 kHz, B's picture) watched this long when nothing
 *                     stops it: 1, 2, 4 or 8 s
 *   STAY ON A SIGNAL  scan_stay: DOESN'T STOP (A only marks what it finds: SEMI-AUTO), 2 S, 15 S, 1 MIN, WHILE IT TALKS
 *                     (5 minutes at most: a carrier that never ends does not keep it for good), UNTIL YOU MOVE ON
 *                     (held: SCAN CFG's F2 GO ON or F3 SKIP)
 *   GO ON AFTER       scan_quiet: a signal quiet this long is left: 1, 3, 5 or 10 s
 * A stops on a signal by its AUTO lock (track_step), B by holding its scan on the channel (rtl_scan_step) */
enum { STY_BROWSE, STY_SCANNER, STY_SAMPLER, STY_STAY, STY_SURVEY, STY_CUSTOM, STY_N };
enum { STAY_NONE, STAY_2S, STAY_15S, STAY_1MIN, STAY_TALKS, STAY_HOLD, STAY_N };
static const uint32_t    stay_ms[STAY_N]   = {0, 2000, 15000, 60000, 300000, UINT32_MAX};
static const char *const stay_word[STAY_N] = {"DOESN'T STOP", "2 S", "15 S", "1 MIN", "WHILE IT TALKS",
                                              "UNTIL YOU MOVE ON"};
static const uint32_t    quiet_ms[]        = {1000, 3000, 5000, 10000};
static int               scan_stay = STAY_NONE, scan_quiet = 1;
static const struct {
    const char *name, *word, *about;
    int         dwell, stay, quiet;
} styles[STY_N] = {
    {"BROWSE", "BROWSE", "LOOKS AROUND AND NEVER STOPS: A MOMENT ON EACH STEP OF THE BAND, WHAT IS THERE MARKED. THE "
     "CALMEST.", 2000, STAY_NONE, 1},
    {"SCANNER", "SCANNER", "LIKE A POLICE SCANNER: STOPS ON EACH SIGNAL, STAYS WHILE IT TALKS, GOES ON 3 S AFTER IT "
     "STOPS.", 2000, STAY_TALKS, 1},
    {"SAMPLER", "SAMPLER", "STOPS ON EACH SIGNAL FOR 15 S, THEN GOES ON EVEN IF IT STILL TALKS: A TASTE OF "
     "EVERYTHING.", 2000, STAY_15S, 1},
    {"STOP & STAY", "STAY", "STOPS ON THE FIRST SIGNAL IT FINDS AND STAYS THERE TILL YOU SAY: F2 GO ON, F3 SKIP.", 2000,
     STAY_HOLD, 1},
    {"SURVEY", "SURVEY", "TOUCHES EACH SIGNAL FOR 2 S AND WRITES IT IN C LOG: A QUICK LIST OF WHAT IS ON THE AIR.", 1000,
     STAY_2S, 0},
    {"CUSTOM", "CUSTOM", "YOUR OWN MIX OF STEP TIME, STAY AND GO ON AFTER (THE ROWS UNDER IT).", 0, 0, 0},
};

/* The style the three settings make; CUSTOM when none does */
static int scan_style(void) {
    for (int k = 0; k < STY_CUSTOM; k++)
        if (styles[k].dwell == scan_dwell_ms && styles[k].stay == scan_stay &&
            (scan_stay == STAY_NONE || scan_stay == STAY_HOLD || styles[k].quiet == scan_quiet))
            return k;
    return STY_CUSTOM;
}

/* A's mode as the settings have it: AUTO to stop on signals, SEMI to only mark them (MAN-OVERRIDE, a hand on the
 * VFO, left by it too) */
static void scan_mode_sync(void) {
    set_mode(scan_stay == STAY_NONE ? MODE_SEMI : MODE_AUTO);
}

static void style_apply(int k) {
    if (k < 0 || k >= STY_CUSTOM) return;
    scan_dwell_ms = styles[k].dwell;
    scan_stay     = styles[k].stay;
    scan_quiet    = styles[k].quiet;
    scan_mode_sync();
}
static int               rtl_mode_of(int64_t hz);

/* B's channel's mode as heard: DMR, P25, D-STAR, YSF while the daemon finds it there (in NFM), else the band's */
static const char *rtl_mode_word(int64_t hz) {
    static char word[16];
    rtl_dmr_t   m;
    rtl_dsd_t   d;

    if (rtl_spectrum_dmr(&m) && m.hz == hz) return "DMR";
    if (rtl_spectrum_dsd(&d) && d.hz == hz) {
        snprintf(word, sizeof(word), "%s", rtl_dsd_name(d.proto));
        return word;
    }
    return rtl_mode_names[rtl_mode_of(hz)];
}

/* The stick's demodulator for a frequency: broadcast FM wide, the airband AM, the rest narrow FM */
static int rtl_mode_of(int64_t hz) {
    if (hz >= 87500000 && hz < 108000000) return 3;                 /* FM broadcast: wide FM */
    if ((hz >= 108000000 && hz < 137000000) || hz < 29500000) return 2;     /* the airband, and CB and HF: AM */
    return 1;                                                       /* 10 m's FM segment and up: narrow FM */
}

/* The sentry */
static scr_t     scr;
static sysmode_t smode;

/* The title bar's word for the scans: the style, MANUAL while a hand on the VFO has the X6200 */
static const char *scan_word(bool full) {
    if (smode == MODE_MAN) return full ? "MANUAL" : "MAN";
    return styles[scan_style()].word;
}
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
    int8_t  sdr;            /* the stick's: which SDR found it (sdr_code), 1 one not kept (a log of before) */
    char    what[28];       /* what it was taken for */
    time_t  first, last;
    int     times;
    float   peak;           /* the most seen: the X6200's dB over the noise, the stick's level in dB */
} hit_t;
static hit_t    hits[HITS];
static int      nhits, g_sel;
static int      g_onx = -1, g_onr = -1;     /* the entries heard now: the X6200's, the stick's (each -1 none). Both
                                             * at once with BOTH: a scan of the log on each radio */
static int64_t  g_hz;                       /* heard on the stick: the entry's, or where the VFO took it */
static bool     g_stick, hits_dirty, hits_loaded;
static uint32_t g_clear_ms;                 /* F3 pressed once: a second press within 3 s clears it */
static bool     g_scan;                     /* F4: the log's scans, one on each radio (each its own entries), with
                                             * A's and B's scans that fill it */
static int      g_pick = -1;               /* Listen on an entry two radios here take: which (F1 ALL, F2 the X6200,
                                             * F3 B's SDR, F5 BACK), -1 none asked */
static int      g_hear = -1;                /* the LISTEN in force when C began playing entries (BOTH keeps the other
                                             * radio heard meanwhile), put back when it stops; -1 none */
static uint32_t g_nx, g_nr;                 /* when each radio's log scan moves on */
static int     visited_i;
static int32_t ignored[IGNORED];
static int     n_ignored;
static int     band_sel, dec_sel, dec_row;
static int32_t last_tap_hz;
static uint32_t last_tap_ms;
static uint32_t tap_pause;              /* a tap on A's picture: AUTO's locks wait till then */

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
static lv_obj_t *ab_arrow, *ab_tag;     /* PTT refused: where the A/B key is (landscape) */
extern const lv_font_t power_sym_14;    /* the clock's: Montserrat's battery and bolt, and a plug (clock.c) */
#define SYMBOL_PLUG "\xEF\x87\xA6"     /* U+F1E6 */
typedef struct { lv_obj_t *frame, *cap, *line, *opt[12]; int n; } box_t;     /* opt: E SYSTEM's 9 rows the most */
static lv_obj_t *tick_l[TICKS], *tgt_l[LABELS];
/* SCAN CFG: its rows (the MFK goes through them, its push or a tap changes the one chosen), what the chosen one does,
 * and what each radio's scan does now */
enum { CR_STYLE, CR_STEP, CR_STAY, CR_QUIET, CR_SWEEP, CR_ASTEP, CR_BSTEP, CR_BGAIN, CR_BSQL, CR_N };
static lv_obj_t *cr_row_l[CR_N], *cr_val_l[CR_N], *cr_about_l, *cr_now_l;
static int       cr_sel;
static void      show_track(void);
static void      cr_change(int r);
static void      cr_hold(void);
static void      cr_skip(void);
static lv_obj_t *bd_head_l, *bd_row_l[20], *bd_about_l;
static int       bd_rows;
static lv_obj_t *lg_head_l, *lg_row_l[20], *lg_rx_l[20], *lg_about_l;   /* lg_rx_l: a row's RX, in pixel_12 */
static int       lg_rows;
static box_t     d_sel, d_how, d_ant, d_now;
static lv_obj_t *d_head_l, *d_row_l[12];

/* E SYSTEM: the rest of the radio, from here */
typedef enum {
    SYS_SETTINGS, SYS_APPS, SYS_MODES, SYS_AIRSPACE, SYS_VHF, SYS_POOL, SYS_CLASSIC, SYS_SHELL, SYS_BASEFW, SYS_HOME,
    SYS_N
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
    {"AIRSPACE (ADS-B)", "AIRCRAFT ON 1090 MHZ FROM AN ADS-B RECEIVER ON USB (RTL-SDR, HACKRF, BLADERF, GNS 5890 OR "
                         "MICROADSB): A SCOPE AROUND YOUR QTH, THEIR CALLS, HEIGHTS AND TRACKS, AND THE HISTORY FADING "
                         "OUT. A GNS 5890 OR MICROADSB KEEPS LISTENING WHEN CLOSED; AN SDR IS ITS ONLY WHILE IT IS "
                         "OPEN."},
    {"VHF/UHF (SDR)", "B VHF/UHF: AN SDR'S SPECTRUM (AN RTL-SDR 24 MHZ TO 1.7 GHZ, A HACKRF 1 MHZ TO 6 GHZ, A BLADERF), "
                      "ALONE, SPLIT WITH THE X6200'S OR THE X6200'S ALONE (F3 VIEW), A'S KEYS: F1 SCANS THE BAND, F2 "
                      "CHOOSES IT, F4 LISTEN (X6200, THE SDR, BOTH OR OFF), F5 SCAN CFG. THE VFO TUNES ITS CHANNEL, THE "
                      "MFK PANS. AIRSPACE TAKES A SPARE SDR, ELSE B'S WHILE IT IS OPEN. A HACKRF OR BLADERF ALSO "
                      "TRANSMITS ON G."},
    {"SDR POOL", "EVERY SDR ON USB (EACH RTL BY ITS NUMBER, A HACKRF, A BLADERF), WHOSE IT IS (B, AIRSPACE, A WATCH "
                 "CHANNEL, FREE) AND WHETHER IT IS HEARD: F1 USES ONE ON B, F2 STOPS OR STARTS IT, F3 HEARS OR MUTES IT, "
                 "F4 WATCHES B'S CHANNEL. APP HELD OPENS IT FROM ANY SCREEN."},
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
static box_t     s_list, s_what, s_look, s_home, s_keys, s_load;
static lv_obj_t *key_l[5];

/* The knobs and keys, said: the legend, a word on screen for a moment (OSD), the list of them all (help) */
typedef enum { VF_VOL, VF_SQL, VF_RFG } volfn_t;
static volfn_t     vol_fn;
/* The VOL knob is the volume again VOL_BACK_MS after its last turn or push (vol_back): the user, 2026-11-16, "ensure
 * the volume squeltch RF gain button always reverts back to volume for its next use requiring presses to change to
 * the others ... when I'm just trying to change the volume and expect it to be the default behavior" */
#define VOL_BACK_MS 5000
static uint32_t    vol_used_ms;
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

/* A's band now: the one the dial is in, or the one chosen last (BAND, D) while the dial is at its listening place:
 * 30 M and 19 M BC listen to WWV, 10 and 15 MHz, just outside them, and BAND stuck there (each press found no band,
 * took the next one up and came back to WWV), A's F1 there had "NO BAND HERE" (2026-11-14) */
static int a_band_now(int64_t hz) {
    int i = band_of(hz);

    if (i < 0 && band_sel >= 0 && band_sel < BANDS_N && llabs(hz - bands[band_sel].listen_hz) < 5000) i = band_sel;
    return i;
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
    {15000000, "WWV/WWVH 15 MHZ"}, {20000000, "WWV 20 MHZ"}, {25000000, "WWV 25 MHZ"}};
/* CHU (3.330, 7.850, 14.670 MHz) is not among them: Canada's NRC closed it for good on 22 June 2026, and what is
 * heard there now is something else */

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
    return float_kth(v, n, n / 2);  /* kth.h */
}

/* v into the sorted s (n of them) */
static void sorted_in(float *s, int *n, float v) {
    int lo = 0, hi = *n;

    while (lo < hi) {
        int m = (lo + hi) / 2;

        if (s[m] < v) lo = m + 1;
        else hi = m;
    }
    memmove(s + lo + 1, s + lo, (size_t)(*n - lo) * sizeof(float));
    s[lo] = v;
    (*n)++;
}

/* v out of the sorted s: false when it is not there (a NaN) */
static bool sorted_out(float *s, int *n, float v) {
    int lo = 0, hi = *n;

    while (lo < hi) {
        int m = (lo + hi) / 2;

        if (s[m] < v) lo = m + 1;
        else hi = m;
    }
    if (lo == *n || s[lo] != v) return false;
    memmove(s + lo, s + lo + 1, (size_t)(*n - lo - 1) * sizeof(float));
    (*n)--;
    return true;
}

/* The noise around each bin: the median of the bins +-FLOOR_HALF. FM broadcast (anything 200 kHz wide) would be
 * its own floor there: then the floor is the picture's, the targets 100 kHz apart and placed at their middle. The
 * window kept sorted as it slides, a bin out and a bin in, each a short move: the same medians as each window's own
 * selection gave, which was 4% of G's GUI thread at every frame of the X6200's spectrum (perf, 2026-11-17) */
static void find_floor(void) {
    float w[2 * FLOOR_HALF + 2], all[MAX_BINS];
    int   wn = 0;

    broad = centre > 87000000 && centre < 108500000;
    for (int i = 0; i <= FLOOR_HALF && i < nbins && !broad; i++) sorted_in(w, &wn, avg_db[i]);
    for (int i = 0; i < nbins && !broad; i++) {
        int out = i - FLOOR_HALF, in = i + FLOOR_HALF + 1;

        floor_db[i] = w[wn / 2];        /* the window lo..hi, wn = hi - lo + 1: as median() of it */
        if (out >= 0 && !sorted_out(w, &wn, avg_db[out])) {
            /* not found (a NaN in the levels): the next window gathered again */
            int lo = out + 1, hi = in < nbins ? in - 1 : nbins - 1;

            wn = 0;
            for (int k = lo; k <= hi; k++) sorted_in(w, &wn, avg_db[k]);
        }
        if (in < nbins) sorted_in(w, &wn, avg_db[in]);
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

/* The battery's icon after its %: the bolt charging, the plug on a supply not charging, its level on the battery */
static const char *batt_sym(float ext, uint8_t cap, bool chg) {
    return chg && cap < 100 ? LV_SYMBOL_CHARGE : ext >= 3 ? SYMBOL_PLUG
         : cap > 80 ? LV_SYMBOL_BATTERY_FULL : cap > 55 ? LV_SYMBOL_BATTERY_3 : cap > 30 ? LV_SYMBOL_BATTERY_2
         : cap > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
}

/* The title bars' tags, after the battery, in the tags' colour: the devices on USB (the stick, an ADS-B receiver, the
 * WR12), then who works the radio from afar: WEB while a browser is on the radio's Remote page (remote_server.c),
 * WEB CTL while one holds the control (control_owner.c: the radio's own keys and knobs take it back), TCI CTL a TCI
 * program holding it. The user, 2026-11-16: "when web is connected, we know via the icon. that will help with doubts
 * about the web interface locking a control" */
static void fmt_mhz(char *s, size_t n, int64_t hz, int dec);

static void dev_tags(char *dev, size_t n) {
    ctl_info_t c;

    dev[0] = 0;
    if (sdr_here) snprintf(dev + strlen(dev), n - strlen(dev), "  #%s %s#", tag_hex, rtl_spectrum_dev_tag());
    if (adsb_here) snprintf(dev + strlen(dev), n - strlen(dev), "  #%s ADS-B#", tag_hex);
    if (wr12_here) snprintf(dev + strlen(dev), n - strlen(dev), "  #%s WR12#", tag_hex);
    ctl_get(&c);
    if (c.kind == CTL_BROWSER) snprintf(dev + strlen(dev), n - strlen(dev), "  #%s WEB CTL#", tag_hex);
    else if (remote_server_viewers() > 0) snprintf(dev + strlen(dev), n - strlen(dev), "  #%s WEB#", tag_hex);
    if (c.kind == CTL_TCI) snprintf(dev + strlen(dev), n - strlen(dev), "  #%s TCI CTL#", tag_hex);
    for (int i = 0; i < rtl_watch_count(); i++) {   /* a watch channel heard (rtl_watch.c): on every screen */
        rtl_watch_info_t w;
        char             f[16];

        if (rtl_watch_get(i, &w) && w.open) {
            fmt_mhz(f, sizeof(f), w.hz, 3);
            snprintf(dev + strlen(dev), n - strlen(dev), "  #%s W %s#", tag_hex, f);
            break;
        }
    }
}

/* The title bar's line as every screen has it (pre: up to the battery's %, then room for its icon and the devices):
 * the screen's radio's frequency and mode, the transmitter's lock, the Sentry's mode, the time, the processor, the
 * battery and the tags (dev_tags). Compact, for a line too long for its bar: the time without its zone, the Sentry's
 * mode in a word (AUTO, SEMI, MAN) */
static bool head_compact;

static void head_status(char *pre, size_t pn, char *line, size_t ln, const char **icon_out, uint32_t *bc_out) {
    char       f[24], t[16], tc[32] = "";
    time_t     now = time(NULL);
    struct tm  tm;
    int32_t    fq  = cparam_i_get(cfg_fg_freq);
    int        c   = clock_cpu_temp();  /* the processor's, the kernel's cpu-thermal zone (sun4i-gpadc-iio) */

    localtime_r(&now, &tm);             /* the radio's time zone (Settings > General > DATE / TIME), named */
    strftime(t, sizeof(t), head_compact ? "%H:%M" : "%H:%M %Z", &tm);
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
        icon = batt_sym(l_ext, l_cap, l_chg);
    }
    snprintf(f, sizeof(f), "%d.%03d.%02d", fq / 1000000, (fq / 1000) % 1000, (fq % 1000) / 10);
    const char *mw = scan_word(!head_compact);     /* the scans' style (SCAN CFG), MANUAL with a hand on the VFO */

    char dev[128];                      /* at the end, in the tags' colour */
    dev_tags(dev, sizeof(dev));

    if (scr == SCR_RTL || g_stick_side()) {     /* the screen's radio: the stick */
        int64_t hz = scr == SCR_LOG ? g_hz : listen_hz;

        snprintf(f, sizeof(f), "%s %lld.%03lld.%02lld", rtl_spectrum_dev_tag(), (long long)(hz / 1000000),
                 (long long)(hz / 1000 % 1000), (long long)(hz % 1000 / 10));
        snprintf(pre, pn, "%s %s  %s  %s  %s%s", f, rtl_mode_word(hz),
                 rx_held ? "RX ONLY" : "TX NOT LOCKED", mw, t, tc);
    } else {
        snprintf(pre, pn, "%s %s%s%s  %s  %s  %s%s", f, mode_name(cparam_i_get(cfg_cur_mode)),
                 cparam_i_get(cfg_cur_pre) ? " PRE" : "", cparam_i_get(cfg_cur_att) ? " ATT" : "",
                 rx_held ? "RX ONLY" : "TX NOT LOCKED", mw, t, tc);
    }
    /* the line, room for the icon after the %, then the devices */
    snprintf(line, ln, "%s%s%s", pre, icon[0] ? "   " : "", dev);
    *icon_out = icon;
    *bc_out   = bc;
}

/* The head's line within w px of pixel_16: compact (head_compact) when the full one is wider */
static void head_fit(char *pre, size_t pn, char *line, size_t ln, const char **icon, uint32_t *bc, lv_coord_t w) {
    head_compact = false;
    head_status(pre, pn, line, ln, icon, bc);
    if (lv_txt_get_width(line, strlen(line), &pixel_16, 0, LV_TEXT_FLAG_RECOLOR) <= w) return;
    head_compact = true;
    head_status(pre, pn, line, ln, icon, bc);
    head_compact = false;
}

static void show_head(void) {
    char        pre[160], line[240];
    const char *icon;
    uint32_t    bc;

    head_fit(pre, sizeof(pre), line, sizeof(line), &icon, &bc, SW - 120);
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

static void show_walkie(void);
static void walkie_step(void);
static void walkie_ch(int dir, int to);
static void walkie_arrow(int dir);      /* G's arrows: a channel, or a step of the dial */
static void walkie_show(bool on);
static bool g_portrait;         /* G held up: its own portrait screen shows (walkie_show) */
static bool g_armed;            /* G's transmitter armed (the A/B key): the Sentry's receive-only hold let go */
/* G on the SDR's channels (a HackRF or a bladeRF on the HOST port: they transmit): its own channel there, its own
 * transmitter. The X6200 stays held at 0 W (receive only) all the while */
static int64_t w_sdr_hz;        /* G's channel on the SDR; 0: G is the X6200's */
static bool    g_sdr_armed;     /* its transmitter armed (A/B on one of its channels) */
static bool    w_sdr_ptt;       /* transmitting for G now (asked of the daemon, or on the air) */
static uint8_t w_sdr_by;        /* what keyed it: 1 the top PTT key, 2 the hand mic's PTT (the base keyed, at 0 W) */
static int     w_sdr_gain = 10; /* its TX gain, dB: low (a HackRF's 0-47; a bladeRF's over its least) */
static bool    w_sdr_amp;       /* a HackRF's amplifier: off, unless chosen (never kept across a restart) */
static int64_t w_sdr_resume;    /* G's SDR channel before a restart: gone back to once the SDR says it is there */
static void    w_sdr_left(void);
static void    w_sdr_arm(bool on);
static void    w_sdr_key(bool down, uint8_t by);
static void    w_sdr_turn(int32_t diff);
static void    w_sdr_gain_next(void);
static void    w_sdr_step(void);
static int  w_at = -1;          /* G's channel (w_chs[]), -1 none chosen yet */
static void walkie_group(int dir);
static void walkie_enter(void);
static void walkie_key_label(int k, char *s, size_t n);
static int64_t w_hz(void);      /* G's frequency: the X6200's dial */
static int  w_match(void);      /* the plan's channel the dial is on, -1 none */
static bool w_chan(int i);      /* channel i one of a service's defined channels (CB) */
static bool w_cb_here(void);    /* the dial in CB's span: the arrows step its channels */
static void walkie_group_mode(void);
static void x62_by_hand(void);
static bool dial_held(void);    /* TX unlocked or on the air: the tuning knobs held, and said so */
static void w_say(const char *portrait, const char *title, const char *text);  /* G's message line, or the OSD */
static void ws_view_next(void);
static void ws_feed(void);      /* G's picture, a frame of the X6200's spectrum */
static void g_gpu_draw(void);   /* G's two pictures by the GPU, when something of them changed */
static bool       g_gpu;            /* G held up: its spectrum and voiceprint drawn by the GPU (g_gpu_draw) */
static bool       g_dirty = true;   /* and something of them changed since they were drawn */
static int        vp_head;          /* the GPU's voiceprint: its oldest column in the ring (the next written) */
static void walkie_arm(bool on);

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

/* NOW as a colour, for C's BEST (the owner: "indicate availability with a green / red instead of a NOW column", "add
 * a yellow for any band that is about to close too!"): its theme role, green open now (signals heard there lately, an
 * always-open or local band, a day band by day, a night band by night, a band opening at dawn or dusk), yellow about
 * to close (CLOSING: within 6 degrees of sunset for a day band, of sunrise for a night band), red closed (a day band
 * by night); -1 none, the row's amber: a night band by day (short range only), no QTH */
static int band_now_role(int i) {
    char s[16];

    band_now(i, s, sizeof(s));
    if (strstr(s, "SIG") || !strcmp(s, "OPEN") || !strcmp(s, "OPENING") || (!strcmp(s, "LOCAL") && bands[i].when == 'L'))
        return TC_avail_on;
    if (!strcmp(s, "CLOSING")) return TC_avail_soon;
    return strcmp(s, "CLOSED") ? -1 : TC_avail_off;
}

/* A frequency's band in D: the one it is in, or the one listening there (WWV's 10 and 15 MHz, just outside 30 M and
 * 19 M BC); -1 none (WWV's 2.5, 20 and 25 MHz, DSC's 2187.5 kHz) */
static int band_for(int64_t hz) {
    int i = band_of(hz);

    for (int k = 0; i < 0 && k < BANDS_N; k++)
        if (llabs(hz - bands[k].listen_hz) < 5000) i = k;
    return i;
}

/* The radio that receives D's band i: its RX column, and what LISTEN opens */
static int band_rx(int i) {
    return band_uses_rtl(i) ? RX_SDR : cfg_is_valid_hw_freq(bands[i].listen_hz) ? RX_X6200 : RX_NONE;
}

/* D's last line: the radio that receives the band and what LISTEN sets on it, who may send there, and E's types in
 * it (the two screens in sync: E's rows name D's bands) */
static void band_more(int i, char *s, size_t n) {
    const band_t *b  = &bands[i];
    rxset_t       st = rx_setup(b->listen_hz, radio_agc_auto);
    char          set[32], tx[32], e[96] = "";

    if (b->listen_hz == ADSB_HZ) st.gain = -3;
    size_t        k = 0;
    int           rx = band_rx(i);

    rx_setup_text(&st, set, sizeof(set));
    tx_text(b->listen_hz, true, tx, sizeof(tx));
    for (int d = 0; d < DECODERS_N; d++)
        for (int j = 0; j < decoders[d].n; j++) {
            if (band_for(decoders[d].hz[j]) != i) continue;
            if (k + strlen(decoders[d].name) + 2 < sizeof(e)) k += snprintf(e + k, sizeof(e) - k, " %s", decoders[d].name);
            break;
        }
    if (rx == RX_NONE)
        snprintf(s, n, "NO RADIO HERE TUNES IT%s%s", e[0] ? " | E:" : "", e);
    else
        snprintf(s, n, "RX %s, %s | %s%s%s", rx == RX_SDR ? rtl_spectrum_dev_tag() : "X6200", set, tx,
                 e[0] ? " | E:" : "", e);
}

static void show_bands(void) {
    char s[400], now[16];
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
        char rx[24];                    /* which radio takes it: the stick (in the tags' orange, but in the row's ink
                                         * on the chosen row: orange on amber hardly showed) or the X6200 */
        if (band_uses_rtl(i)) {
            if (sdr_here && i != band_sel) snprintf(rx, sizeof(rx), "#%s %s#", tag_hex, rtl_spectrum_dev_tag());
            else snprintf(rx, sizeof(rx), "%s", rtl_spectrum_dev_tag());
        }
        else snprintf(rx, sizeof(rx), "%s", cfg_is_valid_hw_freq(bands[i].listen_hz) ? "X6200" : "");
        snprintf(s, sizeof(s), BAND_ROW, band_rt[i].sweep ? '*' : ' ',
                 bands[i].name, mhz, bands[i].hear, best_name(bands[i].when), bands[i].ant, now,
                 rx);
        lv_label_set_text(bd_row_l[r], s);
        lv_obj_set_style_bg_opa(bd_row_l[r], i == band_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(bd_row_l[r], i == band_sel ? ink : amb, 0);
    }
    {
        char more[200];

        band_more(band_sel, more, sizeof(more));
        snprintf(s, sizeof(s), "%s: %s\n%s", bands[band_sel].name, bands[band_sel].about, more);
    }
    for (char *c = s; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    lv_label_set_text(bd_about_l, s);
}

/* E's TYPE list: its rows shown, the chosen one kept in the middle once the list is longer than the box */
static int dec_top(void) {
    int top = dec_sel - d_sel.n / 2;

    if (top > DECODERS_N - d_sel.n) top = DECODERS_N - d_sel.n;
    return top < 0 ? 0 : top;
}

/* The radio a type is received on, as its RX column says it: the X6200's apps, else the SDR's (by its name, "SDR"
 * with none plugged in; ADS-B's GNS 5890 stick when only it is there) */
static const char *dec_rx(const decoder_t *d, int32_t f) {
    if (d->rx == 'X') return cfg_is_valid_hw_freq(f) ? "X6200" : "";
    if (d->rx == 'H' && !sdr_here && adsb_here) return "STICK";
    return rtl_spectrum_dev_tag();
}

static void show_decode(void) {
    const decoder_t *d = &decoders[dec_sel];
    char             s[128], now[16];
    int              top = dec_top();

    for (int r = 0; r < d_sel.n; r++)
        opt_set(&d_sel, r, top + r < DECODERS_N ? decoders[top + r].name : "", top + r == dec_sel);
    for (int r = 0; r < 12; r++) {
        if (r >= d->n) {
            lv_label_set_text(d_row_l[r], "");
            lv_obj_set_style_bg_opa(d_row_l[r], LV_OPA_TRANSP, 0);
            continue;
        }
        int32_t f  = d->hz[r];
        int     bi = band_for(f);
        char    fs[16], set[32];
        rxset_t st = rx_setup_on(f, d->agc, d->rx != 'X');

        if (f < 1000000)
            snprintf(fs, sizeof(fs), "%d.%d KHZ", f / 1000, (f % 1000) / 100);
        else
            snprintf(fs, sizeof(fs), "%d.%04d", f / 1000000, (f % 1000000) / 100);
        if (bi >= 0) band_now(bi, now, sizeof(now));
        else now[0] = 0;                /* WWV's 2.5, 20 and 25 MHz, DSC's 2187.5: in none of D's bands */
        if (d->rx == 'H') st.gain = -3;     /* dump1090's gain, or the stick's */
        rx_setup_text(&st, set, sizeof(set));
        const char *who = dec_rx(d, f);
        char        rx[24];             /* the radio it is heard on: the X6200 (its apps), else the SDR, in the tags'
                                         * orange but on the chosen row */
        if (d->rx == 'X' || r == dec_row) snprintf(rx, sizeof(rx), "%s", who);
        else snprintf(rx, sizeof(rx), "#%s %s#", tag_hex, who);
        snprintf(s, sizeof(s), DEC_ROW, fs, bi >= 0 ? bands[bi].name : "", bi >= 0 ? best_name(bands[bi].when) : "",
                 set, now, rx);
        lv_label_set_text(d_row_l[r], s);
        lv_obj_set_style_bg_opa(d_row_l[r], r == dec_row ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(d_row_l[r], r == dec_row ? ink : amb, 0);
    }
    opt_set(&d_how, 0, d->about, 0);
    opt_set(&d_ant, 0, d->opens, 0);
    {
        int32_t f = dec_row < d->n ? d->hz[dec_row] : 0;
        char    tx[32];

        tx_text(f, d->two_way, tx, sizeof(tx));
        snprintf(s, sizeof(s), "%s: %s", dec_rx(d, f), strcmp(tx, "RX ONLY") ? "RX AND TX" : "RX ONLY");
        opt_set(&d_ant, 1, s, 0);
        int bi = f ? band_for(f) : -1;
        opt_set(&d_now, 0, bi >= 0 ? bands[bi].ant : "", 0);
        opt_set(&d_now, 1, bi >= 0 ? bands[bi].name : "", 2);
    }
}
/* F SYSTEM's load (the user: "on F the SYSTEM screen... add a CPU, GPU, and RAM display"): the processor's four cores
 * (/proc/stat, its clock from cpufreq), the Mali-400 and the memory (MemAvailable), each a second, a bar in the
 * spectrum's ramp beside each. The kernel gives no busy figure for the Mali-400, only the clock its governor sets from
 * its load (devfreq): its % is where the last second's clock stood between its lowest and highest, 0 idle */
#define MB_H   10                       /* the meter bars: G's and these */
#define MB_SEG 6                        /* a segment and its gap */
static lv_img_dsc_t *ld_frame[3];
static lv_obj_t     *ld_img[3], *ld_val[3], *ld_more[3];
static int           ld_w;
static void          mbar_draw(lv_img_dsc_t *f, int w, float level);

#define GPU_DEVFREQ "/sys/class/devfreq/1c40000.gpu"

static void sys_load_show(void) {
    static unsigned long long prev_t[5], prev_i[5];
    static unsigned long long gpu_prev[8];
    static unsigned long      gpu_hz[8];
    static int                ngpu;
    char                      line[256], t[48];
    float                     pct[5] = {0};
    FILE                     *f;

    if (!ld_val[0]) return;
    if ((f = fopen("/proc/stat", "r"))) {           /* cpu, cpu0..cpu3: user nice system idle iowait irq softirq steal */
        for (int k = 0; k < 5 && fgets(line, sizeof(line), f); k++) {
            unsigned long long v[8] = {0}, tot = 0;
            char               name[8];

            if (sscanf(line, "%7s %llu %llu %llu %llu %llu %llu %llu %llu", name, &v[0], &v[1], &v[2], &v[3], &v[4],
                       &v[5], &v[6], &v[7]) < 5 || strncmp(name, "cpu", 3))
                break;
            for (int j = 0; j < 8; j++) tot += v[j];
            unsigned long long idle = v[3] + v[4], dt = tot - prev_t[k], di = idle - prev_i[k];

            pct[k]    = prev_t[k] && dt ? 100.0f * (float)(dt - di) / (float)dt : 0;
            prev_t[k] = tot;
            prev_i[k] = idle;
        }
        fclose(f);
    }
    int mhz = 0;
    if ((f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r"))) {
        if (fscanf(f, "%d", &mhz) == 1) mhz /= 1000;
        fclose(f);
    }
    snprintf(t, sizeof(t), "CPU %3.0f%%", pct[0]);
    lv_label_set_text(ld_val[0], t);
    snprintf(t, sizeof(t), "%.0f %.0f %.0f %.0f%% %.1f GHZ", pct[1], pct[2], pct[3], pct[4], mhz / 1000.0);
    lv_label_set_text(ld_more[0], t);
    mbar_draw(ld_frame[0], ld_w, pct[0] / 100);

    /* the GPU: each clock's time (ms, the line's last number) in trans_stat, against the last second's */
    float         gl  = -1;
    unsigned long cur = 0, top = 0, low = 0;
    if ((f = fopen(GPU_DEVFREQ "/trans_stat", "r"))) {
        unsigned long long now[8], dsum = 0, wsum = 0;
        int                n = 0;

        while (fgets(line, sizeof(line), f) && n < 8) {
            char *p = line;
            while (*p == ' ' || *p == '*') p++;
            char         *e;
            unsigned long hz = strtoul(p, &e, 10);
            if (!hz || *e != ':') continue;
            char *last = NULL, *q = e + 1;
            for (char *tok = strtok(q, " \t\n"); tok; tok = strtok(NULL, " \t\n")) last = tok;
            if (!last) continue;
            gpu_hz[n] = hz;
            now[n]    = strtoull(last, NULL, 10);
            if (hz > top) top = hz;
            if (!low || hz < low) low = hz;
            n++;
        }
        fclose(f);
        if (n == ngpu)
            for (int i = 0; i < n; i++) {
                unsigned long long d = now[i] - gpu_prev[i];
                dsum += d;
                wsum += d * (gpu_hz[i] / 1000000);
            }
        if (n == ngpu && dsum && top > low)
            gl = ((float)wsum / (float)dsum - low / 1000000.0f) / ((top - low) / 1000000.0f);
        memcpy(gpu_prev, now, sizeof(now[0]) * (size_t)n);
        ngpu = n;
    }
    if ((f = fopen(GPU_DEVFREQ "/cur_freq", "r"))) {
        if (fscanf(f, "%lu", &cur) != 1) cur = 0;
        fclose(f);
    }
    if (gl < 0 && cur && top > low) gl = (float)(cur - low) / (float)(top - low);
    snprintf(t, sizeof(t), gl >= 0 ? "GPU %3.0f%%" : "GPU   -", gl * 100);
    lv_label_set_text(ld_val[1], t);
    snprintf(t, sizeof(t), "MALI %lu/%lu MHZ", cur / 1000000, top / 1000000);
    lv_label_set_text(ld_more[1], cur ? t : "NO GPU CLOCK");
    mbar_draw(ld_frame[1], ld_w, gl > 0 ? gl : 0);

    unsigned long mt = 0, ma = 0;
    if ((f = fopen("/proc/meminfo", "r"))) {
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "MemTotal: %lu", &mt);
            sscanf(line, "MemAvailable: %lu", &ma);
        }
        fclose(f);
    }
    float used = mt ? (float)(mt - ma) / mt : 0;
    snprintf(t, sizeof(t), "RAM %3.0f%%", used * 100);
    lv_label_set_text(ld_val[2], t);
    snprintf(t, sizeof(t), "%lu OF %lu MB", (mt - ma) / 1024, mt / 1024);
    lv_label_set_text(ld_more[2], t);
    mbar_draw(ld_frame[2], ld_w, used);
    for (int i = 0; i < 3; i++) lv_obj_invalidate(ld_img[i]);
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
    opt_set(&s_keys, 1, "F2 OR PRESS: OPEN", 2);
    sys_load_show();
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
    int64_t     hz;
    int64_t     lo, hi;         /* the band F1 (SCAN) sweeps, end to end */
    const char *about;
} rtl_presets[] = {           /* those past what the stick tunes are passed over (rtl_preset_ok): the last ones and the
                               * HF ones a HackRF's */
    {"NOAA WX", 162475000, 162400000, 162550000, "WEATHER RADIO 162.400-.550, AIS 161.975 / 162.025"},
    {"2 M", 145000000, 144000000, 148000000, "HAMS 144-148: APRS 144.390, THE ISS 145.800"},
    {"2 M RPT", 147000000, 146000000, 148000000, "HAMS 146-148: REPEATERS AND SIMPLEX"},
    {"AIRBAND", 121000000, 118000000, 137000000, "AIRCRAFT VOICE (AM) 118-137, GUARD 121.500"},
    {"FM BC", 98000000, 88000000, 108000000, "FM BROADCAST 88-108 MHZ"},
    {"MARINE", 156800000, 156000000, 162025000, "MARINE VHF, CALLING CHANNEL 16 156.800"},
    {"WX SAT", 137500000, 137000000, 138000000, "METEOR-M WEATHER SATELLITES 137.100 / 137.900"},
    {"SONDES", 403000000, 400000000, 406000000, "WEATHER BALLOONS 400-406 MHZ"},
    {"70 CM", 446000000, 420000000, 450000000, "HAMS 420-450: SIMPLEX 446.000"},
    /* DMR (the daemon's dmr.c finds it in any narrow FM channel): the ham simplex channels, and the business and public
     * service bands where most of it is (the user, 2026-11-16: "ensure the band selection can tune in") */
    {"DMR 70 CM", 441000000, 440000000, 450000000, "HAM DMR: SIMPLEX 441.000 (CALLING), 446.500, 446.075"},
    {"UHF LMR", 460000000, 450000000, 470000000, "BUSINESS AND PUBLIC SERVICE RADIO 450-470 (MUCH OF IT DMR)"},
    {"VHF LMR", 152000000, 150000000, 174000000, "BUSINESS AND PUBLIC SERVICE RADIO 150-174 (DMR AND FM)"},
    {"ISM 433", 433920000, 433050000, 434790000, "SENSORS, KEY FOBS, WEATHER STATIONS"},
    {"GMRS", 462600000, 462550000, 467725000, "GMRS AND FRS RADIOS"},
    {"ISM 915", 915000000, 902000000, 928000000, "METERS AND SENSORS 902-928 MHZ"},
    {"UAT 978", 978000000, 977000000, 979000000, "AIRCRAFT UAT, 978 MHZ"},
    {"ADS-B", 1090000000, 1089000000, 1091000000, "AIRCRAFT, 1090 MHZ (AIRSPACE DECODES THEM)"},
    {"6 M", 52525000, 50000000, 54000000, "HAMS 50-54: FM CALLING 52.525"},
    {"1.25 M", 223500000, 222000000, 225000000, "HAMS 222-225: FM CALLING 223.500"},
    {"23 CM", 1294500000, 1240000000, 1300000000, "HAMS 1240-1300: FM CALLING 1294.500"},
    {"GPS L1", 1575420000, 1574420000, 1576420000, "GPS L1 1575.42 (A HUMP IN THE NOISE)"},
    {"WIFI 2.4", 2437000000LL, 2400000000LL, 2483500000LL, "WI-FI AND BLUETOOTH 2.4 GHZ (HACKRF, BLADERF)"},
    {"13 CM", 2304000000LL, 2300000000LL, 2450000000LL, "HAMS 2300-2450 (HACKRF, BLADERF)"},
    {"5.8 GHZ", 5800000000LL, 5650000000LL, 5925000000LL, "FPV VIDEO, HAMS 5650-5925 (HACKRF, BLADERF)"},
    {"HF 20 M", 14200000, 14000000, 14350000, "HAMS 14.000-14.350 (A HACKRF; THE X6200 HEARS IT BETTER)"},
    {"HF 10 M", 28400000, 28000000, 29700000, "HAMS 28.000-29.700"},
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
static lv_obj_t     *rds_l;            /* B's channel's RDS, at the top of the stick's history */
static lv_obj_t     *watch_l;          /* B's watch channels (rtl_watch.c), at the bottom of it */
static void          watch_show(void);
static bool          rds_tuned_note(char *t, size_t n);
static lv_obj_t     *hf_peak_l[RTL_PEAKS];     /* the X6200's half: A's signals, named */
static box_t         r_dev, r_tune, r_pre, r_sig;
static lv_timer_t   *rtl_timer;
static int           rtl_img_h, rtl_preset, rtl_step = 2, rtl_drop;
static bool          f2_roll, f2_rolled;    /* B's F2 held: the VFO knob rolls the presets (rotary_cb); turned so */
static bool          rtl_direct;    /* the picture turned straight onto the display's primary plane (rtl_present) */
static bool          rtl_gpu;       /* and drawn there by the GPU (gpu.c): the CPU fills no pixels of it */
static int           hf_bx = -1, rtl_bx = -1;  /* the GPU's brackets this frame: their columns, -1 none */
static void          rtl_gpu_draw(const lv_area_t *a);

/* The picture as drawn into rtl_frame shown: turned straight onto the display's primary plane under the GUI's hole
 * (one pass, no copy through LVGL), only while A or B shows it (elsewhere the plane is under another screen); without
 * the planes, LVGL draws it again */
static void rtl_present(void) {
    if (!rtl_img) return;
#if USE_DRM
    if (rtl_direct) {
        lv_area_t a;

        if (scr != SCR_SCAN && scr != SCR_RTL) return;
        lv_obj_update_layout(rtl_img);  /* its place, laid out (nothing done when it is) */
        lv_obj_get_coords(rtl_img, &a);
        if (rtl_gpu) rtl_gpu_draw(&a);
        else drm_wf_rotate((const uint32_t *)rtl_frame->data, a.x1, a.y1, RTL_COLS, rtl_img_h);
        return;
    }
#endif
    lv_obj_invalidate(rtl_img);
}

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
static void to_cols_n(const float *db, int n, double lo_hz, double bin_hz, int64_t view_lo, double px_hz, float *cols,
                      int ncols) {
    for (int x = 0; x < ncols; x++) {
        double f0 = view_lo + x * px_hz, f1 = f0 + px_hz;
        int    b0 = (int)floor((f0 - lo_hz) / bin_hz), b1 = (int)ceil((f1 - lo_hz) / bin_hz);
        float  best = -300;

        if (b1 <= b0) b1 = b0 + 1;
        for (int b = b0; b < b1; b++)
            if (b >= 0 && b < n && db[b] > best) best = db[b];
        cols[x] = best > -300 ? best : (x ? cols[x - 1] : -150);
    }
}

static void to_cols(const float *db, int n, double lo_hz, double bin_hz, int64_t view_lo, double px_hz, float *cols) {
    to_cols_n(db, n, lo_hz, bin_hz, view_lo, px_hz, cols, RTL_COLS);
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

    if (rtl_gpu) {      /* the GPU draws it (rtl_gpu_draw): per column its line's row, the span to the column before
                         * and the level, in 0..255 of the trace's height */
        static uint8_t tr[RTL_COLS][4];
        int            hm = h > 1 ? h - 1 : 1;

        for (int x = 0; x < RTL_COLS; x++) {
            int y = h - 1 - (int)((cols[x] - p->bot_db) / (p->top_db - p->bot_db) * (h - 1));
            y     = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
            int y0 = prev < 0 ? y : prev < y ? prev : y, y1 = prev < 0 ? y : prev > y ? prev : y;

            tr[x][0] = (uint8_t)((y * 255 + hm / 2) / hm);
            tr[x][1] = (uint8_t)((y0 * 255 + hm / 2) / hm);
            tr[x][2] = (uint8_t)((y1 * 255 + hm / 2) / hm);
            tr[x][3] = panel_level(p, cols[x]);
            prev     = y;
        }
        gpu_trace_set(p == &p_hf ? 0 : 1, (const uint8_t (*)[4])tr);
        return;
    }
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
/* Meteor-M LRPT on B (the daemon's lrpt.c): while B's channel is on a satellite's frequency, its pictures decoded
 * whether B is heard or not; once lines come, the pass's picture in place of the stick's history, the newest lines at
 * the bottom, in colour or a channel's grey (F3 held), and its state on RDS's line. The user, 2026-11-17: the
 * pictures in B's picture half, saved in /mnt/meteor */
static rtl_lrpt_t met;                  /* the decoder's state as last said */
static bool       met_on;               /* its picture shown in the history's place */
static int        met_view;             /* 0 colour (65, 65, 64), 1-6 APID 64-69 */
static uint32_t   met_ver_cpu = UINT32_MAX, met_ver_gpu = UINT32_MAX;
static uint32_t  *met_px;               /* the GPU's copy of it */
static void       met_update(void);

/* The Meteor satellite's frequency a channel is on (within 30 kHz), 0 none */
static int64_t met_of(int64_t hz) {
    if (llabs(hz - 137100000) <= 30000) return 137100000;
    if (llabs(hz - 137900000) <= 30000) return 137900000;
    return 0;
}

static void panel_render(const panel_t *p) {
    uint32_t *px = (uint32_t *)rtl_frame->data, bg = argb(ink);
    int       h = p->wf_h, y0 = p->wf_y0, n = p->count;
    float     cx = RTL_COLS / 2.0f;

    if (h < 8 || rtl_gpu) return;       /* the GPU draws it from the rows (rtl_gpu_draw) */
    if (p == &p_rtl && met_on) {        /* a Meteor pass: its picture in the history's place */
        rtl_spectrum_lrpt_picture(px + (size_t)y0 * RTL_COLS, RTL_COLS, h, RTL_COLS, met_view, &met_ver_cpu);
        return;
    }
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
    if (rtl_gpu) gpu_levels_row(p == &p_hf ? 0 : 1, p->head, row);     /* into the GPU's ring as well */
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
    if (rtl_gpu) gpu_levels_all(p == &p_hf ? 0 : 1, &p->rows[0][0]);
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

    met_ver_cpu = met_ver_gpu = UINT32_MAX;     /* the Meteor picture's height may be another */

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
    hf_bx = rtl_bx = -1;
    lv_label_set_text(rtl_src_l[0], "");
    lv_label_set_text(rtl_src_l[1], "");
    for (int i = 0; i < RTL_TICKS; i++) {
        lv_obj_add_flag(hf_tick_l[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rtl_tick_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < RTL_PEAKS; i++) {      /* both halves' names: hf_labels() runs only with the X6200 shown, so
                                                * A's (SPLIT) stayed over B's picture (RTL) when the ring went on */
        lv_obj_add_flag(rtl_peak_l[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(hf_peak_l[i], LV_OBJ_FLAG_HIDDEN);
    }
    rtl_present();
}

/* The GPU's bracket on a trace (at landscape x0, y0, height h): the channel's sides from 2 rows down to 2 rows up,
 * and their ends turned 4 pixels inwards, as the CPU's */
static void gpu_bracket(int x0, int y0, int x, int h) {
    uint32_t line = argb(amb);

    gpu_rect(x0 + x - 10, y0 + 2, 1, h - 3, line);
    gpu_rect(x0 + x + 10, y0 + 2, 1, h - 3, line);
    gpu_rect(x0 + x - 10, y0 + 2, 4, 1, line);
    gpu_rect(x0 + x + 7, y0 + 2, 4, 1, line);
    gpu_rect(x0 + x - 10, y0 + h - 2, 4, 1, line);
    gpu_rect(x0 + x + 7, y0 + h - 2, 4, 1, line);
}

/* The picture by the GPU, into the plane under the hole at a: each half's trace (once it has one), its 3D rows and
 * its bracket, the split's line between them; the CPU's drawing of it (rtl_frame) skipped */
static void rtl_gpu_draw(const lv_area_t *a) {
    int      view = spec_view(), x0 = a->x1, y0 = a->y1;
    uint32_t grid = argb(faint);

    if (!gpu_frame_begin(false)) return;
    gpu_clear(x0, y0, RTL_COLS, rtl_img_h, argb(ink));
    if (view != VIEW_RTL) {
        if (p_hf.floor > -250)
            gpu_trace(0, x0, y0 + p_hf.y0, RTL_COLS, p_hf.trace_h, p_hf.bot_db, p_hf.top_db, grid, argb(ink));
        gpu_rows(0, x0, y0 + p_hf.wf_y0, RTL_COLS, p_hf.wf_h, p_hf.head, p_hf.count, D3_ROWS);
        if (hf_bx >= 0) gpu_bracket(x0, y0 + p_hf.y0, hf_bx, p_hf.trace_h);
    }
    if (view != VIEW_X6200) {
        if (p_rtl.floor > -250)
            gpu_trace(1, x0, y0 + p_rtl.y0, RTL_COLS, p_rtl.trace_h, p_rtl.bot_db, p_rtl.top_db, grid, argb(ink));
        if (met_on && met_px) {         /* a Meteor pass: its picture in the history's place */
            if (rtl_spectrum_lrpt_picture(met_px, RTL_COLS, p_rtl.wf_h, RTL_COLS, met_view, &met_ver_gpu))
                gpu_pic(met_px, RTL_COLS, p_rtl.wf_h);
            gpu_pic_draw(x0, y0 + p_rtl.wf_y0, RTL_COLS, p_rtl.wf_h);
        } else {
            gpu_rows(1, x0, y0 + p_rtl.wf_y0, RTL_COLS, p_rtl.wf_h, p_rtl.head, p_rtl.count, D3_ROWS);
        }
        if (rtl_bx > -20 && rtl_bx < RTL_COLS + 20) gpu_bracket(x0, y0 + p_rtl.y0, rtl_bx, p_rtl.trace_h);
    }
    if (view == VIEW_SPLIT) gpu_rect(x0, y0 + p_rtl.y0 - 3, RTL_COLS, 2, argb(amb));
    gpu_frame_end(x0, y0, RTL_COLS, rtl_img_h);
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

/* Why the stick's picture is empty, the device named as it is (RTL-SDR, HACKRF, BLADERF); "" when it is not */
static void stick_why(rtl_spec_state_t st, char *b, size_t n) {
    const char *d = rtl_spectrum_dev_name();
    char        tn[16];

    switch (st) {
        case RTL_SPEC_NO_HELPER: snprintf(b, n, "THE SDR'S DAEMON DOES NOT START\n\n/USR/BIN/X6X00-RTLSPEC"); break;
        case RTL_SPEC_NO_STICK:
            snprintf(b, n, "NO SDR ON USB\n\nPLUG AN RTL-SDR, A HACKRF OR A BLADERF\nINTO THE HOST PORT");
            break;
        case RTL_SPEC_BUSY: snprintf(b, n, "THE %s IS BUSY\n\nWAITING FOR IT", d); break;
        case RTL_SPEC_OPENING: snprintf(b, n, "OPENING THE %s", d); break;
        case RTL_SPEC_STALLED: snprintf(b, n, "THE %s SENDS NOTHING\n\nUNPLUG IT AND PLUG IT IN AGAIN", d); break;
        case RTL_SPEC_TX: snprintf(b, n, "THE %s IS TRANSMITTING\n\nG WALKIE HAS IT", d); break;
        case RTL_SPEC_NEEDS:
            rtl_spectrum_tuner(tn, sizeof(tn));
            if (!strcmp(tn, "LIBBLADERF"))
                snprintf(b, n, "THE BLADERF NEEDS LIBBLADERF\n\nNOT ON THIS IMAGE");
            else if (!strcmp(tn, "STARTFAIL"))
                snprintf(b, n, "THE BLADERF FAILED AS IT STARTED\n\nMOST LIKELY TOO LITTLE POWER: ITS OWN\n"
                               "5 V OR A POWERED HUB, THEN PLUG IT IN AGAIN");
            else if (!strcmp(tn, "FIRMWARE") || !strcmp(tn, "FPGA-UPDATE"))
                snprintf(b, n, "THE BLADERF'S FPGA IMAGE AND ITS\nFIRMWARE DO NOT GO TOGETHER\n\nNUAND'S bladeRF_fw_vX.Y.Z.img IN\n"
                               "/mnt/bladerf UPDATES ITS FIRMWARE");
            else
                snprintf(b, n, "THE BLADERF NEEDS ITS FPGA IMAGE\n\n%s IN /mnt/bladerf\nOR IN ITS OWN FLASH", tn);
            break;
        default: b[0] = 0; break;
    }
}

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
    snprintf(t, sizeof(t), "HEARD: %s", hear_name(heard));
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
        snprintf(t, sizeof(t), "+%.0f DB  %s", tg[b].con, scan_word(false));
    } else {
        opt_set(&r_sig, 0, "ONLY NOISE", 0);
        snprintf(t, sizeof(t), "SCAN STYLE: %s", scan_word(true));
    }
    opt_set(&r_sig, 1, t, 2);
    /* the stick's half, split: why it is empty, as C says */
    rtl_spec_state_t st = rtl_spectrum_state();
    char why[160];

    stick_why(st, why, sizeof(why));
    lv_label_set_text(rtl_why_l, why);
    if (spec_view() == VIEW_X6200 || st == RTL_SPEC_ON || st == RTL_SPEC_OFF) lv_obj_add_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
}

/* The stick's strongest signal off its own picture (F's scan running behind another screen, SCAN CFG): the bin highest
 * over the row's median within the view, 10 dB over at least; 0 none */
static void rtl_pick_bg(const float *db, uint16_t n, uint32_t rate, int64_t c) {
    static float sorted[8192];
    int64_t      half = rtl_view_hz() / 2;
    int          k0   = (int)(n / 2 - (double)half * n / rate), k1 = (int)(n / 2 + (double)half * n / rate), b = -1;

    if (!n || !rate) return;
    memcpy(sorted, db, (size_t)n * sizeof(float));
    float med = median(sorted, n);

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
    static const char *const st[] = {"OFF", "ITS DAEMON DOES NOT START", "NO SDR ON USB", "BUSY: WAITING",
                                     "OPENING", "RECEIVING", "SENDS NOTHING", "TRANSMITTING (G)", "NOT READY"};
    rtl_spec_state_t s = rtl_spectrum_state();
    char             t[48], tn[16], f[16];
    int              g = param_i_get(cfg_rtl_gain);

    /* No stick for a moment (the first second of a start, a replug) changes nothing: its audio is silent meanwhile,
     * RECEIVER says why, and it is heard again as soon as it is back (the daemon is told what to play as the GUI
     * connects) */
    rtl_spectrum_tuner(tn, sizeof(tn));
    if (rtl_spectrum_dev() == RTL_DEV_RTL || rtl_spectrum_dev() == RTL_DEV_NONE || s == RTL_SPEC_NEEDS) {
        snprintf(t, sizeof(t), "%s %s", rtl_spectrum_dev_name(), s == RTL_SPEC_NEEDS ? "" : tn);
    } else {                            /* the model says it: "HACKRF-ONE" as HACKRF ONE, "BLADERF2" */
        snprintf(t, sizeof(t), "%s", tn[0] ? tn : rtl_spectrum_dev_name());
        for (char *c = t; *c; c++) if (*c == '-') *c = ' ';
    }
    opt_set(&r_dev, 0, t, 0);
    if (s == RTL_SPEC_ON) snprintf(t, sizeof(t), "HEARD: %s", hear_name(heard));
    opt_set(&r_dev, 1, s == RTL_SPEC_ON ? t : st[s], s == RTL_SPEC_ON ? (listen_rtl ? 1 : 2) : 0);
    fmt_mhz(f, sizeof(f), rtl_hz, 3);
    snprintf(t, sizeof(t), "%s MHZ", f);
    opt_set(&r_tune, 0, t, 0);
    static const char *const steps[] = {"12.5K", "25K", "100K", "500K", "1M"};
    if (rds_tuned_note(t, sizeof(t))) {     /* the station on B's channel, by its RDS */
        opt_set(&r_tune, 1, t, 0);
    } else {
        snprintf(t, sizeof(t), "%.1f MHZ / %s", rtl_view_hz() / 1e6, steps[rtl_step]);
        opt_set(&r_tune, 1, t, 2);
    }
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
    char why[160];

    stick_why(s, why, sizeof(why));
    lv_label_set_text(rtl_why_l, why);
    if (s == RTL_SPEC_ON || s == RTL_SPEC_OFF) lv_obj_add_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(rtl_why_l, LV_OBJ_FLAG_HIDDEN);
}

/* The stick's tuning, sent, and kept in the settings when keep (a scan's steps are not) */
static void rtl_tune_to(int64_t hz, bool keep) {
    if (hz < rtl_spectrum_min_hz()) hz = rtl_spectrum_min_hz();
    if (hz > rtl_spectrum_max_hz()) hz = rtl_spectrum_max_hz();
    rtl_hz = hz;
    stick_at = hz;
    if (keep && hz <= INT32_MAX) param_i_set(cfg_rtl_freq, (int32_t)hz);   /* the setting's 32 bits: past 2.1 GHz
                                                                           * (a HackRF's) gui_state keeps it, in kHz */
    rtl_spectrum_tune(hz);
    /* B's channel follows the stick out of a span it has left (a preset, a band, a pan): to the new one's middle, and
     * heard there; left behind it played nothing (2026-11-14: a preset changed on B went silent) */
    if (!listen_hz || llabs(listen_hz - hz) > rtl_view_hz() / 2) {
        listen_hz = rtl_snap(hz);
        if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
    }
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

    if (heard == HEAR_BOTH && rtl_spectrum_on_monitor()) {     /* BOTH's monitor test: the X6200 at A's volume as
                                                                 * ever, the stick at B's share of it */
        radio_set_vol_override(-1);
        rtl_spectrum_levels(x && r < x ? powf((float)r / x, 1.5f) : 1, 0);
    } else if (heard == HEAR_RTL || (heard == HEAR_BOTH && !rtl_spectrum_on_headphones())) {   /* BOTH on the
                                                                                         * speaker: the stick alone */
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

/* The speaker: the X6200's receiver, the stick at B's channel (listen_hz), or both mixed (rtl_spectrum_mux). The stick
 * goes back to B's place for it if C or G moved it; no stick, the X6200 for now (hear_follow puts the stick on when one
 * is plugged in) */
static void hear_apply(int h) {
    hear_want = h;
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

/* What LISTEN h does, for the OSD: B's channel, its mode put in */
/* BOTH mixes the two only on Bluetooth headphones: while the speaker plays the stick, the base sends nothing of the
 * X6200's receiver (rtl_spectrum.c), so there it is the stick alone, and says so */
static void hear_how(int h, char *t, size_t n) {
    static const char *const how[] = {"THE X6200'S RECEIVER ON THE SPEAKER.",
                                      "THE %s ON THE SPEAKER, AT B'S CHANNEL: %s %s.",
                                      "THE X6200 AND THE %s TOGETHER ON THE HEADPHONES (B'S CHANNEL: %s %s).",
                                      "THE SPEAKER SILENT: NEITHER RADIO IS HEARD (THEIR VOLUME KEPT)."};
    int64_t hz = listen_hz ? listen_hz : rtl_hz;
    char    f[16], sink[128];

    fmt_mhz(f, sizeof(f), hz, 3);
    if (h == HEAR_BOTH && hear_many())  /* ALL: the X6200, B's SDR and the watch channels (rtl_watch.c) */
        snprintf(t, n, btaudio_hp_sink(sink, sizeof(sink))
                           ? "ALL TOGETHER ON THE HEADPHONES: THE X6200, THE %s AT B'S CHANNEL (%s %s) AND EACH WATCH "
                             "CHANNEL AS IT OPENS (F4 HELD ON B ADDS ONE)."
                           : "THE %s (%s %s) AND EACH WATCH CHANNEL AS IT OPENS ON THE SPEAKER: WHILE IT PLAYS THEM THE "
                             "BASE SENDS NOTHING OF THE X6200 (ALL OF IT ON BLUETOOTH HEADPHONES).",
                 rtl_spectrum_dev_name(), f, rtl_mode_names[rtl_mode_of(hz)]);
    else if (h == HEAR_BOTH && !btaudio_hp_sink(sink, sizeof(sink)))
        snprintf(t, n, "THE %s ALONE (%s %s): WHILE THE SPEAKER PLAYS IT, THE BASE SENDS NOTHING OF THE X6200. "
                       "BOTH TOGETHER NEEDS BLUETOOTH HEADPHONES.", rtl_spectrum_dev_name(), f,
                 rtl_mode_names[rtl_mode_of(hz)]);
    else if (h == HEAR_RTL || h == HEAR_BOTH)
        snprintf(t, n, how[h], rtl_spectrum_dev_name(), f, rtl_mode_names[rtl_mode_of(hz)]);
    else
        snprintf(t, n, "%s", how[h]);
}

/* F4 on A or B: that screen's LISTEN, the next one, heard at once. RTL and BOTH with no stick yet: the X6200 alone
 * till one is plugged in, then the stick (hear_follow) */
static bool b_listen_next_sdr(void);
static void show_screen(void);

static void hear_next(void) {
    ParamInt *c = scr == SCR_RTL ? cfg_hear_f : cfg_hear_a;
    int       h;
    char      t[160], title[32];

    if (scr == SCR_RTL && b_listen_next_sdr()) {    /* the next SDR (b_use said so); the LISTEN stays the SDR's */
        hear_apply(HEAR_RTL);
        return;
    }
    h = (param_i_get(c) + 1) % HEAR_N;

    param_i_set(c, h);
    hear_apply(h);
    snprintf(title, sizeof(title), "%s LISTEN: %s", scr == SCR_RTL ? "B" : "A", hear_name(h));
    if (heard != h)                     /* the X6200 standing in */
        snprintf(t, sizeof(t), "NO SDR YET: THE X6200 ALONE TILL AN RTL-SDR, HACKRF OR BLADERF IS PLUGGED INTO THE "
                 "HOST PORT, THEN %s.", h == HEAR_BOTH ? "BOTH TOGETHER" : "THE SDR");
    else
        hear_how(h, t, sizeof(t));
    osd(title, t, -1);
}

/* ---- B's SDR: which one the daemon holds, when more than one is on USB ----
 * The user, 2026-11-15: B and G "should be compatible with all devices"; "needs the SDR daemon to switch between
 * devices ... well work that out... and it should only use devices it can tune". The daemon holds one at a time
 * (x6x00-rtlspec's "k": the one asked for closed and opened in its place); those that tune B's channel are offered:
 * a box when two or more are on USB (once for each set), and B's LISTEN goes through them (X6200, each SDR, BOTH, OFF).
 * Each RTL on its own (2026-11-16, a KrakenSDR's five: "labled with an index like RTL0 RTL1 RTL2", ordered "by serial
 * and usb bus location"): the units below, a HackRF, a bladeRF, RTL0, RTL1... cfg_rtl_dev keeps the kind chosen
 * (rtl_dev_t, bits 0-3: screen_main's start reads them), the set of units it was chosen among (bits 4-13) and the RTL
 * chosen (bits 16-19) */
enum { BU_HACKRF, BU_BLADERF, BU_RTL0 };
#define BU_N          (BU_RTL0 + 8)
#define DEVB(u)       (1u << (u))
#define CFG_KIND(v)   ((rtl_dev_t)((v) & 15))
#define CFG_SET(v)    (((unsigned)(v) >> 4) & 0x3ffu)
#define CFG_RTL(v)    (((unsigned)(v) >> 16) & 15u)
static lv_obj_t *sp_box, *sp_head_l, *sp_row_l[BU_N], *sp_foot_l;
static bool      sp_on;
static int       sp_units[BU_N];
static int       sp_n, sp_sel;
static unsigned  sp_set, sp_seen;
static uint32_t  sp_seen_ms;

static rtl_dev_t bu_kind(int u) {
    return u == BU_HACKRF ? RTL_DEV_HACKRF : u == BU_BLADERF ? RTL_DEV_BLADERF : RTL_DEV_RTL;
}

static int bu_of(rtl_dev_t d, int rtl) {
    return d == RTL_DEV_HACKRF ? BU_HACKRF : d == RTL_DEV_BLADERF ? BU_BLADERF
         : d == RTL_DEV_RTL    ? BU_RTL0 + (rtl < 0 ? 0 : rtl > 7 ? 7 : rtl) : -1;
}

static const char *bu_word(int u) {
    return u == BU_HACKRF ? "HACKRF" : u == BU_BLADERF ? "BLADERF" : rtl_spectrum_rtl_tag(u - BU_RTL0);
}

/* The SDRs on USB that tune B's channel, a bit each unit */
static unsigned b_sdrs(void) {
    unsigned p = adsb_rx_present(), m = 0;
    int64_t  hz = listen_hz ? listen_hz : rtl_hz, lo, hi;
    int      rtls = sdr_pool_rtl_count();

    for (int u = 0; u < BU_N; u++) {
        rtl_dev_t k = bu_kind(u);

        if (u == BU_HACKRF ? !(p & (1u << ADSB_KIND_HACKRF)) : u == BU_BLADERF ? !(p & (1u << ADSB_KIND_BLADERF))
                                                                              : u - BU_RTL0 >= rtls)
            continue;
        rtl_spectrum_kind_range(k, &lo, &hi);
        if (!hz || (hz >= lo && hz <= hi)) m |= DEVB(u);
    }
    return m;
}

static bool hear_many(void) {
    return __builtin_popcount(b_sdrs()) >= 2;
}

/* F4 held on B and the VFO turned: B's SDRs one a click (back when turned back), the one chosen used when F4 is let
 * go (each switch opens a device: a few seconds), as F2 held rolls the presets (the user, 2026-11-16: "holding F4 and
 * turning dial should handle same way as holding F2 and turning dial, but ... it scrolls the connected devices") */
static bool f4_roll, f4_rolled;
static int  f4_pick = -1;
static int  b_dev(void);
static void osd(const char *title, const char *text, int bar);

static void b_roll(int diff) {
    unsigned m = b_sdrs();
    int      u, n = __builtin_popcount(m);
    char     t[200], title[40];

    if (n < 2) {
        osd("ONE SDR", "B HAS THE ONLY SDR THAT TUNES ITS CHANNEL.", -1);
        return;
    }
    u = f4_pick >= 0 ? f4_pick : b_dev();
    for (int d = diff > 0 ? diff : -diff; d > 0; d--) {
        int k = 0;

        do {
            u = (u + (diff > 0 ? 1 : BU_N - 1)) % BU_N;
        } while (!(m & DEVB(u)) && ++k < BU_N);
    }
    f4_pick = u;
    int at = snprintf(t, sizeof(t), "LET GO OF F4 TO USE IT (A FEW SECONDS):");   /* the list, the one chosen marked */
    for (int k = 0; k < BU_N && at < (int)sizeof(t); k++)
        if (m & DEVB(k)) at += snprintf(t + at, sizeof(t) - (size_t)at, k == u ? " [%s]" : " %s", bu_word(k));
    snprintf(title, sizeof(title), "B: %s%s", bu_word(u), u == b_dev() ? " (HAS IT)" : "");
    osd(title, t, -1);
}

/* The one B has: the one asked for while it is there (the daemon may still be opening it), else the daemon's */
static int b_dev(void) {
    rtl_dev_t u = rtl_spectrum_used();
    int       want = u == RTL_DEV_NONE ? -1 : bu_of(u, rtl_spectrum_used_rtl());

    if (want >= 0 && (b_sdrs() & DEVB(want))) return want;
    return bu_of(rtl_spectrum_dev(), rtl_spectrum_dev_rtl());
}

/* Unit u the daemon's from now on, kept with the set it was chosen among */
static void b_use(int u, unsigned set) {
    char t[160], title[32];
    int  was = b_dev();

    if (bu_kind(u) == RTL_DEV_RTL) rtl_spectrum_use_rtl(u - BU_RTL0);
    else rtl_spectrum_use(bu_kind(u));
    param_i_set(cfg_rtl_dev, (int)bu_kind(u) | (int)(set << 4) | (bu_kind(u) == RTL_DEV_RTL ? (u - BU_RTL0) << 16 : 0));
    snprintf(title, sizeof(title), "B: THE %s", bu_word(u));
    if (was == u) snprintf(t, sizeof(t), "THE %s STAYS B'S SDR.", bu_word(u));
    else if (was < 0)
        snprintf(t, sizeof(t), "THE %s OPENS: A FEW SECONDS. G'S SDR CHANNELS AND AIRSPACE USE IT TOO.", bu_word(u));
    else
        snprintf(t, sizeof(t), "THE %s CLOSED AND THE %s OPENED IN ITS PLACE: A FEW SECONDS. G'S SDR CHANNELS AND "
                 "AIRSPACE USE IT TOO.", bu_word(was), bu_word(u));
    osd(title, t, -1);
}

static void sp_fill(void) {
    int cur = bu_of(rtl_spectrum_dev(), rtl_spectrum_dev_rtl());

    for (int r = 0; r < BU_N; r++) {
        int64_t lo, hi;
        char    what[48];
        int     u = r < sp_n ? sp_units[r] : -1;

        if (u < 0) {
            lv_obj_add_flag(sp_row_l[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        rtl_spectrum_kind_range(bu_kind(u), &lo, &hi);
        if (bu_kind(u) == RTL_DEV_RTL) {        /* each RTL by its serial and USB location */
            char sn[24], loc[24];

            sdr_unit_t pu[SDR_POOL_UNITS];
            int        pn = sdr_pool_units(pu, SDR_POOL_UNITS);

            sn[0] = loc[0] = 0;
            for (int k = 0; k < pn; k++)
                if (pu[k].kind == RTL_DEV_RTL && pu[k].rtl == u - BU_RTL0) {
                    snprintf(sn, sizeof(sn), "%s", pu[k].sn);
                    snprintf(loc, sizeof(loc), "%s", pu[k].loc);
                }
            snprintf(what, sizeof(what), "SN %s, USB %s, RECEIVE ONLY", sn[0] ? sn : "-", loc[0] ? loc : "-");
        } else {
            snprintf(what, sizeof(what), "%s-6 GHZ, TRANSMITS ON G", lo < 10000000 ? "1 MHZ" : "70 MHZ");
        }
        lv_label_set_text_fmt(sp_row_l[r], "%c %-8s %s%s", r == sp_sel ? '>' : ' ', bu_word(u), what,
                              u == cur ? " (OPEN)" : "");
        lv_obj_set_style_bg_opa(sp_row_l[r], r == sp_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(sp_row_l[r], r == sp_sel ? ink : amb, 0);
        lv_obj_clear_flag(sp_row_l[r], LV_OBJ_FLAG_HIDDEN);
    }
}

static void sp_open(unsigned set) {
    int  cur = b_dev();
    char f[16];

    sp_set = set;
    sp_n = sp_sel = 0;
    for (int u = 0; u < BU_N; u++)
        if (set & DEVB(u)) {
            if (u == cur) sp_sel = sp_n;
            sp_units[sp_n++] = u;
        }
    fmt_mhz(f, sizeof(f), listen_hz ? listen_hz : rtl_hz, 3);
    lv_label_set_text_fmt(sp_head_l, "%d SDRS HERE TUNE B'S CHANNEL (%s MHZ). WHICH?", sp_n, f);
    lv_obj_set_y(sp_foot_l, 50 + sp_n * 28 + 10);
    lv_obj_set_height(sp_box, 50 + sp_n * 28 + 10 + 50);
    lv_obj_set_y(sp_box, sp_n > 3 ? 60 : 100);
    sp_fill();
    sp_on = true;
    lv_obj_clear_flag(sp_box, LV_OBJ_FLAG_HIDDEN);
}

static void sp_hide(void) {
    sp_on = false;
    if (sp_box) lv_obj_add_flag(sp_box, LV_OBJ_FLAG_HIDDEN);
}

/* Closed: unit u B's SDR (-1: as it is), this set answered */
static void sp_close(int u) {
    sp_hide();
    if (u >= 0) b_use(u, sp_set);
    else param_i_set(cfg_rtl_dev, (int)(((unsigned)param_i_get(cfg_rtl_dev) & ~(0x3ffu << 4)) | (sp_set << 4)));
    show_screen();
}

static void sp_step(int d) {
    if (!sp_n) return;
    sp_sel = (sp_sel + d + sp_n) % sp_n;
    sp_fill();
}

static void sp_row_tap_cb(lv_event_t *e) {
    int r = (int)(intptr_t)lv_event_get_user_data(e);

    if (sp_on && r < sp_n) sp_close(sp_units[r]);
}

/* On B, each blink: the box when a new set of two or more has stood 3 s and was not answered before */
static void sp_check(void) {
    unsigned m;

    if (scr != SCR_RTL || help_on || !adsb_rx_usb_looks()) {
        if (sp_on && scr != SCR_RTL) sp_hide();
        return;
    }
    m = b_sdrs();
    if (m != sp_seen) {
        sp_seen    = m;
        sp_seen_ms = lv_tick_get();
        return;
    }
    if (lv_tick_get() - sp_seen_ms < 3000) return;
    if (sp_on) {
        if (m == sp_set) return;
        if (__builtin_popcount(m) >= 2) sp_open(m);
        else sp_hide();
        return;
    }
    if (__builtin_popcount(m) >= 2 && CFG_SET(param_i_get(cfg_rtl_dev)) != m) sp_open(m);
}

/* B's LISTEN with two or more SDRs that tune its channel: X6200, then each SDR (the daemon switched to it), BOTH, OFF.
 * true: it went to the next SDR (the LISTEN stays the SDR's) */
static bool b_listen_next_sdr(void) {
    unsigned m = b_sdrs();
    int      cur = b_dev(), h = param_i_get(cfg_hear_f);

    if (__builtin_popcount(m) < 2) return false;
    if (h == HEAR_X6200) {              /* into the SDRs: the first that tunes it */
        for (int u = 0; u < BU_N; u++)
            if (m & DEVB(u)) {
                if (u != cur) b_use(u, m);
                return false;           /* LISTEN goes on to the SDR as ever */
            }
    }
    if (h != HEAR_RTL) return false;
    for (int u = cur + 1; u < BU_N; u++)
        if (m & DEVB(u)) {
            b_use(u, m);
            return true;
        }
    return false;                       /* the last: on to BOTH */
}

/* ---- The SDR pool's panel (sdr_pool.c): every SDR on USB, whose it is and whether it is heard, and keys to choose,
 * stop, start and hear each, from any screen: APP held, or F SYSTEM's SDR POOL (the user, 2026-11-16: "the rest of
 * the GUI tabs have the ability to select, start, stop, and mux their audio as needed"). F1 uses the one chosen on B
 * (G's SDR channels, C's stick entries, A's SDR LISTEN and D's bands are B's SDR), F2 stops it (nobody's: AIRSPACE
 * and the watch channels move off it; kept over restarts) or starts it again, F3 hears or mutes it (B's: the screen's
 * LISTEN; a watch channel's: in the mix or not), F4 watches B's channel on a free RTL, F5 back ---- */
static lv_obj_t  *pp_box, *pp_head_l, *pp_row_l[SDR_POOL_UNITS], *pp_foot_l;
static bool       pp_on;
static int        pp_sel, pp_n;
static sdr_unit_t pp_u[SDR_POOL_UNITS];
static void       show_keys(void);

static bool b_heard(void) {
    return heard == HEAR_RTL || heard == HEAR_BOTH;
}

static void pp_fill(void) {
    static const char *const mn[] = {"", "NFM", "AM", "WFM"};

    if (!pp_box) return;
    pp_n = sdr_pool_units(pp_u, SDR_POOL_UNITS);
    if (pp_sel >= pp_n) pp_sel = pp_n ? pp_n - 1 : 0;
    lv_label_set_text_fmt(pp_head_l, "SDR POOL: %d ON USB, EACH ONE'S USER AND AUDIO", pp_n);
    for (int r = 0; r < SDR_POOL_UNITS; r++) {
        sdr_unit_t *u = &pp_u[r];
        char        who[48], f[16];

        if (r >= pp_n) {
            lv_obj_add_flag(pp_row_l[r], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        if (u->owner == SDR_OWN_B) {
            int64_t hz = listen_hz ? listen_hz : rtl_hz;

            fmt_mhz(f, sizeof(f), hz, 3);
            snprintf(who, sizeof(who), "B %s %s %s", f, mn[rtl_mode_of(hz) & 3], b_heard() ? "HEARD" : "MUTED");
        } else if (u->owner == SDR_OWN_DF) {
            snprintf(who, sizeof(who), "PAGE I DIRECTION FINDER CH%d", sdr_pool_array_ch(u));
        } else if (u->owner >= SDR_OWN_ADSB && u->owner < SDR_OWN_WATCH) {
            snprintf(who, sizeof(who), "AIRSPACE ADS-B 1090");
        } else if (u->owner >= SDR_OWN_WATCH && u->owner < SDR_OWNERS) {
            rtl_watch_info_t w;

            if (rtl_watch_get(u->owner - SDR_OWN_WATCH, &w)) {
                fmt_mhz(f, sizeof(f), w.hz, 3);
                snprintf(who, sizeof(who), "WATCH %s %s%s", f, w.open ? "OPEN" : "QUIET",
                         sdr_pool_heard(u->owner) ? "" : " MUTED");
            } else {
                snprintf(who, sizeof(who), "WATCH");
            }
        } else {
            snprintf(who, sizeof(who), "%s", u->stopped ? "STOPPED" : "FREE");
        }
        lv_label_set_text_fmt(pp_row_l[r], "%c %-7s %-6s %-7s %s%s", r == pp_sel ? '>' : ' ', u->name,
                              u->sn[0] ? u->sn : "-", u->loc, who, u->own_power ? "" : "  (RADIO'S POWER)");
        lv_obj_set_style_bg_opa(pp_row_l[r], r == pp_sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(pp_row_l[r], r == pp_sel ? ink : u->stopped ? dim : amb, 0);
        lv_obj_clear_flag(pp_row_l[r], LV_OBJ_FLAG_HIDDEN);
    }
    int n = pp_n ? pp_n : 1;

    lv_obj_set_y(pp_foot_l, 50 + n * 28 + 10);
    lv_obj_set_height(pp_box, 50 + n * 28 + 10 + 50);
    if (!pp_n) {
        lv_obj_clear_flag(pp_row_l[0], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pp_row_l[0], "  NO SDR ON USB: AN RTL-SDR, KRAKENSDR, HACKRF OR BLADERF ON THE HOST PORT");
    }
}

static void pp_keys(void) {
    for (int k = 0; k < 5; k++) buttons_refresh(&btn[k]);
    if (key_l[0]) show_keys();
}

static void pp_open(void) {
    if (g_portrait) {                   /* drawn on the landscape screen: not over G held up */
        w_say("SDR POOL: FROM ANOTHER SCREEN", "SDR POOL", "THE SDR POOL OPENS FROM ANY SCREEN BUT G HELD UP.");
        return;
    }
    if (sp_on) sp_hide();
    pp_on = true;
    pp_fill();
    lv_obj_clear_flag(pp_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pp_box);
    pp_keys();
}

static void pp_close(void) {
    pp_on = false;
    if (pp_box) lv_obj_add_flag(pp_box, LV_OBJ_FLAG_HIDDEN);
    pp_keys();
    show_screen();
}

static void b_watch_toggle(void);

static void pp_action(int k) {
    sdr_unit_t *u = pp_n ? &pp_u[pp_sel] : NULL;
    char        t[160];

    if (k == 4 || !u) {
        pp_close();
        return;
    }
    switch (k) {
        case 0:                         /* this one B's */
            if (u->stopped) {
                osd("STOPPED", "START IT FIRST (F2), THEN USE IT ON B.", -1);
                break;
            }
            b_use(bu_of(u->kind, u->rtl), b_sdrs());
            break;
        case 1:                         /* nobody's, or free again */
            if (u->owner == SDR_OWN_B) {
                osd("B'S SDR", "B KEEPS ONE: USE ANOTHER ON B (F1 ON IT), THEN STOP THIS ONE.", -1);
                break;
            }
            sdr_pool_stop(pp_sel, !u->stopped);
            snprintf(t, sizeof(t), u->stopped ? "FREE AGAIN: AIRSPACE, A WATCH CHANNEL OR B MAY TAKE IT."
                                              : "NOBODY'S TILL STARTED AGAIN (F2), OVER RESTARTS TOO: WHOEVER HAD "
                                                "IT MOVES TO ANOTHER OR WAITS.");
            osd(u->stopped ? "STARTED" : "STOPPED", t, -1);
            break;
        case 2:                         /* in the mix or not */
            if (u->owner == SDR_OWN_B) {    /* the screen's LISTEN (B's off A and B): the SDR, or the X6200 */
                ParamInt *c  = scr == SCR_SCAN ? cfg_hear_a : cfg_hear_f;
                int       to = b_heard() ? HEAR_X6200 : HEAR_RTL;

                param_i_set(c, to);
                hear_apply(to);
                osd(to == HEAR_RTL ? "B'S SDR HEARD" : "B'S SDR MUTED",
                    to == HEAR_RTL ? "ITS CHANNEL ON THE SPEAKER (THE X6200 OFF IT): F3 AGAIN, THE X6200."
                                   : "THE X6200 ON THE SPEAKER AGAIN: F3 AGAIN, THE SDR.", -1);
            } else if (u->owner >= SDR_OWN_WATCH && u->owner < SDR_OWNERS) {
                bool on = !sdr_pool_heard(u->owner);

                sdr_pool_hear(u->owner, on);
                osd(on ? "WATCH HEARD" : "WATCH MUTED", on ? "ITS AUDIO ON THE SPEAKER WHEN IT OPENS."
                                                           : "STILL WATCHED (ITS LINE ON B), NOT HEARD.", -1);
            } else if (u->owner == SDR_OWN_DF) {
                osd("PAGE I", "NO AUDIO: THE DIRECTION FINDER HAS THE WHOLE KRAKEN WHILE PAGE I IS OPEN.", -1);
            } else if (u->owner >= SDR_OWN_ADSB) {
                osd("ADS-B", "NO AUDIO: DUMP1090 DECODES IT FOR AIRSPACE.", -1);
            } else {
                osd("FREE", "NOTHING TO HEAR: F1 USES IT ON B, F4 WATCHES B'S CHANNEL ON A FREE ONE.", -1);
            }
            break;
        case 3:
            b_watch_toggle();
            break;
    }
    pp_fill();
    pp_keys();
}

static const char *pp_label(int k) {
    static char s[24];
    sdr_unit_t *u = pp_n ? &pp_u[pp_sel] : NULL;

    switch (k) {
        case 0: return "Use\non B";
        case 1: return u && u->stopped ? "Start" : "Stop";
        case 2:
            if (u && u->owner == SDR_OWN_B) return b_heard() ? "Mute" : "Hear";
            if (u && u->owner >= SDR_OWN_WATCH && u->owner < SDR_OWNERS) return sdr_pool_heard(u->owner) ? "Mute" : "Hear";
            return "Hear";
        case 3: snprintf(s, sizeof(s), "%s", "Watch\nB's ch"); return s;
        default: return "Back";
    }
}

/* An RTL-SDR plugged in (its rows coming) while what is to be heard has the stick and the X6200 stands in for it:
 * the stick on the speaker at once, alone or mixed with the X6200, on whatever screen. A stick pulled out changes
 * nothing: it is silent meanwhile and heard again as soon as it is back (the daemon keeps what it plays) */
static void hear_follow(void) {
    char t[160], title[32];

    if ((hear_want != HEAR_RTL && hear_want != HEAR_BOTH) || listen_rtl || rtl_spectrum_state() != RTL_SPEC_ON ||
        scr == SCR_WALKIE)              /* G is the X6200's alone */
        return;
    hear_apply(hear_want);
    if (!listen_rtl || lv_tick_get() - opened_ms < 5000) return;
    snprintf(title, sizeof(title), "%s IN: %s", rtl_spectrum_dev_name(), hear_name(heard));
    hear_how(heard, t, sizeof(t));
    osd(title, t, -1);
}

/* The VFO knob while the stick is listened to: a channel a click; the stick follows when it nears an edge */
static void rtl_listen_turn(int32_t diff) {
    rtl_scan_set(false);                /* a channel chosen by hand: the scan stops there */
    int64_t c = rtl_hz, half = rtl_view_hz() / 2;

    int64_t was = listen_hz;

    listen_hz = rtl_snap(was + diff * rtl_step_of(was));
    /* at a grid's edge a step can snap back where it was (108.000's 25 kHz down into FM's 100 kHz channels): more */
    for (int i = 2; listen_hz == was && i <= 8; i++) listen_hz = rtl_snap(was + diff * i * rtl_step_of(was));
    if (listen_hz < rtl_spectrum_min_hz()) listen_hz = rtl_spectrum_min_hz();
    if (listen_hz > rtl_spectrum_max_hz()) listen_hz = rtl_spectrum_max_hz();
    if (llabs(listen_hz - c) > half * 85 / 100) rtl_tune(listen_hz);
    if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));     /* muted: the bracket moves alone */
    show_head();
    show_rtl();
    b_tuned();                          /* on B with the X6200 heard: the SDR heard */
}

/* The VFO knob is the screen's radio's: on B the stick's channel always (heard or not), on C a stick entry playing,
 * on G the channel whichever radio has it (the dial is not turned a step as well) */
static bool knob_taken(void) {
    if (g_armed || g_sdr_armed || radio_get_state() == RADIO_TX) return true;  /* held: dial_held says why */
    if (scr == SCR_WALKIE) return w_sdr_hz != 0;    /* the dial, fine, as on A (only the arrows step CB's
                                                     * channels); on the SDR its channel */
    return scr == SCR_RTL || g_stick_side();
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
/* B's scan stopped on a signal (SCAN CFG's STAY ON A SIGNAL): its channel, since when, when it last talked; held: till
 * SCAN CFG's F2 GO ON or F3 SKIP. And the channels F3 SKIP passed over, till the Sentry closes */
static struct { bool on, held; int64_t hz; uint32_t start, heard; } b_stop;
static int64_t b_skip[16];
static int     b_nskip;
#define B_TALK_DB 10                    /* B's channel this far over its picture's noise talks (B's squelch, when set) */

static bool b_skipped(int64_t hz) {
    for (int i = 0; i < b_nskip; i++)
        if (llabs(b_skip[i] - hz) < rtl_step_of(hz)) return true;
    return false;
}

static bool b_talking(void) {
    return param_i_get(cfg_rtl_sql) ? rtl_spectrum_squelch_open() : rtl_spectrum_level() >= B_TALK_DB;
}

static void rtl_scan_set(bool on) {
    if (on == rtl_scan) return;
    rtl_scan  = on;
    b_stop.on = false;
    if (on) {
        scan_next = 0;                  /* at once: to the band's low end */
        scan_pick = 0;
    } else if (rtl_hz) {
        if (rtl_hz <= INT32_MAX) param_i_set(cfg_rtl_freq, (int32_t)rtl_hz);     /* where it stopped, kept */
    }
    if (scr == SCR_RTL) buttons_refresh(&btn[0]);       /* F1's On or Off at once (a pan stops it too) */
    show_rtl();
}

static void rtl_scan_step(void) {
    uint32_t now = lv_tick_get();

    /* on whatever screen (B's log and SCAN CFG see it), but not while C plays a stick entry of its own */
    if (!rtl_scan || rtl_spectrum_state() != RTL_SPEC_ON || (scr == SCR_LOG && g_onr >= 0)) return;
    if (scan_pick && now >= scan_pick) {
        scan_pick = 0;
        if (rtl_best && !b_skipped(rtl_snap(rtl_best))) {  /* the picture's strongest (10 dB over its noise), not
                                                             * skipped: into C's log, B's channel to it, heard if B is */
            int64_t hz = rtl_snap(rtl_best);

            hit_add(hz, rtl_mode_of(hz), true, rtl_name(hz), rtl_best_db);
            listen_hz = hz;
            if (listen_rtl) rtl_spectrum_listen(listen_hz, rtl_mode_of(listen_hz));
            show_head();
            if (scan_stay != STAY_NONE) {   /* and stopped there, as the style says */
                b_stop.on    = true;
                b_stop.held  = scan_stay == STAY_HOLD;
                b_stop.hz    = hz;
                b_stop.start = b_stop.heard = now;
            }
        }
    }
    if (b_stop.on) {                    /* stopped: while it talks (or as long as STAY says), or held */
        if (listen_hz != b_stop.hz) {
            b_stop.on = false;          /* B's channel moved by hand: on from there */
        } else {
            uint32_t stay = stay_ms[scan_stay];

            if (b_talking()) b_stop.heard = now;
            if (b_stop.held ||
                (now - b_stop.heard <= quiet_ms[scan_quiet] && (stay == UINT32_MAX || now - b_stop.start <= stay)))
                return;
            b_stop.on = false;
            scan_next = now;            /* quiet, or its time up: on at once */
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
    if (scr == SCR_WALKIE) ws_feed();   /* G's picture: the X6200's spectrum around the dial */
    if (scr == SCR_WALKIE && g_dirty) g_gpu_draw();
    rtl_spectrum_meteor(met_of(listen_hz));     /* B on a Meteor satellite: its pictures, whatever the screen */
    met_update();
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
        if (rtl_gpu) {
            hf_bx = RTL_COLS / 2;       /* the GPU's bracket (rtl_gpu_draw) */
        } else {   /* the dial: the X6200's channel, as the stick's is bracketed below */
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
        rtl_bx = -100;
        if (listen_hz && rtl_gpu) {
            rtl_bx = (int)lround((listen_hz - lo) / px_hz);     /* the GPU's bracket (rtl_gpu_draw) */
        } else if (listen_hz) {             /* F's channel (the VFO knob's): a bracket on the trace */
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
            snprintf(s, sizeof(s), "%s  %s %s  %s", rtl_spectrum_dev_tag(), lf, rtl_mode_word(listen_hz),
                     !listen_rtl ? "MUTED" : rtl_spectrum_squelch_open() ? "HEARD" : "SQUELCHED");
        } else {
            snprintf(s, sizeof(s), "%s  %s MHZ", rtl_spectrum_dev_name(), f);
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
    if (drew) rtl_present();
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
        a_tuned();
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
#if USE_DRM
    rtl_direct = drm_planes();
#endif
    rtl_gpu = rtl_direct && gpu_init(RTL_COLS, D3_ROWS);   /* the GPU there (Mesa on the image): it draws it */
    if (rtl_gpu) gpu_palette(0, d3_pal);
    if (rtl_direct) {   /* the picture on the primary plane (rtl_present), the GUI a hole over it: the ticks, peaks
                         * and the bracket's labels drawn over the hole. LVGL drew it (a copy into its buffer), turned
                         * it onto the panel (a second) and copied that (a third), each frame: most of the GUI
                         * thread's time on A and B (perf, 2026-11-14) */
        rtl_img = lv_obj_create(p);
        lv_obj_remove_style_all(rtl_img);
        lv_obj_set_size(rtl_img, RTL_COLS, rtl_img_h);
        lv_obj_add_event_cb(rtl_img, direct_hole_cb, LV_EVENT_DRAW_MAIN_END, NULL);
    } else {
        rtl_img = lv_img_create(p);
        lv_img_set_src(rtl_img, rtl_frame);
    }
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
    rds_l = label(p, &pixel_16, amb, IMG_X + 6, iy + 6, 0, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_set_style_bg_color(rds_l, ink, 0);
    lv_obj_set_style_bg_opa(rds_l, LV_OPA_80, 0);     /* over the history, as it is drawn */
    lv_obj_set_style_pad_hor(rds_l, 3, 0);
    lv_obj_add_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
    watch_l = label(p, &pixel_16, amb, IMG_X + 6, iy + rtl_img_h - 24, 0, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_set_style_bg_color(watch_l, ink, 0);
    lv_obj_set_style_bg_opa(watch_l, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(watch_l, 3, 0);
    lv_label_set_recolor(watch_l, true);
    lv_obj_add_flag(watch_l, LV_OBJ_FLAG_HIDDEN);
    rtl_hz = param_i_get(cfg_rtl_freq);
    for (int i = RTL_PRESETS - 1; i >= 0; i--)  /* the preset the tuning is in (at, first), if one */
        if (rtl_hz >= rtl_presets[i].lo && rtl_hz <= rtl_presets[i].hi) rtl_preset = i;
    for (int i = 0; i < RTL_PRESETS; i++)
        if (rtl_presets[i].hz == rtl_hz) rtl_preset = i;
    rtl_layout();
}

/* The second source label goes with the stick's half, RDS's line at the top of its history */
static void rds_show(bool now);

static void rtl_labels_place(void) {
    int iy = lv_obj_get_y(rtl_img);

    lv_obj_set_y(rtl_src_l[1], iy + p_rtl.y0 + 3);
    lv_obj_set_y(rtl_src_l[0], iy + p_hf.y0 + 3);
    lv_obj_set_y(rds_l, iy + p_rtl.wf_y0 + 6);
    if (watch_l) lv_obj_set_y(watch_l, iy + rtl_img_h - 24);
    rds_show(true);
    watch_show();
}

/* B's watch channels, a line at the bottom of the stick's history: each RTL, channel and mode, OPEN lit in the pale
 * of the tags' top while it is heard */
static void watch_show(void) {
    static const char *const mn[] = {"", "NFM", "AM", "WFM"};
    char                     line[200], f[16];
    int                      n = rtl_watch_count(), at;

    if (!watch_l) return;
    if (scr != SCR_RTL || !n) {
        lv_obj_add_flag(watch_l, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    uint32_t pale = lv_color_to32(TC(accent_soft)) & 0xffffff;

    at = snprintf(line, sizeof(line), "WATCH");
    for (int i = 0; i < n && at < (int)sizeof(line); i++) {
        rtl_watch_info_t w;

        if (!rtl_watch_get(i, &w)) continue;
        fmt_mhz(f, sizeof(f), w.hz, 3);
        if (w.place < 0)
            at += snprintf(line + at, sizeof(line) - (size_t)at, "  %s NO RTL FREE", f);
        else if (w.open)
            at += snprintf(line + at, sizeof(line) - (size_t)at, "  #%06X %s %s %s OPEN#", (unsigned)pale,
                           rtl_spectrum_rtl_tag(w.place), f, mn[w.mode & 3]);
        else
            at += snprintf(line + at, sizeof(line) - (size_t)at, "  %s %s %s%s", rtl_spectrum_rtl_tag(w.place), f,
                           mn[w.mode & 3], w.up ? "" : " (STARTING)");
    }
    lv_label_set_text(watch_l, line);
    lv_obj_clear_flag(watch_l, LV_OBJ_FLAG_HIDDEN);
}

/* F4 held on B: B's channel watched on a spare RTL of its own, or no longer (the user, 2026-11-16, "Tuners at once":
 * the Kraken's free RTLs each on a channel, squelched, heard when it opens) */
static void b_watch_toggle(void) {
    int64_t hz = listen_hz ? listen_hz : rtl_hz;
    int     r;
    char    f[16], t[200], title[40];

    if (rtl_watch_has(hz)) r = rtl_watch_toggle(hz, rtl_mode_of(hz), 0, 0);
    else r = rtl_watch_toggle(hz, rtl_mode_of(hz), param_i_get(cfg_rtl_sql), param_i_get(cfg_rtl_gain));
    fmt_mhz(f, sizeof(f), hz, 3);
    if (r == 1) {
        snprintf(title, sizeof(title), "WATCH: %s MHZ", f);
        snprintf(t, sizeof(t), "ON A SPARE RTL OF ITS OWN, SQUELCHED: HEARD WHEN IT OPENS (THE X6200 OFF THE SPEAKER "
                               "MEANWHILE). F4 HELD AGAIN ON IT: NO LONGER.");
    } else if (r == 0) {
        snprintf(title, sizeof(title), "NOT WATCHED: %s MHZ", f);
        snprintf(t, sizeof(t), "ITS RTL IS FREE AGAIN (AIRSPACE MAY TAKE IT).");
    } else {
        snprintf(title, sizeof(title), "NO RTL FREE");
        snprintf(t, sizeof(t), "%s", rtl_watch_count() >= RTL_WATCH_N
                                         ? "4 WATCH CHANNELS ALREADY: F4 HELD ON ONE OF THEM DROPS IT."
                                         : "A WATCH CHANNEL NEEDS AN RTL OF ITS OWN, NOT B'S, ON A HUB WITH ITS OWN "
                                           "POWER (A KRAKENSDR'S).");
    }
    osd(title, t, -1);
    watch_show();
}

/* RDS: the station on B's channel when that is a broadcast FM one (WFM), decoded by the stick's daemon (rds.c) whether it
 * is heard or not. RBDS's call letters and programme types in the Americas (the band plan's region 2, the US plans) */
static int64_t rds_channel(void) {
    int64_t hz = scr == SCR_LOG && g_onr >= 0 ? g_hz : listen_hz;   /* C playing a stick entry: that one's, so C's
                                                                     * log learns its name */

    return hz && rtl_mode_of(hz) == 3 ? hz : 0;     /* WFM: broadcast FM */
}

static bool rds_rbds(void) {
    int plan = band_plan_now();

    return plan == BP_PLAN_R2 || plan >= BP_PLAN_US_EXTRA;
}

static void upper(char *t) {
    for (; *t; t++) *t = (char)toupper((unsigned char)*t);
}

/* The call letters from its PI (RBDS, the Americas; "" elsewhere or none) and its name (PS) trimmed, "" until whole */
static void rds_names(const rtl_rds_t *d, char call[5], char ps[9]) {
    call[0] = 0;
    if (rds_rbds()) {
        int pi = d->pi;
        if (pi >= 0x1000 && pi <= 0x994F) {         /* K from 0x1000, W from 0x54A8 (NRSC-4) */
            int k = pi >= 0x54A8 ? pi - 0x54A8 : pi - 0x1000;
            snprintf(call, 5, "%c%c%c%c", pi >= 0x54A8 ? 'W' : 'K', 'A' + k / 676, 'A' + k % 676 / 26, 'A' + k % 26);
        }
    }
    const char *p = d->ps;
    while (*p == ' ') p++;
    snprintf(ps, 9, "%s", p);
    for (int i = (int)strlen(ps); i > 0 && ps[i - 1] == ' '; i--) ps[i - 1] = 0;
    upper(ps);
}

/* "WOSU TESTFM12": the call letters (RBDS) and the name, or "PI 7B88" until the name came */
static bool rds_tuned_note(char *t, size_t n) {
    rtl_rds_t d;
    char      call[5], ps[9], *p = ps;

    if (!rtl_spectrum_rds(&d)) return false;
    rds_names(&d, call, ps);
    if (call[0] && *p) snprintf(t, n, "%s %s", call, p);
    else if (*p) snprintf(t, n, "%s", p);
    else if (call[0]) snprintf(t, n, "%s", call);
    else snprintf(t, n, "PI %04X", d.pi);
    upper(t);
    return true;
}

/* The Meteor decoder's state taken (each tick): its picture shown once a pass has lines and B's channel is on the
 * satellite, B's own history again otherwise */
static void met_update(void) {
    bool    got = rtl_spectrum_lrpt(&met), lines = false;
    int64_t sat = met_of(listen_hz);

    for (int c = 0; c < 6; c++) lines |= met.rows[c] > 0;
    bool on = got && sat && met.hz == sat && lines && spec_view() != VIEW_X6200;
    if (on && !met_px) met_px = malloc((size_t)RTL_COLS * (rtl_img_h > 0 ? rtl_img_h : 480) * sizeof(uint32_t));
    if (on == met_on) return;
    met_on      = on;
    met_ver_cpu = met_ver_gpu = UINT32_MAX;     /* drawn afresh */
    if (on && met_view && !met.rows[met_view - 1]) met_view = 0;
    if (!on) panel_render(&p_rtl);              /* the history again */
    rds_show(true);
}

/* F3 held on B: the picture's next view: colour (with 64 and 65 there), then each channel received */
static void met_next_view(void) {
    static const char *const what[6] = {"VISIBLE 0.5-0.7 UM", "NEAR INFRARED 0.7-1.1 UM", "INFRARED 1.6-1.8 UM",
                                        "INFRARED 3.5-4.1 UM", "THERMAL INFRARED 10.5-11.5 UM",
                                        "THERMAL INFRARED 11.5-12.5 UM"};
    char t[48];

    for (int n = 0; n < 7; n++) {
        met_view = (met_view + 1) % 7;
        if (met_view ? met.rows[met_view - 1] > 0 : met.rows[0] > 0 && met.rows[1] > 0) break;
    }
    met_ver_cpu = met_ver_gpu = UINT32_MAX;
    panel_render(&p_rtl);
    rtl_present();
    if (!met_view) {
        osd("METEOR: COLOUR", "CHANNELS 65 (RED, GREEN) AND 64 (BLUE). F3 HELD: THE NEXT VIEW.", -1);
    } else {
        snprintf(t, sizeof(t), "METEOR: CHANNEL %d", 63 + met_view);
        osd(t, what[met_view - 1], -1);
    }
    rds_show(true);
}

/* The decoder's line in RDS's place while B's channel is on a Meteor satellite: what it hears, and the pass's lines.
 * True: shown */
static bool met_shown;

static bool met_show(bool now) {
    static uint32_t seq = UINT32_MAX;
    char            f[16], line[220], chans[24] = "";
    int             lines = 0;
    size_t          at;
    int64_t         sat = met_of(listen_hz);

    if (!sat || !met.at_ms || met.hz != sat || lv_tick_elaps(met.at_ms) > 3000 || !p_rtl.trace_h ||
        (scr != SCR_RTL && scr != SCR_SCAN)) {
        if (met_shown) {
            met_shown = false;
            lv_obj_add_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
        }
        return false;
    }
    if (!now && met_shown && met.seq == seq) return true;
    seq = met.seq;
    for (int c = 0, k = 0; c < 6; c++) {
        if (met.rows[c] > lines) lines = met.rows[c];
        if (met.rows[c]) k += snprintf(chans + k, sizeof(chans) - (size_t)k, "%s%d", k ? " " : "", 64 + c);
    }
    fmt_mhz(f, sizeof(f), sat, 3);
    at = (size_t)snprintf(line, sizeof(line), "METEOR %s  %s %s  CARRIER %+.1f KHZ  LOCK %d%%", f,
                          met.lock ? "RECEIVING" : "LOOKING:", met.mode, met.offset / 1000.0, met.quality);
    if (met.frames)
        at += (size_t)snprintf(line + at, sizeof(line) - at, "\nFRAMES %u (%u GOOD)", met.frames, met.good);
    if (lines)
        at += (size_t)snprintf(line + at, sizeof(line) - at, "  LINES %d (%s)  %s", lines, chans,
                               !met_on ? "" : met_view ? "GREY" : "COLOUR");
    if (met.file[0]) {
        const char *b = strrchr(met.file, '/');
        snprintf(line + at, sizeof(line) - at, "  SAVED %s", b ? b + 1 : met.file);
    }
    upper(line);
    lv_label_set_text(rds_l, line);
    lv_obj_clear_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
    met_shown = true;
    return true;
}

/* DMR on B's channel (the daemon's dmr.c, found in any NFM channel by its sync): RDS's line says it while it is
 * there, each timeslot's call: who talks (the radio's ID) to whom (a talkgroup, TG, or a radio), TALKING while it goes
 * on. The user, 2026-11-16: "general DMR decode support (ensure the band selection can tune in)". True: shown */
static bool dmr_shown;

static bool dmr_show(bool now) {
    static uint32_t seq = UINT32_MAX, dseq = UINT32_MAX;
    static bool     was_dsd;
    rtl_dmr_t       m;
    rtl_dsd_t       ds;
    char            f[16], line[200];
    size_t          at;
    bool            dmr = rtl_spectrum_dmr(&m), dsd = !dmr && rtl_spectrum_dsd(&ds);

    if ((!dmr && !dsd) || !p_rtl.trace_h || (scr != SCR_RTL && scr != SCR_SCAN)) {
        if (dmr_shown) {                /* gone: RDS's line as it was, or none */
            dmr_shown = false;
            lv_obj_add_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
        }
        return false;
    }
    if (dsd != was_dsd) now = true;     /* DMR for the others, or the others for DMR: said afresh */
    was_dsd = dsd;
    if (dsd) {                          /* P25, D-STAR, YSF, X2-TDMA (the daemon's dsd-fme): who talks to whom */
        if (!now && dmr_shown && ds.seq == dseq) return true;
        dseq = ds.seq;
        fmt_mhz(f, sizeof(f), ds.hz, 3);
        at = (size_t)snprintf(line, sizeof(line), "%s %s", rtl_dsd_name(ds.proto), f);
        for (int i = 0, first = 1; i < 2 && at < sizeof(line); i++) {
            if (!ds.call[i][0]) continue;
            at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s%s TALKING", first ? "\n" : "   ",
                                   i ? "SLOT 2 " : "", ds.call[i]);
            first = 0;
        }
        lv_label_set_text(rds_l, line);
        lv_obj_clear_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
        dmr_shown = true;
        return true;
    }
    if (!now && dmr_shown && m.seq == seq) return true;
    seq = m.seq;
    fmt_mhz(f, sizeof(f), m.hz, 3);
    at = (size_t)snprintf(line, sizeof(line), "DMR %s", f);
    if (m.cc >= 0) at += (size_t)snprintf(line + at, sizeof(line) - at, "  COLOUR CODE %d", m.cc);
    at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s", m.bs ? "  REPEATER" : "",
                           m.vocoder ? "" : "  (NO VOCODER: SILENT)");
    static const int order[3] = {1, 2, 0};      /* TS1, TS2, then a radio's whose slot is not said */
    bool             first    = true;

    for (int i = 0; i < 3 && at < sizeof(line); i++) {
        const rtl_dmr_slot_t *sl = &m.slot[order[i]];
        char                  who[48] = "";

        if (!sl->at_ms) continue;
        if (sl->lc && sl->flco == 3) snprintf(who, sizeof(who), " TO %u FROM %u", sl->dst, sl->src);
        else if (sl->lc) snprintf(who, sizeof(who), " TG %u FROM %u", sl->dst, sl->src);
        at += (size_t)snprintf(line + at, sizeof(line) - at, "%s%s%s %s", first ? "\n" : "   ",
                               order[i] == 1 ? "TS1" : order[i] == 2 ? "TS2" : "TS?", who,
                               sl->voice ? "TALKING" : sl->lc ? "(ENDED)" : "IDLE");
        first = false;
    }
    lv_label_set_text(rds_l, line);
    lv_obj_clear_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
    dmr_shown = true;
    return true;
}

static void rds_show(bool now) {
    static uint32_t   seq = UINT32_MAX;
    static const char *const RBDS[32] = {
        "", "NEWS", "INFORMATION", "SPORTS", "TALK", "ROCK", "CLASSIC ROCK", "ADULT HITS", "SOFT ROCK", "TOP 40",
        "COUNTRY", "OLDIES", "SOFT", "NOSTALGIA", "JAZZ", "CLASSICAL", "R&B", "SOFT R&B", "LANGUAGE",
        "RELIGIOUS MUSIC", "RELIGIOUS TALK", "PERSONALITY", "PUBLIC", "COLLEGE", "SPANISH TALK", "SPANISH MUSIC",
        "HIP HOP", "", "", "WEATHER", "EMERGENCY TEST", "EMERGENCY"};
    static const char *const RDS[32] = {
        "", "NEWS", "CURRENT AFFAIRS", "INFORMATION", "SPORT", "EDUCATION", "DRAMA", "CULTURE", "SCIENCE", "VARIED",
        "POP MUSIC", "ROCK MUSIC", "EASY LISTENING", "LIGHT CLASSICAL", "SERIOUS CLASSICAL", "OTHER MUSIC", "WEATHER",
        "FINANCE", "CHILDREN'S", "SOCIAL AFFAIRS", "RELIGION", "PHONE IN", "TRAVEL", "LEISURE", "JAZZ", "COUNTRY",
        "NATIONAL MUSIC", "OLDIES", "FOLK MUSIC", "DOCUMENTARY", "ALARM TEST", "ALARM"};
    rtl_rds_t d;
    char      note[32], f[16], line[160];
    bool      got;

    if (!rds_l) return;
    bool was = dmr_shown;

    if (met_show(now)) return;          /* a Meteor satellite on B's channel: its decoder's line in RDS's place */
    if (dmr_show(now)) return;          /* DMR on B's channel: its line in RDS's place */
    if (was) now = true;                /* DMR gone: RDS's line again as it is */
    got = rtl_spectrum_rds(&d);
    if (!now && d.seq == seq) return;
    seq = d.seq;
    if (scr == SCR_RTL) show_rtl();         /* TUNED's second line */
    if (!got || !p_rtl.trace_h || (scr != SCR_RTL && scr != SCR_SCAN) || !rds_tuned_note(note, sizeof(note))) {
        lv_obj_add_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *pty = d.pty >= 0 && d.pty < 32 ? (rds_rbds() ? RBDS : RDS)[d.pty] : "";

    fmt_mhz(f, sizeof(f), d.hz, 3);
    /* two lines at most: a RadioText's 64 characters fit the width on its own */
    snprintf(line, sizeof(line), "RDS %s  %s%s%s%s%s%s", f, note, pty[0] ? "  " : "", pty, d.ta ? "  TRAFFIC" : "",
             d.rt[0] ? "\n" : "", d.rt);
    upper(line);
    lv_label_set_text(rds_l, line);
    lv_obj_clear_flag(rds_l, LV_OBJ_FLAG_HIDDEN);
}

/* The next preset (dir 1) or the one before that the stick tunes: a HackRF's 2.4 GHz is passed over with an RTL-SDR */
static void rtl_preset_next(int dir) {
    for (int n = 0; n < (int)RTL_PRESETS; n++) {
        rtl_preset = (rtl_preset + (dir > 0 ? 1 : (int)RTL_PRESETS - 1)) % (int)RTL_PRESETS;
        if (rtl_presets[rtl_preset].lo >= rtl_spectrum_min_hz() && rtl_presets[rtl_preset].hi <= rtl_spectrum_max_hz())
            break;
    }
}

/* B's presets: dir of them on (back when negative), tuned at once and named: BAND up/down, F2 held with the VFO knob */
static void rtl_preset_go(int dir) {
    f_band = -1;
    for (int n = abs(dir) < (int)RTL_PRESETS ? abs(dir) : (int)RTL_PRESETS; n > 0; n--) rtl_preset_next(dir);
    rtl_tune(rtl_presets[rtl_preset].hz);
    scan_next = 0;
    osd(rtl_presets[rtl_preset].name, rtl_presets[rtl_preset].about, -1);
    show_screen();
    b_tuned();
}

static void rtl_fkey(int k) {
    switch (k) {
        case 1:
            if (f_band >= 0) f_band = -1;           /* from C's band back to the presets, at the one it was on */
            else rtl_preset_next(1);
            rtl_tune(rtl_presets[rtl_preset].hz);
            scan_next = 0;              /* scanning: the new band, from its low end */
            osd(rtl_presets[rtl_preset].name, rtl_presets[rtl_preset].about, -1);  /* named, as A's F2 names its band */
            b_tuned();
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
    else if (vol_fn == VF_VOL && heard == HEAR_BOTH && rtl_spectrum_on_headphones())   /* both heard: this one's,
                                                                                     * and the other's to match it */
        snprintf(s, n, "%d (%s %d)", v, vol_stick() ? "X6200" : rtl_spectrum_dev_tag(),
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
    static const char *const mfk[SCR_N] = {"PAN, PUSH STRONGEST", "ROW, PUSH CHANGE", "BAND, PUSH LISTEN",
                                           "FREQ, PUSH OPEN", "ITEM, PUSH OPEN", "PAN, PUSH STRONGEST",
                                           "SIGNAL, PUSH LISTEN", "GROUP, PUSH CB 19"};

    char v[24];

    vol_text(v, sizeof(v));
    /* VOL names its radio: E said VOLUME 0 while A said VOLUME 2 (2026-11-17), the SDR's and the X6200's. Past the
     * line's 98 characters (RF GAIN's) without what its push gives: the VOL pop-up says it */
    char        l[160];
    const char *who = vol_stick() ? rtl_spectrum_dev_tag() : "X6200";

    snprintf(l, sizeof(l), "VFO: TUNE | MFK: %s | RING: SCREEN | VOL: %s %s %s, PUSH %s | GEN: HELP", mfk[scr], who,
             vol_short[vol_fn], v, vol_short[(vol_fn + 1) % 3]);
    if (strlen(l) > 98)
        snprintf(l, sizeof(l), "VFO: TUNE | MFK: %s | RING: SCREEN | VOL: %s %s %s | GEN: HELP", mfk[scr], who,
                 vol_short[vol_fn], v);
    lv_label_set_text(legend_l, l);
}

static void osd_hide_cb(lv_timer_t *t) {
    lv_timer_del(t);
    osd_timer = NULL;
    lv_obj_add_flag(osd_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_title_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_text_l, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_bar_frame, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(osd_bar, LV_OBJ_FLAG_HIDDEN);
    if (ab_arrow) lv_obj_add_flag(ab_arrow, LV_OBJ_FLAG_HIDDEN);
    if (ab_tag) lv_obj_add_flag(ab_tag, LV_OBJ_FLAG_HIDDEN);
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
    /* The SDR's, named as it is: the RTL's and the bladeRF's AUTO is their AGC, the HackRF has none (its daemon takes
     * a middle gain for AUTO) */
    static const char *const stick[] = {
        "THE %s'S OWN, WHEREVER IT IS HEARD (THE X6200 KEEPS ITS OWN). PUSH: ITS SQUELCH.",
        "THE %s'S: QUIET UNTIL A SIGNAL STANDS THIS MUCH OVER THE NOISE. OFF: ALWAYS OPEN. PUSH: RF GAIN.",
        "THE %s'S GAIN: UP FOR WEAK SIGNALS, DOWN IF STRONG ONES SWAMP IT. AUTO: %s. PUSH: VOLUME."};
    char t[48], v[24], who[12] = "", say[160];

    vol_text(v, sizeof(v));
    if (vol_fn == VF_VOL) snprintf(who, sizeof(who), "%s ", vol_stick() ? rtl_spectrum_dev_tag() : "X6200");
    snprintf(t, sizeof(t), "%s%s %s", who, vol_names[vol_fn], v);
    if (vol_stick())
        snprintf(say, sizeof(say), stick[vol_fn], rtl_spectrum_dev_tag(),
                 rtl_spectrum_dev() == RTL_DEV_HACKRF ? "A MIDDLE GAIN (IT HAS NO AGC)" : "ITS AGC");
    else
        snprintf(say, sizeof(say), "%s", how[vol_fn]);
    osd(t, say, vol_bar());
    show_legend();
}

static void vol_turn(int d) {
    int v;

    vol_used_ms = lv_tick_get();
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
        static signed char was[5] = {-1, -1, -1, -1, -1};    /* lit (1), dimmed (0) as last shown */
        signed char        lit    = l[0] && !btn[i].disabled;

        if (!strcmp(lv_label_get_text(key_l[i]), t) && was[i] == lit + 2 * btn[i].disabled) continue;  /* only what
                                                                    * changed: this runs each half second (blink_cb) */
        was[i] = lit + 2 * btn[i].disabled;
        lv_label_set_text(key_l[i], t);
        lv_obj_set_style_pad_top(key_l[i], strchr(t, '\n') ? 2 : 12, 0);
        lv_obj_set_style_text_color(key_l[i], lit ? amb : dim, 0);     /* dimmed: nothing to do */
        lv_obj_set_style_border_color(key_l[i], btn[i].disabled ? dim : amb, 0);
    }
}

static char scr_letter(scr_t s) {
    static const char letters[SCR_N] = {'A', 'A', 'D', 'E', 'F', 'B', 'C', 'G'};  /* by scr_t; SCR_TRACK: its screen's */

    return s == SCR_TRACK ? letters[cfg_from] : letters[s];
}

/* The F keys dimmed (and refused) while they can do nothing: C's Listen, Remove and Clear with an empty log (its Scan
 * and Dwell always act: A's and B's scans, which fill it). The other screens' all lit */
static void keys_dim(void) {
    bool changed = false;

    for (int k = 0; k < 5; k++) {
        bool off = scr == SCR_LOG && k <= 2 && nhits == 0;

        if (btn[k].disabled != off) {
            buttons_disabled(&btn[k], off);
            changed = true;
        }
    }
    if (changed && key_l[0]) show_keys();   /* the Sentry's own keys, over the bar's */
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
    keys_dim();
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

const char *app_sentry_release(void) {
    static char s[48];

    if (!s[0]) snprintf(s, sizeof(s), "%s %s", strrchr(VERSION, ' ') + 1, GIT_COMMIT);
    return s;
}

/* A title with the release, as every screen's: "XIEGU X6200 <name>: v0.3.12.19 f739c40<more>", without "XIEGU "
 * where that is wider than w px in font f */
static void title_text(char *s, size_t n, const char *name, const char *more, const lv_font_t *f, int w) {
    snprintf(s, n, "XIEGU X6200 %s: %s%s", name, app_sentry_release(), more ? more : "");
    if (lv_txt_get_width(s, strlen(s), f, 0, LV_TEXT_FLAG_NONE) > w) memmove(s, s + 6, strlen(s + 6) + 1);
}

/* The title: the screen's, or a weather alert's event while it lasts, blinking */
static void show_title(void) {
    if (alert_until && time(NULL) < alert_until && (time(NULL) & 1))
        lv_label_set_text_fmt(title_l, "WX ALERT: %s", alert_event);
    else {
        /* every screen's name, then the release (src/main.h's version, the tag it is built for) and the commit; F
         * SYSTEM adds the base's firmware, "... SYSTEM CONTROL: v0.3.9.4 c6e93af - base 1.0.8". One line between the
         * badges, 68 of pixel_20's 10 px characters: a long one gives up "XIEGU " for it */
        const char *t = titles[scr], *ver = radio_base_ver_str();
        char        s[128], more[24] = "";

        if (strstr(t, "SYSTEM")) snprintf(more, sizeof(more), " - base %s", ver && ver[0] ? ver : "?");
        title_text(s, sizeof(s), t, more, lv_obj_get_style_text_font(title_l, 0),
                   lv_obj_get_style_width(title_l, LV_PART_MAIN));     /* the width given, laid out or not yet */
        lv_label_set_text(title_l, s);
    }
}

/* ---- G LOG: what the scans locked on ---- */

#define LOG_ROW "%c%-10.10s %-8.8s %-22.22s %s %-16.16s %-5.5s %4.4s"  /* 78 in pixel_16 (BEST 5, given padded); RX
                                                                         * after it in pixel_12: 25 of 26 */
#define LOG_RX_X  (9 + 78 * 8)  /* where RX starts: the row's 78 characters of 8 px */
#define G_STICK_OFF 200000      /* the stick tuned this far under what it plays: its middle is not used */

static void hits_save(void) {
    char  tmp[sizeof(HITS_FILE) + 4];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.new", HITS_FILE);
    if (!(f = fopen(tmp, "w"))) return;
    for (int i = 0; i < nhits; i++)
        fprintf(f, "%lld\t%d\t%d\t%lld\t%lld\t%d\t%.1f\t%s\n", (long long)hits[i].hz, hits[i].mode,
                hits[i].rtl ? (hits[i].sdr > 0 ? hits[i].sdr : 1) : 0,
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
        h.sdr   = (int8_t)(rtl > 0 && rtl < 18 ? rtl : rtl ? 1 : 0);
        h.first = (time_t)first;
        h.last  = (time_t)last;
        snprintf(h.what, sizeof(h.what), "%s", line + n);
        h.what[strcspn(h.what, "\r\n")] = 0;
        hits[nhits++] = h;
    }
    fclose(f);
}

/* The SDR a log entry was found on, kept in the log file's RX number: 0 the X6200, 1 an SDR not named (a log of
 * before 2026-11-17), 2 the HackRF, 3 the bladeRF, 10 + n RTLn. A GUI of before reads any but 0 as the stick's */
static int sdr_code(void) {
    rtl_dev_t d = rtl_spectrum_dev();

    return d == RTL_DEV_HACKRF ? 2 : d == RTL_DEV_BLADERF ? 3 : d == RTL_DEV_RTL ? 10 + rtl_spectrum_dev_rtl() : 1;
}

static const char *sdr_code_tag(int c) {     /* its kind, no stick number (the owner: "drop the 4") */
    return c == 2 ? "HACKRF" : c == 3 ? "BLADERF" : c >= 10 ? "RTL" : "SDR";
}

/* Every kind of receiver that takes hz, as C's RX column names them (the owner, 2026-11-17: "all *Valid* reception
 * types", no stick numbers, the WR12 too; HACK and BLADE, "leave off the RF"): the X6200 (0.1-55 MHz, and FM broadcast
 * and the airband, 87.5-137), an RTL-SDR (24-1766 MHz), a HackRF (1 MHz-6 GHz), a bladeRF (70 MHz-6 GHz), the WR12
 * (54 MHz-1 GHz). Those here now (the X6200; an SDR of that kind on USB; a WR12 set up) in green, the others in red,
 * unless lit is false (the chosen row, in its ink) */
enum { RXK_X6200, RXK_RTL, RXK_HACKRF, RXK_BLADERF, RXK_WR12, RXK_N };

static bool rxk_takes(int k, int64_t hz) {
    int64_t lo, hi;

    switch (k) {
        case RXK_X6200: return (hz >= BOARD_HF_MIN_HZ && hz <= BOARD_HF_MAX_HZ) || (hz >= 87500000 && hz <= 137000000);
        case RXK_WR12: return hz >= 54000000 && hz <= 1000000000;
        default:
            rtl_spectrum_kind_range(k == RXK_RTL ? RTL_DEV_RTL : k == RXK_HACKRF ? RTL_DEV_HACKRF : RTL_DEV_BLADERF,
                                    &lo, &hi);
            return hz >= lo && hz <= hi;
    }
}

static void rx_kinds_text(int64_t hz, bool lit, char *s, size_t n) {
    static const char *const name[RXK_N] = {"X6200", "RTL", "HACK", "BLADE", "WR12"};
    bool       here[RXK_N] = {true, false, false, false, wr12_here};
    sdr_unit_t u[SDR_POOL_UNITS];
    int        un = lit ? sdr_pool_units(u, SDR_POOL_UNITS) : 0;
    size_t     at = 0;

    for (int j = 0; j < un; j++) {
        if (u[j].kind == RTL_DEV_RTL) here[RXK_RTL] = true;
        if (u[j].kind == RTL_DEV_HACKRF) here[RXK_HACKRF] = true;
        if (u[j].kind == RTL_DEV_BLADERF) here[RXK_BLADERF] = true;
    }
    s[0] = 0;
    for (int k = 0; k < RXK_N && at < n; k++) {
        if (!rxk_takes(k, hz)) continue;
        if (lit)
            at += (size_t)snprintf(s + at, n - at, "%s#%06X %s#", at ? " " : "",
                                   (unsigned)theme_hex(here[k] ? TC_avail_on : TC_avail_off), name[k]);
        else
            at += (size_t)snprintf(s + at, n - at, "%s%s", at ? " " : "", name[k]);
    }
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
        if (rtl) h.sdr = (int8_t)sdr_code();    /* heard again: on the SDR it was heard on now */
        if (peak > h.peak) h.peak = peak;
    } else {
        memset(&h, 0, sizeof(h));
        h.hz    = hz;
        h.mode  = mode;
        h.rtl   = rtl;
        h.sdr   = (int8_t)(rtl ? sdr_code() : 0);
        h.first = h.last = now;
        h.times = 1;
        h.peak  = peak;
        snprintf(h.what, sizeof(h.what), "%s", what ? what : "");
        if (nhits < HITS) nhits++;
        j = nhits - 1;                  /* full: the oldest goes */
        if (g_onx == j) g_onx = -1;
        if (g_onr == j) g_onr = -1;
        if (g_pick == j) g_pick = -1;
    }
    memmove(&hits[1], &hits[0], (size_t)j * sizeof(hit_t));
    hits[0] = h;
    if (g_sel == j) g_sel = 0;
    else if (g_sel < j) g_sel++;
    if (g_onx == j) g_onx = 0;
    else if (g_onx >= 0 && g_onx < j) g_onx++;
    if (g_onr == j) g_onr = 0;
    else if (g_onr >= 0 && g_onr < j) g_onr++;
    if (g_pick == j) g_pick = 0;
    else if (g_pick >= 0 && g_pick < j) g_pick++;
    hits_dirty = true;
    if (scr == SCR_LOG) {
        show_glog();
        keys_dim();                     /* the first one in: C's keys lit */
    }
}

/* The stick (running for the whole Sentry: rtl_spectrum.h) tuned for G's entry hz */
static void g_stick_on(int64_t hz) {
    if (!g_stick) rtl_spectrum_squelch(param_i_get(cfg_rtl_sql));
    rtl_spectrum_tune(hz + G_STICK_OFF);
    stick_at = hz + G_STICK_OFF;
    g_stick  = true;
}

static int g_stick_mode(const hit_t *h);

/* The speaker for the entries C plays (g_onx, g_onr): both, the two mixed (BOTH); the stick's alone, the stick (mixed
 * with the X6200 if BOTH was the LISTEN); the X6200's alone, the X6200 (BOTH: the stick on B's channel mixed in);
 * none, the LISTEN of before back */
static void g_route(void) {
    bool x = g_onx >= 0, r = g_onr >= 0;

    if (!x && !r) {
        if (g_hear >= 0) hear_apply(g_hear);
        g_hear  = -1;
        g_stick = false;                /* the stick B's again */
        show_screen();
        return;
    }
    if (g_hear < 0) g_hear = hear_want; /* the LISTEN in force, given back when the log stops */
    if (r) {
        bool both = x || g_hear == HEAR_BOTH;
        int  m    = g_stick_mode(&hits[g_onr]);

        heard = hear_want = both ? HEAR_BOTH : HEAR_RTL;
        hear_vol_refresh();
        rtl_spectrum_mute(false);
        rtl_spectrum_mux(both);
        rtl_spectrum_listen(g_hz, m);
    } else {
        bool both = g_hear == HEAR_BOTH && (sdr_here || rtl_spectrum_state() == RTL_SPEC_ON);

        if (both) {                     /* the X6200 on the entry, the stick on B's channel (B's scan) mixed in */
            if (heard != HEAR_BOTH || g_stick) hear_apply(HEAR_BOTH);
        } else if (heard != HEAR_X6200) {
            hear_apply(HEAR_X6200);     /* the X6200 on the speaker again (OFF too) */
        }
        hear_want = both ? HEAR_BOTH : HEAR_X6200;
        g_stick   = false;
    }
    show_screen();
}

/* The radios here that take entry i, for C's Listen (the owner: "the hear it on should list sources that work. and
 * when selected we should get to choose which ones tune in, or have all tune"): the X6200 (with a WR12 on, above its
 * own receiver too) and B's SDR (the one B has: RTL, HACK or BLADE) */
static bool g_x_takes(int i) {
    int64_t hz = hits[i].hz;

    return hz > 0 && hz <= INT32_MAX && cfg_is_valid_hw_freq((int32_t)hz);
}

static bool g_r_takes(int i) {
    int64_t   lo, hi, hz = hits[i].hz;
    rtl_dev_t d = rtl_spectrum_dev();

    if (!sdr_here || d == RTL_DEV_NONE) return false;
    rtl_spectrum_kind_range(d, &lo, &hi);
    return hz >= lo && hz + G_STICK_OFF <= hi;      /* the stick sits G_STICK_OFF above what it plays */
}

static const char *g_x_name(int i) {
    return rxk_takes(RXK_X6200, hits[i].hz) ? "X6200" : "WR12";
}

static const char *g_r_name(void) {
    rtl_dev_t d = rtl_spectrum_dev();

    return d == RTL_DEV_HACKRF ? "HACK" : d == RTL_DEV_BLADERF ? "BLADE" : "RTL";
}

/* An entry's mode on the other radio: the stick's demodulator (1 NFM, 2 AM, 3 WFM) for the X6200's mode, the X6200's
 * mode for the stick's; its band's own when it has none */
static int g_stick_mode(const hit_t *h) {
    if (h->rtl) return h->mode > 0 ? h->mode : rtl_mode_of(h->hz);
    return h->mode == radio_mode_am ? 2 : h->mode == radio_mode_nfm ? 1 : h->mode == radio_mode_wfm ? 3
                                                                                               : rtl_mode_of(h->hz);
}

static int g_x_mode(const hit_t *h) {
    if (!h->rtl) return h->mode;
    int m = h->mode > 0 ? h->mode : rtl_mode_of(h->hz);

    return m == 2 ? radio_mode_am : m == 3 ? radio_mode_wfm : radio_mode_nfm;
}

/* Entry i on the radios asked (x the X6200, r B's SDR), each in place of the one it played; a radio not asked goes on
 * with its own */
static void g_play_on(int i, bool x, bool r) {
    if (i < 0 || i >= nhits) return;
    const hit_t *h = &hits[i];

    if (r) {
        g_hz = h->hz;
        g_stick_on(g_hz);
        g_onr = i;
    }
    if (x) {
        if (!g_x_takes(i) || main_screen_freq_locked()) {
            tune((int32_t)h->hz, g_x_mode(h));      /* says why */
        } else {
            if (trk.on) track_end();    /* A's lock ended first: its end (AUTO) tuned back to its place while C played
                                         * this one */
            if (tune((int32_t)h->hz, g_x_mode(h))) g_onx = i;
        }
    }
    g_route();
}

/* Entry i on the radio that found it (the log's scan, each radio its own entries) */
static void g_play(int i) {
    if (i < 0 || i >= nhits) return;
    if (hits[i].rtl && !sdr_here) {
        osd("NO SDR", "THIS ONE WAS HEARD ON AN SDR: PLUG AN RTL-SDR, HACKRF OR BLADERF INTO THE HOST PORT.", -1);
        return;
    }
    g_play_on(i, !hits[i].rtl, hits[i].rtl);
}

/* Another entry chosen while some play: on the radios playing that take it, else on the one that found it */
static void g_follow(int i) {
    bool x = g_onx >= 0 && g_x_takes(i), r = g_onr >= 0 && g_r_takes(i);

    if (x || r) g_play_on(i, x, r);
    else g_play(i);
}

/* Listen on entry i by hand: on the one radio here that takes it, or asked which when two do; none, who would */
static void g_listen(int i) {
    bool x = g_x_takes(i), r = g_r_takes(i);

    if (x && r) {
        g_pick = i;
        show_screen();
    } else if (x || r) {
        g_play_on(i, x, r);
    } else {
        char k[48], t[160];

        rx_kinds_text(hits[i].hz, false, k, sizeof(k));
        snprintf(t, sizeof(t), "%s TAKE IT; NONE OF THEM IS HERE NOW (B'S SDR: %s).", k,
                 sdr_here ? g_r_name() : "NONE PLUGGED IN");
        osd("NO RECEIVER FOR IT", t, -1);
    }
}

/* Entry i stopped on every radio playing it */
static void g_stop_entry(int i) {
    if (g_onx == i) g_onx = -1;
    if (g_onr == i) g_onr = -1;
    g_route();
}

static void g_stop_all(void) {
    g_onx = g_onr = -1;
    g_route();
}

static bool g_playing(int i) {
    return i >= 0 && (i == g_onx || i == g_onr);
}

/* Entry i by hand (Listen, the MFK's push, a touch): heard if it was not, stopped if it was. The log's own scans stop
 * (A's and B's go on) */
static void g_toggle(int i) {
    if (i < 0 || i >= nhits) return;
    g_scan = false;
    if (g_pick >= 0) {                  /* asked which radios: the push again is ALL */
        int p  = g_pick;

        g_pick = -1;
        if (p == i) {
            g_play_on(i, true, true);
            return;
        }
    }
    if (g_playing(i)) g_stop_entry(i);
    else g_listen(i);
}

/* C's radio: the stick while it plays a stick entry and that one is chosen, or nothing of the X6200's plays */
static bool g_stick_side(void) {
    return scr == SCR_LOG && g_onr >= 0 && (g_onx < 0 || (g_sel < nhits && hits[g_sel].rtl));
}

/* The VFO while G plays the stick: a channel a click, the entry kept as it was */
static void g_turn(int32_t diff) {
    g_hz = rtl_snap(g_hz + diff * rtl_step_of(g_hz));
    if (g_hz < rtl_spectrum_min_hz() + G_STICK_OFF) g_hz = rtl_spectrum_min_hz() + G_STICK_OFF;
    if (g_hz > rtl_spectrum_max_hz() - G_STICK_OFF) g_hz = rtl_spectrum_max_hz() - G_STICK_OFF;
    rtl_spectrum_tune(g_hz + G_STICK_OFF);
    rtl_spectrum_listen(g_hz, rtl_mode_of(g_hz));
    show_head();
}

/* What C played when it is left is where A and B are when shown (the user, 2026-11-15: "anytime something is set on
 * C, the transition survives to A and B"): the X6200's entry (or where the VFO took it on C) A's place, its lock's
 * place to come back to, the stick's B's channel, B's picture over it (to it, and its preset, if it was out of
 * view). Chosen by hand, that radio's own scan (A's band scan or sweep, B's scan) stops there, as a channel chosen
 * by hand on B stops B's; the log's scan (C's F4) leaves A's and B's scans on, from there */
static void g_hand_over(void) {
    bool by_hand = !g_scan;

    if (g_onx >= 0) {
        if (by_hand && routine != ROUT_BAND) {
            routine     = ROUT_BAND;
            sweep_next  = 0;
            scan_a_next = 0;
        }
        if (routine == ROUT_BAND) anchor = cparam_i_get(cfg_fg_freq);   /* a lock ends back there (track_end) */
    }
    if (g_onr >= 0 && g_hz && (by_hand || !rtl_scan)) {    /* B's scan going on: its own band and picture */
        int64_t hz = rtl_snap(g_hz);

        if (by_hand) rtl_scan_set(false);
        if (llabs(hz - rtl_hz) > rtl_view_hz() / 2 * 85 / 100) {
            if (hz < f_lo() || hz > f_hi()) {   /* out of B's band: the preset it is in, if one */
                f_band = -1;
                for (int i = RTL_PRESETS - 1; i >= 0; i--)
                    if (hz >= rtl_presets[i].lo && hz <= rtl_presets[i].hi) rtl_preset = i;
            }
            rtl_tune_to(hz, true);      /* as B's own VFO does at its picture's edge */
        }
        listen_hz = hz;
    }
}

/* G left: its scan stops; what it played goes on until A or F is shown (they set the speaker: hear_apply), as on
 * every screen that is not a spectrum, and stays A's and B's (g_hand_over). The stick runs on; F retunes it when
 * shown */
static void g_leave(bool to_f) {
    g_hand_over();
    if ((g_onx >= 0 || g_onr >= 0) && g_hear >= 0 && !to_f) hear_apply(g_hear);     /* B (to_f) sets its own */
    g_stick = false;
    g_onx = g_onr = g_pick = -1;
    g_hear  = -1;                       /* F4's scans stay on: A's and B's go on, the log's again when C is back */
}

/* G opened: the stick tuned to its first entry of the stick's, so that one plays without waiting */
static void g_enter(void) {
    g_nx = g_nr = 0;                        /* the log's scans (F4 on) at once */
    if (!sdr_here || listen_rtl) return;    /* the stick heard (B's channel, BOTH): left where it is */
    for (int i = 0; i < nhits; i++)
        if (hits[i].rtl) {
            g_stick_on(hits[i].hz);
            return;
        }
}

/* The log's scan on each radio, at once (C shown, F4 on): that radio's next entry, held while it is busy (the X6200's
 * over HEARD_DB in the picture, the stick's with its squelch open; no squelch, it only dwells), each quiet one
 * scan_dwell_ms. A radio with nothing in the log is left to its own scan (A's, B's) */
static bool g_busy(int i) {
    if (i < 0 || i >= nhits) return false;
    if (hits[i].rtl) return param_i_get(cfg_rtl_sql) > 0 && rtl_spectrum_squelch_open();
    return nbins && con_at(hits[i].hz, 2) >= HEARD_DB;
}

static void g_scan_radio(bool rtl, uint32_t now) {
    int      *on = rtl ? &g_onr : &g_onx;
    uint32_t *nx = rtl ? &g_nr : &g_nx;

    if (now < *nx || (rtl && !sdr_here)) return;
    if (*on >= 0 && g_busy(*on)) {
        *nx = now + 1500;               /* someone there: stay, and look again */
        return;
    }
    int from = *on >= 0 ? *on : nhits - 1;

    for (int k = 1; k <= nhits; k++) {
        int i = (from + k) % nhits;

        if (hits[i].rtl != rtl) continue;
        if (i != *on) {
            g_sel = i;
            g_play(i);
        }
        break;
    }
    *nx = lv_tick_get() + scan_dwell_ms;
}

static void g_scan_step(void) {
    uint32_t now = lv_tick_get();

    if (!g_scan || scr != SCR_LOG || !nhits) return;
    g_scan_radio(false, now);
    g_scan_radio(true, now);
}

/* F4 on C: every scan at once, on or off. A's (AUTO) and B's find and fill the log, on whatever screen; the log's
 * scan on each radio plays its entries while C is shown (that radio's own scan waits meanwhile: one frequency at a
 * time). Off: all of them stop, what the log played with them */
static bool g_scanning(void) {
    return g_scan || routine == ROUT_SCAN || rtl_scan;
}

static void g_scan_set(bool on) {
    g_scan = on;
    if (on) {
        int i = a_band_now(centre ? centre : dial_centre());    /* A: the dial's band end to end (A's F1), AUTO */

        if (routine != ROUT_SCAN && i >= 0 && cfg_is_valid_hw_freq(bands[i].listen_hz)) {
            scan_a_band = i;
            set_routine(ROUT_SCAN);
        }
        if (smode != MODE_AUTO) set_mode(MODE_AUTO);
        if (sdr_here) rtl_scan_set(true);
        g_nx = g_nr = 0;
    } else {
        if (routine == ROUT_SCAN) set_routine(ROUT_BAND);
        if (smode != MODE_MAN) scan_mode_sync();       /* the style's own again (it set SEMI before) */
        rtl_scan_set(false);
        g_stop_all();
    }
    show_screen();
}

static void g_remove(int i) {
    if (i < 0 || i >= nhits) return;
    if (g_playing(i)) g_stop_entry(i);
    g_pick = -1;
    memmove(&hits[i], &hits[i + 1], (size_t)(nhits - i - 1) * sizeof(hit_t));
    nhits--;
    if (g_onx > i) g_onx--;
    if (g_onr > i) g_onr--;
    if (g_sel >= nhits) g_sel = nhits ? nhits - 1 : 0;
    hits_dirty = true;
}

/* A call sign in t, a word of its own ("WNCI" in "WNCI -", "WOSU" in "WOSU-FM"): K or W and two or three letters,
 * into out; false none */
static bool call_in(const char *t, char out[5]) {
    while (*t) {
        int n = 0;

        while (t[n] >= 'A' && t[n] <= 'Z') n++;
        if ((n == 3 || n == 4) && (t[0] == 'K' || t[0] == 'W') && !(t[n] >= '0' && t[n] <= '9')) {
            memcpy(out, t, n);
            out[n] = 0;
            return true;
        }
        t += n ? n : 1;
        while (*t && (*t == ' ' || *t == '-' || *t == '.' || *t == '/')) t++;
    }
    return false;
}

/* C's FM broadcast entries named by RDS (the user, 2026-11-15: "add RDS to what it was for FM radio stations. just
 * the station name"): "FM" and the name, on the stick's entries and the X6200's at that channel, from the channel
 * watched (B's, or the stick entry C plays). Its PS when it has stood RDS_STEADY_MS ("WOSU-FM"); many US stations
 * scroll the song through it a few seconds a piece (97.9 WNCI: "WNCI -", "IT ALL -", "SWIMS", "97.9", 4 s each, the
 * first whole one taken was "IT ALL -"), so in the Americas a call sign among its pieces names it at once (WNCI),
 * and a steady PS then replaces that only when it holds the call. No call letters from the PI: WNCI's reads KCTC */
#define RDS_STEADY_MS 10000
static void g_rds_take(void) {
    static int64_t  at_hz;
    static char     last[9];
    static uint32_t since;
    rtl_rds_t       d;
    char            pi_call[5], ps[9], call[5], had[5], steady[sizeof(hits[0].what)] = "", by_call[sizeof(hits[0].what)] = "";
    bool            changed = false;

    if (!rtl_spectrum_rds(&d)) {
        at_hz = 0;
        return;
    }
    rds_names(&d, pi_call, ps);
    if (d.hz != at_hz || strcmp(ps, last)) {    /* a new channel, or its PS changed: steady from now */
        at_hz = d.hz;
        snprintf(last, sizeof(last), "%s", ps);
        since = lv_tick_get();
    }
    if (ps[0] && lv_tick_get() - since >= RDS_STEADY_MS) snprintf(steady, sizeof(steady), "FM %s", ps);
    if (ps[0] && rds_rbds() && call_in(ps, call)) snprintf(by_call, sizeof(by_call), "FM %s", call);
    if (!steady[0] && !by_call[0]) return;
    for (int i = 0; i < nhits; i++) {
        char *w = hits[i].what;
        bool  plain, named_by_call;

        /* an FM broadcast channel's entry: "FM", "FM BROADCAST 97.1 MHZ", or a name of before */
        if (rtl_mode_of(hits[i].hz) != 3 || llabs(hits[i].hz - d.hz) > 50000 || (w[0] && strncmp(w, "FM", 2))) continue;
        plain         = !w[0] || !strcmp(w, "FM") || !strncmp(w, "FM BROADCAST", 12);
        named_by_call = !plain && call_in(w + 2, had);
        const char *to = steady[0] && (plain || !named_by_call || strstr(ps, had)) ? steady
                       : by_call[0] && (plain || !named_by_call) ? by_call : NULL;

        if (!to || !strcmp(w, to)) continue;
        snprintf(w, sizeof(hits[i].what), "%s", to);
        changed = true;
    }
    if (!changed) return;
    hits_dirty = true;
    if (scr == SCR_LOG) show_glog();
}

static void show_glog(void) {
    char s[240];
    int  top = g_sel - lg_rows / 2;

    if (top > nhits - lg_rows) top = nhits - lg_rows;
    if (top < 0) top = 0;
    for (int r = 0; r < lg_rows; r++) {
        int i = top + r;

        lv_obj_set_style_bg_opa(lg_row_l[r], i == g_sel && i < nhits ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lg_row_l[r], i == g_sel ? ink : amb, 0);
        if (i >= nhits) {
            lv_label_set_text(lg_row_l[r], "");
            lv_label_set_text(lg_rx_l[r], "");
            continue;
        }
        const hit_t *h = &hits[i];
        bool         in;
        int          bi = band_near(h->hz, &in);
        char         f[16], last[8], bn[12];
        struct tm    tm;

        snprintf(f, sizeof(f), "%lld.%03lld.%02lld", (long long)(h->hz / 1000000), (long long)(h->hz / 1000 % 1000),
                 (long long)(h->hz % 1000 / 10));
        localtime_r(&h->last, &tm);
        strftime(last, sizeof(last), "%H:%M", &tm);
        char n[8];
        snprintf(n, sizeof(n), "%d", h->times);
        snprintf(bn, sizeof(bn), "%s%s", in ? "" : "~", bi >= 0 ? bands[bi].name : "-");
        char rxs[96], b5[8], best[24];
        rx_kinds_text(h->hz, i != g_sel, rxs, sizeof(rxs));    /* every kind that takes it; the chosen row in its ink */
        snprintf(b5, sizeof(b5), "%-5.5s", bi >= 0 ? best_name(bands[bi].when) : "");
        int role = bi >= 0 && i != g_sel ? band_now_role(bi) : -1;    /* NOW: green open, yellow closing, red closed */
        if (role >= 0) snprintf(best, sizeof(best), "#%06X %s#", (unsigned)theme_hex((theme_color_t)role), b5);
        else snprintf(best, sizeof(best), "%s", b5);
        snprintf(s, sizeof(s), LOG_ROW, g_playing(i) ? '>' : ' ', f, bn, h->what, best, bi >= 0 ? bands[bi].ant : "",
                 last, n);
        lv_label_set_text(lg_rx_l[r], rxs);
        lv_obj_set_style_text_color(lg_rx_l[r], i == g_sel ? ink : amb, 0);
        lv_label_set_text(lg_row_l[r], s);
    }
    if (!nhits) {
        lv_label_set_text(lg_about_l, "NOTHING LOCKED ON YET: F4 STARTS A'S AND B'S SCANS (A'S F1 WITH AUTO, B'S F1 "
                                      "TOO), WHICH PUT WHAT THEY STOP ON HERE.");
        return;
    }
    const hit_t *h = &hits[g_sel];
    char         a[16], b[16];
    struct tm    tm;

    localtime_r(&h->first, &tm);
    strftime(a, sizeof(a), "%H:%M %b %d", &tm);
    localtime_r(&h->last, &tm);
    strftime(b, sizeof(b), "%H:%M %b %d", &tm);
    char on[24], push[64];
    bool x = g_x_takes(g_sel), r = g_r_takes(g_sel);

    snprintf(on, sizeof(on), "%s", h->rtl ? sdr_code_tag(h->sdr) : "X6200");
    if (g_pick == g_sel) {              /* which radios: the line says the keys */
        snprintf(s, sizeof(s), "HEAR %s ON: F1 ALL (THE %s AND THE %s TUNE IN TOGETHER), F2 THE %s ONLY, F3 THE %s "
                               "ONLY. PUSH AGAIN: ALL. F5: BACK",
                 h->what[0] ? h->what : "IT", g_x_name(g_sel), g_r_name(), g_x_name(g_sel), g_r_name());
        lv_label_set_text(lg_about_l, s);
        return;
    }
    if (x && r) snprintf(push, sizeof(push), "PUSH: HEAR IT ON THE %s, THE %s OR BOTH", g_x_name(g_sel), g_r_name());
    else if (x) snprintf(push, sizeof(push), "PUSH: HEAR IT ON THE %s", g_x_name(g_sel));
    else if (r) snprintf(push, sizeof(push), "PUSH: HEAR IT ON THE %s", g_r_name());     /* B's SDR now */
    else snprintf(push, sizeof(push), "NO RECEIVER HERE TAKES IT");
    snprintf(s, sizeof(s), h->rtl ? "%s: FOUND ON %s, FIRST %s, LAST %s, %d TIME%s, %.0f DB. %s"
                                  : "%s: FOUND ON %s, FIRST %s, LAST %s, %d TIME%s, +%.0f DB. %s",
             h->what[0] ? h->what : "THIS ONE", on, a, b, h->times, h->times == 1 ? "" : "S", h->peak,
             g_scan ? "SCANNING THE LIST ON EACH RADIO (F4: STOP)" : g_playing(g_sel) ? "HEARD NOW (PUSH: STOP)" : push);
    for (char *c = s; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    lv_label_set_text(lg_about_l, s);
}

static void go(scr_t s) {
    if (scr == SCR_LOG && s != SCR_LOG) g_leave(s == SCR_RTL);
    scr_t was = scr;

    scr = s;
    if (was == SCR_WALKIE && s != SCR_WALKIE) w_sdr_left();    /* the SDR's transmitter off, the X6200 heard (A and
                                                                * B put their own LISTEN on below) */
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
    rtl_spectrum_scope(s == SCR_WALKIE && w_sdr_hz);   /* the stick's: G's on its own channels */
    if (was != s && (was == SCR_WALKIE || s == SCR_WALKIE)) walkie_show(s == SCR_WALKIE);
    if (was != s && s == SCR_WALKIE) walkie_enter();       /* on a channel, never "--" */
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
    trk.held      = auto_ && scan_stay == STAY_HOLD;      /* STOP & STAY: there till F2 or F3 in SCAN CFG */
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
    /* on a lock: as SCAN CFG's STAY ON A SIGNAL and GO ON AFTER say (DOESN'T STOP, with a lock C's log scan made AUTO
     * for: the step time) */
    uint32_t stay = scan_stay == STAY_NONE ? (uint32_t)scan_dwell_ms : stay_ms[scan_stay];

    if (smode == MODE_AUTO && !trk.held &&
        (now - trk.last_seen > quiet_ms[scan_quiet] || (stay != UINT32_MAX && now - trk.start > stay)))
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
    scan_a_next = now + scan_dwell_ms;
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
    int i = a_band_now(centre ? centre : dial_centre());

    if (i < 0 || !cfg_is_valid_hw_freq(bands[i].listen_hz)) {
        osd("NO BAND HERE", "F2 OR THE BAND KEYS CHOOSE ONE; F1 THEN SCANS IT END TO END.", -1);
        return;
    }
    scan_a_band = i;
    if (smode == MODE_MAN) smode = scan_stay == STAY_NONE ? MODE_SEMI : MODE_AUTO;     /* the style's own again */
    set_routine(ROUT_SCAN);
    char t[24];
    snprintf(t, sizeof(t), "SCAN: %s", bands[i].name);
    osd(t, smode == MODE_AUTO ? "THE BAND END TO END, 360 KHZ AT A TIME, STOPPING ON WHAT IT FINDS AS SCAN CFG'S "
                                "STYLE SAYS."
                              : "THE BAND END TO END, 360 KHZ AT A TIME, MARKING WHAT IT FINDS (BROWSE: SCAN CFG'S "
                                "STYLE MAKES IT STOP).", -1);
}

/* AUTO: lock on the strongest target not ignored nor seen lately, in the routine's place; SWEEP hops on */
static void sentry_step(void) {
    uint32_t now = lv_tick_get();

    if (scr == SCR_WALKIE) return;      /* G shows: the dial is its channel's; A's scan, sweep and lock wait */
    if (trk.on) {
        track_step();
        return;
    }
    if (smode == MODE_MAN) return;
    if (scr == SCR_LOG && g_onx >= 0) return;    /* the log plays an X6200 one: the dial is its */
    if (g_armed) return;                          /* armed to transmit: the dial is the operator's */
    if (routine == ROUT_SCAN && scan_a_band >= 0 && (scan_a_next == 0 || now > scan_a_next)) {
        a_scan_hop(now);
        return;
    }
    if (routine == ROUT_SWEEP && (sweep_next == 0 || now > sweep_next)) {
        int i = sweep_next_band(sweep_i);

        sweep_next = now + scan_dwell_ms;   /* each band the scans' dwell (was SWEEP_MS, 7 s, apart from it) */
        if (i >= 0) {
            sweep_i = i;
            tune(bands[i].listen_hz, bands[i].mode);
        }
        return;
    }
    if (smode != MODE_AUTO || since_move < SETTLE_FRAMES || (int32_t)(now - tap_pause) < 0) return;
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

static void set_mode(sysmode_t m) {
    smode = m;
    if (m == MODE_AUTO) {
        anchor     = centre ? centre : dial_centre();
        sweep_next = 0;
    }
    show_screen();
}

/* SCAN CFG's SWEEP row: the X6200 hops between D's marked bands, the step time each (not along one band: A's F1, the
 * two easily taken for each other); the bands named as it starts */
static void a_sweep_toggle(void) {
    static char t[200];
    int         n = snprintf(t, sizeof(t), "THE X6200 HOPS BETWEEN D'S MARKED BANDS, THE STEP TIME EACH (A'S F1 "
                                           "SCANS ONE BAND END TO END INSTEAD):");

    set_routine(routine == ROUT_SWEEP ? ROUT_BAND : ROUT_SWEEP);
    for (int b = 0, any = 0; b < BANDS_N && n < (int)sizeof(t) - 16; b++)
        if (band_rt[b].sweep && cfg_is_valid_hw_freq(bands[b].listen_hz))
            n += snprintf(t + n, sizeof(t) - n, "%s %s", any++ ? "," : "", bands[b].name);
    if (routine == ROUT_SWEEP && sweep_next_band(-1) < 0)
        snprintf(t, sizeof(t), "NO BAND IS MARKED IN D BANDS (F3 THERE): IT STAYS WHERE IT IS.");
    osd(routine == ROUT_SWEEP ? "SWEEP ON" : "SWEEP OFF",
        routine == ROUT_SWEEP ? t : "THE X6200 STAYS ON THE 360 KHZ AT THE DIAL (A'S F1 SCANS ITS BAND).", -1);
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

static void state_keep(void);
static void state_restore(void);

static void blink_cb(lv_timer_t *t) {
    static int n;

    (void)t;
    if (hits_dirty && n % 120 == 0) hits_save();     /* G's log on DATA, a minute at most behind */
    state_keep();                                   /* for a restart of the GUI: gui_state.c */
    rtl_spectrum_rds_watch(rds_channel());          /* B's FM broadcast channel's RDS (C's stick entry's) */
    if (scr == SCR_SYSTEM && n % 2 == 0) sys_load_show();   /* its load each second */
    if (scr == SCR_RTL) watch_show();                       /* the watch channels, open or quiet */
    if (pp_on) pp_fill();                                   /* the pool's panel: who has what, as it changes */
    rds_show(false);
    g_rds_take();                                   /* C's entries named by it */
    sp_check();                                     /* B: which SDR, when more than one tunes its channel */
    g_scan_step();
    if (scr == SCR_LOG && g_clear_ms && lv_tick_get() - g_clear_ms > 3000) {
        g_clear_ms = 0;
        buttons_refresh(&btn[2]);
    }
    {
        bool here = adsb_rx_sdr_present();     /* an atomic, kept by adsb_rx.c's look at USB each second */

        if (here != sdr_here && scr == SCR_BANDS) {
            sdr_here = here;
            show_bands();
        }
        sdr_here = here;
        hear_follow();                  /* a stick plugged in: heard at once if LISTEN has it */
    }
    if (n++ % 4 == 0) {
        hear_vol_refresh();             /* the X6200's volume may have changed elsewhere (BOTH's shares) */
        bool ads  = adsb_rx_receiver_present() || adsb_rx_state(NULL, 0) == ADSB_RX_ON;
#ifdef BOARD_X6200
        bool wr   = param_i_get(cfg_wr12) && wr12_ready();      /* wr12.c: the X6200's only */
#else
        bool wr   = false;
#endif

        adsb_here = ads;
        wr12_here = wr;
    }
    blink = !blink;
    radio_moni_test_poll();             /* a test of the base's monitor, while /tmp/moni_test is there */
    if (vol_fn != VF_VOL && lv_tick_get() - vol_used_ms >= VOL_BACK_MS) {     /* left on SQL or RF GAIN: the volume */
        vol_fn = VF_VOL;
        show_legend();
    }
    show_head();
    if (key_l[0]) show_keys();          /* a key's word that follows the dial (A's F2 BAND), tuned from elsewhere:
                                         * TCI, the main screen (it said 15 M on 40 M, 2026-11-14) */
    if (scr == SCR_WALKIE && blink) show_walkie();      /* each second: the time, the power, a dial moved from outside */
    if (scr == SCR_TRACK) show_track();
    if (scr == SCR_BANDS && (lv_tick_get() / 500) % 20 == 0) show_bands();
}

/* ---- The keys ---- */

static const char *f_label(int k) {
    static char s[24];

    if (pp_on) return pp_label(k);
    switch (scr) {
        case SCR_SCAN: {        /* C's keys, for the X6200: SCAN its band, the BAND, VIEW, LISTEN, SCAN CFG */
            static const char *const views[] = {"RTL", "Split", "X6200"};
            int bi = routine == ROUT_SCAN ? scan_a_band : a_band_now(centre ? centre : dial_centre());

            if (k == 0) snprintf(s, sizeof(s), "Scan:\n%s", routine == ROUT_SCAN ? "On" : "Off");
            else if (k == 1 && bi >= 0) snprintf(s, sizeof(s), "Band:\n%s", bands[bi].name);
            else if (k == 1) {          /* the dial between bands (7.804 MHz): the nearest, as the BAND box says it,
                                         * not a "-" (the user: "the f2 band button has a - I've never seen that") */
                bool in;
                int  nb = band_near(centre ? centre : dial_centre(), &in);

                snprintf(s, sizeof(s), "Band:\n%s%s", nb >= 0 ? "near " : "", nb >= 0 ? bands[nb].name : "-");
            }
            else if (k == 2) snprintf(s, sizeof(s), "View:\n%s", spec_view() == VIEW_RTL ? rtl_spectrum_dev_tag()
                                                                                       : views[spec_view() % 3]);
            else if (k == 3) snprintf(s, sizeof(s), "Listen:\n%s", hear_name(param_i_get(cfg_hear_a)));
            else snprintf(s, sizeof(s), "Scan Cfg");
            return s;
        }
        case SCR_WALKIE:
            walkie_key_label(k, s, sizeof(s));
            return s;
        case SCR_TRACK: {       /* SCAN CFG: the style, HOLD or GO ON and SKIP for the radio it was opened from */
            bool b = cfg_from == SCR_RTL, held = b ? b_stop.on && b_stop.held : trk.on && trk.held;

            if (k == 0) snprintf(s, sizeof(s), "Style:\n%s", styles[scan_style()].word);
            else if (k == 1) snprintf(s, sizeof(s), "%s:\n%s", b ? "B" : "A", held ? "Go on" : "Hold");
            else if (k == 2) snprintf(s, sizeof(s), "%s:\nSkip", b ? "B" : "A");
            else if (k == 3) snprintf(s, sizeof(s), "%s", b ? "" : recorder_is_on() ? "Rec:\nOn" : "Rec");
            else snprintf(s, sizeof(s), "Back");
            return s;
        }
        case SCR_BANDS:
            if (k == 2) {
                snprintf(s, sizeof(s), "Sweep:\n%s", band_rt[band_sel].sweep ? "In" : "Out");
                return s;
            } else {
                const char *const l[] = {"Scan", "Listen", "", "", ""};    /* F4, F5: nothing here */
                return l[k];
            }
        case SCR_DECODE: {          /* F3 and F4 go up and down the TYPE list */
            const char *const l[] = {"", "Open", "Type:\nUp", "Type:\nDown", ""};
            return l[k];
        }
        case SCR_SYSTEM: {
            const char *const l[] = {"", "Open", "", "", ""};    /* the ring and the list reach the rest */
            return l[k];
        }
        case SCR_RTL: {
            int g = param_i_get(cfg_rtl_gain);

            static const char *const views[] = {"RTL", "Split", "X6200"};

            (void)g;
            if (k == 1) snprintf(s, sizeof(s), "Preset:\n%s", f_name());
            else if (k == 2) snprintf(s, sizeof(s), "View:\n%s", spec_view() == VIEW_RTL ? rtl_spectrum_dev_tag()
                                                                                       : views[spec_view() % 3]);
            else if (k == 3) snprintf(s, sizeof(s), "Listen:\n%s", hear_name(param_i_get(cfg_hear_f)));
            else if (k == 4) snprintf(s, sizeof(s), "Scan Cfg");
            else snprintf(s, sizeof(s), "Scan:\n%s", rtl_scan ? "On" : "Off");
            return s;
        }
        case SCR_LOG: {
            if (g_pick >= 0 && g_pick < nhits) {    /* which radios tune in */
                if (k == 0) snprintf(s, sizeof(s), "Hear on:\nAll");
                else if (k == 1) snprintf(s, sizeof(s), "Hear on:\n%s", g_x_name(g_pick));
                else if (k == 2) snprintf(s, sizeof(s), "Hear on:\n%s", g_r_name());
                else if (k == 4) snprintf(s, sizeof(s), "Back");
                else s[0] = 0;
                return s;
            }
            if (k == 3) snprintf(s, sizeof(s), "Scan:\n%s", g_scanning() ? "On" : "Off");
            else if (k == 4) snprintf(s, sizeof(s), "Dwell:\n%d s", scan_dwell_ms / 1000);
            else if (k == 2) snprintf(s, sizeof(s), g_clear_ms && lv_tick_get() - g_clear_ms < 3000 ? "Clear:\nSure?" : "Clear");
            else if (k == 1) snprintf(s, sizeof(s), "Remove");
            else snprintf(s, sizeof(s), "Listen:\n%s", g_playing(g_sel) ? "On" : "Off");
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
static int     open_mode, open_agc;

/* The X6200 set as D or E chose: the preamp, the attenuator and AGC (rx_setup), on the band just tuned (they are each
 * band's own); the SDR its gain */
static void rx_set(rxset_t st) {
    if (st.gain == -3) return;
    if (st.gain > -2) {
        param_i_set(cfg_rtl_gain, st.gain);
        rtl_spectrum_gain(st.gain);
        p_rtl.floor = -300;             /* the noise looked for again, as the gain key does */
        return;
    }
    cparam_i_set(cfg_cur_pre, st.pre);
    cparam_i_set(cfg_cur_att, st.att);
    if (st.agc >= 0) cparam_i_set(cfg_cur_agc, st.agc);
}

static void rx_setup_apply(int64_t hz, int agc) {
    rx_set(rx_setup(hz, agc));
}

/* CW on the main screen: its decoder hears nothing under the squelch either (screen_main.c's decoder apps open it):
 * held in the same place, so the main screen gives it back as the next app (the Sentry) opens */
static void sql_open_for_cw(void) {
    int32_t s = param_i_get(cfg_squelch);

    if (s <= 0 || param_i_get(cfg_sql_held) >= 0) return;
    param_i_set(cfg_sql_held, s);
    param_i_set(cfg_squelch, 0);
    msg_schedule_long_text_fmt("Squelch %d opened for CW: it gives the decoder silence. Back as the Sentry opens.",
                               (int)s);
}

static void open_tune_cb(lv_timer_t *t) {
    lv_timer_del(t);
    if (main_screen_freq_locked()) {
        msg_update_text_fmt("Choose %d.%03d MHz in the app", open_hz / 1000000, (open_hz / 1000) % 1000);
        return;
    }
    main_screen_set_freq(open_hz);
    if (open_mode >= 0 && !main_screen_mode_locked()) cparam_i_set(cfg_cur_mode, open_mode);
    rx_setup_apply(open_hz, open_agc);
    sql_open_for_cw();
}

static void open_async(void *arg) {
    const decoder_t *d = &decoders[(intptr_t)arg];

    if (d->rx == 'H' || d->rx == 'C') {     /* AIRSPACE: ADS-B, or its ACARS view on B's SDR */
        app_adsb_open_view(d->rx == 'C');
        app_sentry_open_at('H');
        return;
    }
    if (d->proto >= 0) param_i_set(cfg_ft8_protocol, d->proto);    /* the FT8 app: 0 FT4, 1 FT8 */
    if (!d->app) {                          /* CW: the main screen, there, till the Sentry opens again */
        main_screen_home_pause(true);
        dialog_destruct();
        lv_timer_create(open_tune_cb, 50, NULL);
        return;
    }
    /* A digital app opens where its memory says and then holds the frequency (the FT8 app on the preset nearest
     * it): the frequency chosen goes there first. Time Signal and Propagation have none: they start on the dial */
    if (tune(open_hz, open_mode)) {
        rx_setup_apply(open_hz, open_agc);
        if (d->mem[0]) app_mem_save(d->mem);
    }
    main_screen_start_app(d->app);          /* closes the Sentry first */
}

static void b_listen_at(int64_t hz, int band);

static void open_decoder(void) {
    const decoder_t *d = &decoders[dec_sel];

    if (dec_row >= d->n) return;
    if (d->rx == 'B') {                     /* the SDR's decoders: B on the channel, heard there, decoding by itself */
        if (rtl_spectrum_dev() == RTL_DEV_NONE) {
            osd("NO SDR", "THIS ONE IS DECODED BY AN SDR ON USB: AN RTL-SDR, A HACKRF OR A BLADERF.", -1);
            return;
        }
        b_listen_at(d->hz[dec_row], band_for(d->hz[dec_row]));
        rx_set(rx_setup_on(d->hz[dec_row], -1, true));
        osd(d->name, d->about, -1);
        return;
    }
    if (d->rx == 'C' && rtl_spectrum_dev() != RTL_DEV_NONE)  /* ACARS on B's SDR: the airband's gain */
        rx_set(rx_setup_on(d->hz[dec_row], -1, true));
    open_hz   = d->hz[dec_row] - d->audio;     /* the dial: the station that far up in the audio */
    open_mode = d->mode;
    open_agc  = d->agc;
    if (d->mode < 0) open_mode = open_hz < 10000000 ? radio_mode_lsb : radio_mode_usb;
    lv_async_call(open_async, (void *)(intptr_t)dec_sel);
}

static void sys_open_async(void *arg) {
    switch ((int)(intptr_t)arg) {
        case SYS_SETTINGS: main_screen_start_app(ACTION_APP_SETTINGS); break;
        case SYS_APPS: main_screen_open_apps(); break;
        case SYS_MODES: main_screen_start_app(ACTION_APP_MODES); break;
        case SYS_AIRSPACE: app_sentry_open_at('H'); break;
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

static void pp_open(void);

static void sys_open(int i) {
    if (i == SYS_VHF) {
        go(SCR_RTL);
        return;
    }
    if (i == SYS_POOL) {
        pp_open();
        return;
    }
    if (i == SYS_HOME) {
        param_i_set(cfg_sentry_home, !param_i_get(cfg_sentry_home));
        show_screen();
        return;
    }
    lv_async_call(sys_open_async, (void *)(intptr_t)i);
}

static bool listen_band_x6200(int i) {
    const band_t *b = &bands[i];
    bool          tuned;

    if (!cfg_is_valid_hw_freq(b->listen_hz)) {
        msg_update_text_fmt("The radio can't tune %s", b->name);
        return false;
    }
    /* the new band the place to come back to before a lock ends: track_end goes back there */
    routine = ROUT_BAND;
    anchor  = b->listen_hz;
    if (smode == MODE_MAN) smode = scan_stay == STAY_NONE ? MODE_SEMI : MODE_AUTO;   /* the Sentry's dial again, as
                                                 * SCAN CFG's style has it (it was set to AUTO here, on every BAND
                                                 * key, 2026-11-14) */
    if (trk.on) track_end();
    tuned = tune(b->listen_hz, b->mode);
    go(SCR_SCAN);
    return tuned;
}

/* D's LISTEN or SCAN on the X6200: the band set as rx_setup says and heard (A's LISTEN OFF, or on the SDR, played
 * nothing of it). A's BAND keys tune a band alone: its preamp and AGC as the user left them there */
static void d_listen_x6200(int i) {
    if (!listen_band_x6200(i)) return;
    rx_setup_apply(bands[i].listen_hz, radio_agc_auto);
    int h = param_i_get(cfg_hear_a);

    if (h == HEAR_OFF || h == HEAR_RTL) {
        param_i_set(cfg_hear_a, HEAR_X6200);
        if (scr == SCR_SCAN) hear_apply(HEAR_X6200);
    }
}

/* B on the SDR at hz, heard there, its scan off: D's LISTEN for a band the SDR takes (band: B's band from then on, its
 * F1 scans it; -1 B's presets), E's OPEN for the SDR's decoders */
static void b_listen_at(int64_t hz, int band) {
    f_band = band;
    rtl_scan_set(false);
    rtl_tune(hz);
    listen_hz = rtl_snap(hz);
    {
        int h = param_i_get(cfg_hear_f);

        if (h == HEAR_X6200 || h == HEAR_OFF) param_i_set(cfg_hear_f, HEAR_RTL);    /* LISTEN: to hear it */
    }
    if (scr == SCR_RTL) hear_apply(param_i_get(cfg_hear_f));
    go(SCR_RTL);
}

static void listen_band(void);

/* D's SCAN (F1): the band chosen set on its radio first, then scanned end to end there: the X6200's on A (its F1
 * scan, on this band), the stick's on B (its scan, from the band's low end, heard as LISTEN would have it) */
static void scan_band(void) {
    const band_t *b = &bands[band_sel];

    if (b->listen_hz == ADSB_HZ) {      /* ADS-B: AIRSPACE's, not a scan */
        listen_band();
        return;
    }
    if (band_uses_rtl(band_sel)) {
        b_listen_at(b->listen_hz, band_sel);
        rx_setup_apply(b->listen_hz, -1);
        rtl_scan_set(true);
        show_screen();
        return;
    }
    if (!cfg_is_valid_hw_freq(b->listen_hz)) {
        msg_update_text_fmt("The radio can't tune %s", b->name);
        return;
    }
    d_listen_x6200(band_sel);           /* tuned there, A shown */
    scan_a_band = band_sel;             /* this band, not the one the old picture still shows */
    if (smode == MODE_MAN) smode = scan_stay == STAY_NONE ? MODE_SEMI : MODE_AUTO;
    set_routine(ROUT_SCAN);
    {
        char t[24];

        snprintf(t, sizeof(t), "SCAN: %s", b->name);
        osd(t, smode == MODE_AUTO ? "THE BAND END TO END, 360 KHZ AT A TIME, STOPPING ON WHAT IT FINDS AS SCAN "
                                    "CFG'S STYLE SAYS."
                                  : "THE BAND END TO END, 360 KHZ AT A TIME, MARKING WHAT IT FINDS (BROWSE: SCAN CFG'S "
                                    "STYLE MAKES IT STOP).", -1);
    }
}

/* D's LISTEN (F2 or a press): B for a band the SDR takes, its listening frequency on the speaker, the scan off; A for
 * the X6200's; H AIRSPACE for ADS-B. Each set as rx_setup says (the preamp, AGC; the SDR's gain) */
static void listen_band(void) {
    const band_t *b = &bands[band_sel];

    if (b->listen_hz == ADSB_HZ) {      /* ADS-B: AIRSPACE draws it (a GNS 5890 stick or the SDR's dump1090) */
        app_adsb_open_view(false);
        lv_async_call(sys_open_async, (void *)(intptr_t)SYS_AIRSPACE);
        return;
    }
    if (!band_uses_rtl(band_sel)) {
        d_listen_x6200(band_sel);
        return;
    }
    b_listen_at(b->listen_hz, band_sel);
    rx_setup_apply(b->listen_hz, -1);
}

static void fkey(int k);

static void key_tap_cb(lv_event_t *e) {
    fkey((int)(intptr_t)lv_event_get_user_data(e));
}

static void fkey(int k) {
    if (k >= 0 && k < 5 && btn[k].disabled) return;     /* dimmed: a tap on its box does nothing either */
    if (pp_on) {                        /* the SDR pool's panel: its keys */
        pp_action(k);
        return;
    }
    if (sp_on) {                        /* B's SDR box: F4 (LISTEN) goes down it, the rest wait */
        if (k == 3) sp_step(1);
        return;
    }
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
        case SCR_WALKIE:                /* F1 and F2 as the arrows: CH -/+ on CB and the SDR's groups, a step elsewhere;
                                         * F3 the SDR's TX gain */
            if (k == 0) walkie_arrow(-1);
            if (k == 1) walkie_arrow(1);
            if (k == 2) w_sdr_gain_next();
            if (k == 3) ws_view_next();
            if (k == 4) walkie_group_mode();
            break;
        case SCR_TRACK:
            if (k == 4) {
                go(cfg_from);           /* BACK */
                break;
            }
            if (k == 0) cr_change(CR_STYLE);
            if (k == 1) cr_hold();
            if (k == 2) cr_skip();
            if (k == 3 && cfg_from == SCR_SCAN) {
                recorder_set_on(!recorder_is_on());
                rec_ours = recorder_is_on();
            }
            if (scr == SCR_TRACK) show_screen();
            break;
        case SCR_BANDS:
            if (k == 0) scan_band();
            if (k == 1) listen_band();
            if (k == 2) {
                if (!cfg_is_valid_hw_freq(bands[band_sel].listen_hz)) {
                    osd("THE SDR'S BAND", "THE X6200'S SWEEP CANNOT TUNE IT. F2 HERE OPENS IT ON B VHF/UHF, WHERE F1 "
                        "SCANS IT END TO END.", -1);
                    break;
                }
                band_rt[band_sel].sweep = !band_rt[band_sel].sweep;
                show_screen();
            }
            break;                      /* F4, F5: nothing */
        case SCR_DECODE:                /* F1, F5: nothing (no key only goes to another screen) */
            if (k == 1) open_decoder();
            if (k == 2 || k == 3) {
                dec_sel = (dec_sel + (k == 2 ? DECODERS_N - 1 : 1)) % DECODERS_N;
                dec_row = 0;
                show_screen();
            }
            break;
        case SCR_SYSTEM:                /* F2 opens the item; the others nothing (CLASSIC RADIO is on the list) */
            if (k == 1) sys_open(sys_sel);
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
            if (g_pick >= 0) {                  /* F1 ALL, F2 the X6200, F3 B's SDR, F5 BACK */
                int p  = g_pick;

                g_pick = -1;
                if (p < nhits && k <= 2) {
                    g_scan = false;
                    g_play_on(p, k != 2, k != 1);
                } else {
                    show_screen();
                }
                break;
            }
            if (k == 0) g_toggle(g_sel);
            if (k == 1) {
                g_remove(g_sel);
                show_screen();
            }
            if (k == 2) {
                if (g_clear_ms && lv_tick_get() - g_clear_ms < 3000) {
                    g_stop_all();
                    nhits = g_sel = 0;
                    hits_dirty = true;
                    g_clear_ms = 0;
                } else {
                    g_clear_ms = lv_tick_get();
                    osd("CLEAR THE LOG?", "F3 AGAIN WITHIN 3 SECONDS CLEARS ALL OF IT.", -1);
                }
                show_screen();
            }
            if (k == 3) g_scan_set(!g_scanning());
            if (k == 4) {                /* every scan's dwell: A's, B's and the log's */
                dwell_next();
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

/* The MFK's pan on A and B faster the faster it turns: a click within 100 ms of the last one the same way is 3 steps,
 * within 45 ms 8, so a spin crosses the picture and a slow turn still places it (the user: "when we use MFK can we do
 * better at scrolling the band spectrogram?": a step a click, A's 10 kHz of its 360, took 36 clicks across) */
static int pan_steps(int dir) {
    static uint32_t last;
    static int      last_dir;
    uint32_t        now = lv_tick_get(), dt = now - last;
    int             m   = dir != last_dir ? 1 : dt < 45 ? 8 : dt < 100 ? 3 : 1;

    last     = now;
    last_dir = dir;
    return m;
}

/* A's knobs tune the X6200, so A shows it and is heard on it. With VIEW on RTL (the SDR's picture alone, one F3 from
 * SPLIT) nothing on A moved as they did: the user, 2026-11-16, "on A when I twist the large dial it doesn't seem to
 * tune the tuner ... it's like MFK and dial tuning on A got broken" (both tuned; only the TUNED box said so). Turned
 * there, or a band chosen, A goes to SPLIT: the X6200's half again, the SDR's under it. And LISTEN on an SDR (alone,
 * BOTH or ALL) goes to the X6200, said so (the user, 2026-11-16: "IF we try to tune on A while listening to any other
 * device, it should auto switch back to the internal radio and tell the user that is what it did, because A's screen
 * is intended primarily for the internal SDR"). OFF stays off. Said last, over the tuning's own words */
static void a_tuned(void) {
    int  h    = param_i_get(cfg_hear_a);
    bool view = param_i_get(cfg_a_view) == VIEW_RTL, hear = h == HEAR_RTL || h == HEAR_BOTH;
    char title[32], t[200];

    if (scr != SCR_SCAN || (!view && !hear)) return;
    if (view) {
        param_i_set(cfg_a_view, VIEW_SPLIT);
        rtl_layout();
        rtl_labels_place();
    }
    if (hear) {
        snprintf(title, sizeof(title), "%s", hear_name(h));    /* what it was, before it changes */
        snprintf(t, sizeof(t), "YOU TUNED A, THE X6200'S SCREEN: ITS RECEIVER IS ON THE SPEAKER AGAIN (LISTEN WAS %s)%s. "
                 "F4 CHANGES IT.", title, view ? ", AND ITS PICTURE BACK (VIEW SPLIT)" : "");
        param_i_set(cfg_hear_a, HEAR_X6200);
        hear_apply(HEAR_X6200);
    }
    show_screen();
    if (hear)
        osd("A LISTEN: X6200", t, -1);
    else
        osd("VIEW: SPLIT", "THE X6200'S PICTURE IS BACK AS ITS DIAL MOVES (VIEW RTL SHOWED THE SDR'S ALONE). F3 CHANGES "
                           "THE VIEW.", -1);
}

/* B is its SDR's screen: tuned there by hand (the VFO, the MFK turned or pushed, a preset) while LISTEN is the
 * X6200's, the SDR goes on the speaker at B's channel, said so; VIEW on the X6200 alone goes to the SDR's picture (the
 * user, 2026-11-16: "on B if for some reason you happen to be listening to the X6200 internal radio, and you tune
 * manually or use MFK to jump around, it should instantly switch the the selected radio view so there is no
 * confusion"). BOTH, ALL and OFF stay; no SDR on USB, nothing changes */
static void b_tuned(void) {
    bool sdr  = sdr_here || rtl_spectrum_state() == RTL_SPEC_ON;
    bool view = param_i_get(cfg_rtl_view) == VIEW_X6200, hear = param_i_get(cfg_hear_f) == HEAR_X6200;
    char title[32], t[200], f[16];

    if (scr != SCR_RTL || !sdr || (!view && !hear)) return;
    if (view) {
        param_i_set(cfg_rtl_view, VIEW_RTL);
        rtl_layout();
        rtl_labels_place();
    }
    if (hear) {
        param_i_set(cfg_hear_f, HEAR_RTL);
        hear_apply(HEAR_RTL);
    }
    show_screen();
    if (hear) {
        fmt_mhz(f, sizeof(f), listen_hz ? listen_hz : rtl_hz, 3);
        snprintf(title, sizeof(title), "B LISTEN: %s", hear_name(HEAR_RTL));
        snprintf(t, sizeof(t), "YOU TUNED B, THE SDR'S SCREEN: THE %s IS ON THE SPEAKER AT B'S CHANNEL (%s %s), NOT THE "
                 "X6200%s. F4 CHANGES IT.", rtl_spectrum_dev_name(), f,
                 rtl_mode_names[rtl_mode_of(listen_hz ? listen_hz : rtl_hz)],
                 view ? ", AND ITS PICTURE SHOWN (VIEW X6200 SHOWED THE X6200'S ALONE)" : "");
        osd(title, t, -1);
    } else {
        snprintf(title, sizeof(title), "VIEW: %s", rtl_spectrum_dev_tag());
        osd(title, "THE SDR'S PICTURE IS BACK AS ITS CHANNEL MOVES (VIEW X6200 SHOWED THE X6200'S ALONE). F3 CHANGES THE "
                   "VIEW.", -1);
    }
}

static void step(int dir) {
    if ((scr == SCR_SCAN || scr == SCR_WALKIE) && dial_held()) return;     /* the MFK tunes the X6200 here */
    switch (scr) {
        case SCR_SCAN: {                /* as on C: it pans, by SCAN CFG's step (more when spun); a scan stops there */
            int32_t st = a_steps[a_step], fq = cparam_i_get(cfg_fg_freq);

            if (routine == ROUT_SCAN) set_routine(ROUT_BAND);
            tune(fq / st * st + dir * pan_steps(dir) * st, -1);
            show_screen();
            a_tuned();
            break;
        }
        case SCR_TRACK:                 /* SCAN CFG: the next row, or the one before */
            cr_sel = (cr_sel + dir + CR_N) % CR_N;
            show_screen();
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
        case SCR_WALKIE:                /* the group (each band one, CB, the SDR's): the preset bands, as B's F2; the
                                         * arrows step (the user, 2026-11-16: "MFK changes preset band, and dial for
                                         * precise, and the left right channel arrows should do the step advancement") */
            walkie_group(dir);
            break;
        case SCR_LOG:                   /* the next one; while one plays, it plays the next one; a scan stops */
            if (!nhits) break;
            g_scan = false;
            g_pick = -1;
            g_sel = (g_sel + dir + nhits) % nhits;
            if (g_onx >= 0 || g_onr >= 0) g_follow(g_sel);
            else show_screen();
            break;
        case SCR_RTL:
            rtl_scan_set(false);        /* panned by hand: the scan stops where it was */
            rtl_tune(rtl_hz / rtl_steps[rtl_step] * rtl_steps[rtl_step] + (int64_t)dir * pan_steps(dir) * rtl_steps[rtl_step]);
            b_tuned();
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
                a_tuned();
            } else {
                osd("ONLY NOISE", "NO SIGNAL IN THE PICTURE TO LOCK ON.", -1);
            }
            break;
        }
        case SCR_TRACK:                 /* SCAN CFG: the chosen row to its next value */
            cr_change(cr_sel);
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
                b_tuned();
            } else {
                osd("ONLY NOISE", "NO SIGNAL IN THE PICTURE TO GO TO.", -1);
            }
            break;
        case SCR_LOG:
            g_toggle(g_sel);
            break;
        case SCR_WALKIE: walkie_ch(0, 19); break;
        default: break;
    }
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));

    if (pp_on) {                        /* the SDR pool's panel: the MFK goes through it, its push uses one on B */
        switch (key) {
            case LV_KEY_LEFT:
            case LV_KEY_UP: if (pp_n) pp_sel = (pp_sel + pp_n - 1) % pp_n; pp_fill(); pp_keys(); return;
            case LV_KEY_RIGHT:
            case LV_KEY_DOWN: if (pp_n) pp_sel = (pp_sel + 1) % pp_n; pp_fill(); pp_keys(); return;
            case LV_KEY_ENTER: pp_action(0); return;
            case LV_KEY_ESC: pp_close(); return;
            default: break;
        }
    }
    if (sp_on) {                        /* B's SDR box: the MFK goes through it, its push (click_cb) chooses */
        switch (key) {
            case LV_KEY_LEFT:
            case LV_KEY_UP: sp_step(-1); return;
            case LV_KEY_RIGHT:
            case LV_KEY_DOWN: sp_step(1); return;
            case LV_KEY_ENTER: sp_close(sp_units[sp_sel]); return;
            case LV_KEY_ESC: sp_close(-1); return;     /* the one open kept */
            default: break;
        }
    }

    if (help_on && key != KEY_VOL_LEFT_EDIT && key != KEY_VOL_RIGHT_EDIT && key != KEY_VOL_LEFT_SELECT &&
        key != KEY_VOL_RIGHT_SELECT) {
        help_show(false);
        return;
    }
    switch (key) {
        case KEY_VOL_PRESS:
            vol_fn      = (volfn_t)((vol_fn + 1) % 3);
            vol_used_ms = lv_tick_get();
            vol_say();
            return;
        case KEY_MFK_PREV:
        case KEY_MFK_NEXT: {
            scr_t at = scr == SCR_TRACK ? cfg_from : scr;
            int   i  = 0;

            while (i < RING_N && ring[i] != at) i++;
            i = (i % RING_N + (key == KEY_MFK_NEXT ? 1 : RING_N)) % (RING_N + 1);
            /* H: AIRSPACE, in the Sentry's place; before A, I (the direction finder) while a Kraken is there */
            if (i == RING_N) app_sentry_open_at(key == KEY_MFK_PREV && df_rx_present() ? 'I' : 'H');
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
        else if (pp_on)
            pp_action(0);
        else if (sp_on)
            sp_close(sp_units[sp_sel]);
        else
            press();
    }
}

/* The main knob: the radio is the operator's again */
static void rotary_cb(int32_t diff) {
    if (dial_held()) return;            /* TX unlocked: the dial is not moved by a bump */
    if (f4_roll && scr == SCR_RTL) {    /* B's F4 held: its SDRs, one a click, used when F4 is let go */
        f4_rolled = true;
        b_roll(diff);
        return;
    }
    if (f2_roll && scr == SCR_RTL) {    /* B's F2 held: a preset a click, back when turned back */
        f2_rolled = true;
        rtl_preset_go(diff);
        return;
    }
    if (scr == SCR_WALKIE && w_sdr_hz) {    /* on the SDR: its channel, 5 kHz a click */
        w_sdr_turn(diff);
        return;
    }
    if (scr == SCR_WALKIE) {            /* the dial tunes, fine (the main screen's step), by hand */
        x62_by_hand();
        return;
    }
    if (knob_taken()) {                 /* the stick is heard: the knob is its */
        if (scr == SCR_LOG) g_turn(diff);
        else rtl_listen_turn(diff);
        return;
    }
    if (smode != MODE_MAN) {
        smode = MODE_MAN;
        if (routine == ROUT_SCAN) routine = ROUT_BAND;      /* tuned by hand: the scan stops there, as F's */
        if (trk.on) trk.held = true;
        show_screen();
        osd("MANUAL", "YOU TURNED THE VFO: THE SENTRY STOPS MOVING THE X6200'S DIAL. A'S F1 (SCAN) OR A BAND KEY "
                      "HANDS IT BACK, IN SCAN CFG'S STYLE.", -1);
    }
    a_tuned();                          /* on A: the X6200's picture and LISTEN, if they were the SDR's */
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
    if (scr == SCR_WALKIE) {            /* G: the arrows step: CB's and the SDR's channels, elsewhere the dial by A's
                                         * step (the MFK goes from group to group) */
        walkie_arrow(up ? 1 : -1);
        return;
    }
    if (scr == SCR_RTL) {               /* F: its presets, not the X6200's bands (and never A) */
        rtl_preset_go(up ? 1 : -1);
        return;
    }
    int32_t khz = cparam_i_get(cfg_fg_freq) / 1000;
    int     i   = a_band_now((int64_t)cparam_i_get(cfg_fg_freq)), n = 0;

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
    a_tuned();
}

static bool front_key(int key, int state) {
    bool tap = state == KEYPAD_RELEASE, hold = state == KEYPAD_LONG;
    char t[48];

    if (help_on) {
        if (tap || hold) help_show(false);
        return key != KEYPAD_POWER;     /* POWER held switches off from the help too, as from every screen */
    }
    switch (key) {
        case KEYPAD_GEN:
            if (tap && !g_portrait) help_show(true);    /* drawn on the landscape screen: none held up */
            return true;
        case KEYPAD_APP:                /* held: the SDR pool's panel, from any screen */
            if (tap && pp_on) pp_close();
            else if (tap) go(SCR_SYSTEM);
            else if (hold && pp_on) pp_close();
            else if (hold) pp_open();
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
            if (scr == SCR_WALKIE && w_sdr_hz) {            /* G on the SDR's channels: its transmitter */
                if (tap && !g_sdr_armed && !rtl_spectrum_can_tx()) {    /* an RTL-SDR: heard, never sent */
                    snprintf(t, sizeof(t), "THE %s ONLY RECEIVES", rtl_spectrum_dev_name());
                    w_say(t, "RECEIVE ONLY", "G HEARS ITS CHANNELS THROUGH IT. TO TRANSMIT ON THEM: A HACKRF OR A "
                                             "BLADERF ON THE HOST PORT.");
                } else if (tap) {
                    w_sdr_arm(!g_sdr_armed);
                }
                return true;
            }
            if (scr == SCR_WALKIE || scr == SCR_SCAN) {     /* A and G, the X6200's: the transmitter armed or locked */
                if (tap) walkie_arm(!g_armed);
                return true;
            }
            if (tap || hold)
                osd("A/B", scr == SCR_RTL ? "B ONLY RECEIVES: A/B ARMS THE TRANSMITTER ON A AND G (G: A HACKRF'S OR "
                                            "BLADERF'S ON ITS VHF/UHF CHANNELS)."
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
        case KEYPAD_PTT:            /* refused (radio_rx_only) unless armed: said what to press, on every screen */
            if (scr == SCR_WALKIE && w_sdr_hz) {    /* G on the SDR: its transmitter, the X6200 left held */
                if (state == KEYPAD_PRESS) w_sdr_key(true, 1);
                else if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) w_sdr_key(false, 1);
                return true;
            }
            if (rx_held && !g_armed && state != KEYPAD_RELEASE && state != KEYPAD_LONG_RELEASE) walkie_ptt();
            return false;
        case KEYPAD_F2:                 /* B: held, the VFO knob rolls the presets (rotary_cb), and its release
                                         * then steps none; let go unturned, the next preset as ever */
            if (state == KEYPAD_PRESS) {
                f2_roll   = scr == SCR_RTL && !sp_on;
                f2_rolled = false;
                return false;
            }
            if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) {
                bool rolled = f2_roll && f2_rolled;

                f2_roll = f2_rolled = false;
                return rolled;
            }
            return f2_roll && f2_rolled;    /* a hold that rolled: nothing more of it */
        case KEYPAD_F3:                 /* B with a Meteor picture: held, its next view (colour, each channel) */
            if (scr != SCR_RTL || !met_on || sp_on || pp_on) return false;
            if (state == KEYPAD_LONG) {
                met_next_view();
                return true;
            }
            return state == KEYPAD_LONG_RELEASE;    /* a hold: nothing more of it (a tap is VIEW as ever) */
        case KEYPAD_F4: {               /* B: a tap LISTEN as ever; held and the VFO turned, its SDRs (b_roll), the one
                                         * chosen used on letting go; held and let go unturned, its channel watched
                                         * on a spare RTL, or no longer */
            if (scr != SCR_RTL || sp_on || pp_on) return false;
            if (state == KEYPAD_PRESS) {
                f4_roll   = true;
                f4_rolled = false;
                f4_pick   = -1;
                return false;
            }
            if (state == KEYPAD_LONG) return true;      /* nothing yet: a turn rolls, letting go watches */
            if (state == KEYPAD_RELEASE || state == KEYPAD_LONG_RELEASE) {
                bool rolled = f4_roll && f4_rolled;

                f4_roll = f4_rolled = false;
                if (rolled) {
                    if (f4_pick >= 0 && f4_pick != b_dev()) b_use(f4_pick, b_sdrs());
                    f4_pick = -1;
                    return true;
                }
                if (state == KEYPAD_LONG_RELEASE) {
                    b_watch_toggle();
                    return true;
                }
                return false;           /* a tap: LISTEN */
            }
            return f4_roll && f4_rolled;
        }
        default:
            return false;       /* POWER, F1, F3, F5: as ever */
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
            trk.held = true;            /* chosen by hand: there till SCAN CFG's F2 or F3, or another choice */
            return;
        }
        last_tap_hz = hz;
        last_tap_ms = lv_tick_get();
    }
    tap_pause = lv_tick_get() + 10000;  /* AUTO locks on nothing else for 10 s: a second tap locks on this one (it
                                         * turned SEMI before, and the style with it) */
    cursor_hz = hz;
}

static void tap_cb(lv_event_t *e) {
    int tag = (int)(intptr_t)lv_event_get_user_data(e), box = tag / 16, i = tag % 16;

    switch (box) {
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
        case 3:                                                    /* a decoder (the list scrolls) */
            if (dec_top() + i < DECODERS_N) {
                dec_sel = dec_top() + i;
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
            g_toggle(i);
        else {
            g_sel  = i;
            g_pick = -1;
            if (g_onx >= 0 || g_onr >= 0) {
                g_scan = false;
                g_follow(i);
            } else {
                show_screen();
            }
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
/* SCAN CFG's NOW: what A's X6200 and B's SDR scans do, and the latest the scans locked on (C LOG has them all) */
static void cr_now(char *s, size_t n) {
    char   f[16], g[16];
    size_t at;

    if (!wide_ok)
        at = (size_t)snprintf(s, n, "A  X6200  THIS BASE SENDS NO SPECTRUM: THE SENTRY NEEDS AN X6200'S");
    else if (smode == MODE_MAN)
        at = (size_t)snprintf(s, n, "A  X6200  TUNED BY HAND: IT STAYS THERE. A'S F1 OR A BAND KEY SCANS AGAIN");
    else if (trk.on) {
        bool lost = lv_tick_get() - trk.last_seen > 1500;

        const target_t *tg_ = target_near(trk.hz, 3000);

        fmt_freq(f, sizeof(f), trk.id.tune_hz);
        at = (size_t)snprintf(s, n, "A  X6200  STOPPED ON %s %s%s%s%s, %.0f S HEARD%s%s", trk.id.label, f,
                              tg_ ? " (" : "", tg_ ? profile(tg_) : "", tg_ ? ")" : "", trk.heard_s,
                              lost ? ", QUIET NOW" : "", trk.held ? ", HELD" : "");
    } else if (routine == ROUT_SCAN && scan_a_band >= 0) {
        fmt_freq(f, sizeof(f), centre);
        at = (size_t)snprintf(s, n, "A  X6200  SCANNING %s, NOW AT %s%s", bands[scan_a_band].name, f,
                              smode == MODE_AUTO ? "" : " (MARKS ONLY)");
    } else if (routine == ROUT_SWEEP) {
        int k = 0;
        for (int i = 0; i < BANDS_N; i++) k += band_rt[i].sweep;
        fmt_freq(f, sizeof(f), centre);
        at = (size_t)snprintf(s, n, "A  X6200  SWEEPING D'S %d BANDS, NOW AT %s", k, f);
    } else {
        fmt_freq(f, sizeof(f), centre);
        at = (size_t)snprintf(s, n, "A  X6200  WATCHING THE 360 KHZ AT %s%s", f,
                              smode == MODE_AUTO ? ", STOPS ON WHAT IT FINDS" : " (A'S F1 SCANS THE BAND)");
    }
    if (at >= n) return;
    char dev[16];
    snprintf(dev, sizeof(dev), "%-5s", rtl_spectrum_dev_tag());
    fmt_mhz(f, sizeof(f), listen_hz, 3);
    if (rtl_spectrum_state() != RTL_SPEC_ON)
        at += (size_t)snprintf(s + at, n - at, "\nB  %s  NOT RECEIVING: %s", dev, rtl_spectrum_dev_name());
    else if (b_stop.on)
        at += (size_t)snprintf(s + at, n - at, "\nB  %s  STOPPED ON %s %s, %u S%s", dev, rtl_name(b_stop.hz), f,
                               (unsigned)((lv_tick_get() - b_stop.start) / 1000), b_stop.held ? ", HELD" : "");
    else if (rtl_scan)
        at += (size_t)snprintf(s + at, n - at, "\nB  %s  SCANNING %s, NOW AT %s", dev, f_name(), f);
    else
        at += (size_t)snprintf(s + at, n - at, "\nB  %s  ON %s %s (B'S F1 SCANS %s)", dev, f, rtl_name(listen_hz), f_name());
    if (at >= n) return;
    at += (size_t)snprintf(s + at, n - at, "\nLAST:");
    for (int i = 0; i < nhits && i < 3 && at < n; i++) {
        struct tm tm;
        char      t[8];

        localtime_r(&hits[i].last, &tm);
        strftime(t, sizeof(t), "%H:%M", &tm);
        fmt_mhz(g, sizeof(g), hits[i].hz, 3);
        at += (size_t)snprintf(s + at, n - at, "%s %s %s %s %.12s", i ? "  " : " ", t,
                               hits[i].rtl ? sdr_code_tag(hits[i].sdr) : "X6200", g, hits[i].what);
    }
    if (!nhits) snprintf(s + at, n - at, " NOTHING YET");
}

/* A row's value, as SCAN CFG shows it */
static void cr_value(int r, char *v, size_t n) {
    static const char *const bsteps[] = {"12.5 KHZ", "25 KHZ", "100 KHZ", "500 KHZ", "1 MHZ"};
    int                      g = param_i_get(cfg_rtl_gain), q = param_i_get(cfg_rtl_sql), k = 0;

    switch (r) {
        case CR_STYLE: snprintf(v, n, "%s", styles[scan_style()].name); break;
        case CR_STEP: snprintf(v, n, "%d S A STEP", scan_dwell_ms / 1000); break;
        case CR_STAY: snprintf(v, n, "%s", stay_word[scan_stay]); break;
        case CR_QUIET:
            if (scan_stay == STAY_NONE || scan_stay == STAY_HOLD) snprintf(v, n, "(NOT WITH THIS STAY)");
            else snprintf(v, n, "%u S OF QUIET", (unsigned)(quiet_ms[scan_quiet] / 1000));
            break;
        case CR_SWEEP:
            for (int i = 0; i < BANDS_N; i++) k += band_rt[i].sweep && cfg_is_valid_hw_freq(bands[i].listen_hz);
            if (routine == ROUT_SWEEP) snprintf(v, n, "ON, %d BANDS", k);
            else snprintf(v, n, k ? "OFF (%d MARKED IN D)" : "OFF (NONE MARKED IN D)", k);
            break;
        case CR_ASTEP: snprintf(v, n, "%d KHZ A CLICK", (int)(a_steps[a_step] / 1000)); break;
        case CR_BSTEP: snprintf(v, n, "%s A CLICK", bsteps[rtl_step]); break;
        case CR_BGAIN:
            if (g < 0) snprintf(v, n, "AUTO (ITS AGC)");
            else snprintf(v, n, "%d.%d DB", g / 10, g % 10);
            break;
        case CR_BSQL:
            if (q) snprintf(v, n, "%d DB OVER THE NOISE", q);
            else snprintf(v, n, "OFF: ALWAYS HEARD");
            break;
        default: v[0] = 0;
    }
}

/* What the chosen row does, in words */
static const char *cr_about(int r) {
    static char s[200];

    switch (r) {
        case CR_STYLE: return styles[scan_style()].about;
        case CR_STEP:
            return "HOW LONG EACH STEP OF A BAND IS WATCHED WHEN NOTHING STOPS THE SCAN (A'S 360 KHZ, B'S PICTURE). "
                   "SHORTER IS FASTER, LONGER CATCHES MORE.";
        case CR_STAY:
            switch (scan_stay) {
                case STAY_NONE:
                    return "IT NEVER STOPS: A MARKS WHAT IT FINDS, B'S CHANNEL GOES TO EACH PICTURE'S STRONGEST, "
                           "AND BOTH GO ON.";
                case STAY_TALKS:
                    return "IT STOPS ON A SIGNAL AS LONG AS IT TALKS (5 MINUTES AT MOST, FOR A CARRIER THAT NEVER "
                           "ENDS), THEN GOES ON.";
                case STAY_HOLD:
                    return "IT STOPS ON THE FIRST SIGNAL AND STAYS. F2 GO ON LETS IT GO, F3 SKIP PASSES IT OVER TILL "
                           "THE SENTRY CLOSES.";
                default:
                    snprintf(s, sizeof(s), "IT STOPS ON A SIGNAL FOR %s AT MOST, LESS IF THE SIGNAL GOES QUIET FIRST.",
                             stay_word[scan_stay]);
                    return s;
            }
        case CR_QUIET:
            return "A SIGNAL QUIET THIS LONG IS LEFT FOR THE NEXT ONE. SHORT FOR BUSY CHANNELS, LONGER TO CATCH THE "
                   "REPLY.";
        case CR_SWEEP:
            return "ON: THE X6200 HOPS BETWEEN THE BANDS MARKED * IN D BANDS (F3 THERE). OFF: IT STAYS ON ITS BAND, "
                   "AND A'S F1 SCANS THAT ONE END TO END.";
        case CR_ASTEP: return "HOW FAR ONE CLICK OF A'S MFK MOVES THE X6200'S PICTURE.";
        case CR_BSTEP: return "HOW FAR ONE CLICK OF B'S MFK MOVES THE SDR'S PICTURE.";
        case CR_BGAIN:
            return "THE SDR'S GAIN: UP FOR WEAK SIGNALS, DOWN WHEN STRONG ONES SWAMP IT. AUTO: ITS OWN AGC.";
        case CR_BSQL:
            return "B STAYS SILENT TILL ITS CHANNEL IS THIS FAR OVER THE NOISE. OFF: ALWAYS HEARD (B'S SCAN THEN "
                   "TAKES 10 DB AS TALKING).";
        default: return "";
    }
}

static void show_track(void) {
    static const char *const names[CR_N] = {"SCAN STYLE", "STEP TIME", "STAY ON A SIGNAL", "GO ON AFTER",
                                            "A: SWEEP D'S BANDS", "A: KNOB STEP", "B: KNOB STEP", "B: GAIN",
                                            "B: SQUELCH"};
    char v[48], now[400];

    for (int r = 0; r < CR_N; r++) {
        bool on   = r == cr_sel;
        bool idle = r == CR_QUIET && (scan_stay == STAY_NONE || scan_stay == STAY_HOLD);

        label_set_text(cr_row_l[r], names[r]);
        cr_value(r, v, sizeof(v));
        label_set_text(cr_val_l[r], v);
        lv_obj_set_style_bg_opa(cr_row_l[r], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(cr_row_l[r], on ? ink : amb, 0);
        lv_obj_set_style_text_color(cr_val_l[r], on ? ink : idle ? dim : amb, 0);
    }
    label_set_text(cr_about_l, cr_about(cr_sel));
    cr_now(now, sizeof(now));
    label_set_text(cr_now_l, now);
}

/* The chosen row changed to its next value, and said in words */
static void cr_change(int r) {
    static const int sqls[] = {0, 6, 10, 15, 20};
    char             v[48], t[48];

    switch (r) {
        case CR_STYLE: {
            int k = scan_style();
            style_apply(k >= STY_SURVEY ? STY_BROWSE : k + 1);
            break;
        }
        case CR_STEP: dwell_next(); break;
        case CR_STAY:
            scan_stay = (scan_stay + 1) % STAY_N;
            scan_mode_sync();
            break;
        case CR_QUIET:
            if (scan_stay == STAY_NONE || scan_stay == STAY_HOLD) {
                osd("GO ON AFTER", "NOT USED WITH THIS STAY: CHOOSE A STAY THAT ENDS (2 S TO WHILE IT TALKS) FIRST.",
                    -1);
                return;
            }
            scan_quiet = (scan_quiet + 1) % (int)N_OF(quiet_ms);
            break;
        case CR_SWEEP: a_sweep_toggle(); return;           /* says it itself */
        case CR_ASTEP: a_step = (a_step + 1) % (int)N_OF(a_steps); break;
        case CR_BSTEP: rtl_fkey(2); break;
        case CR_BGAIN: rtl_fkey(5); break;
        case CR_BSQL: {
            int q = param_i_get(cfg_rtl_sql), i = 0;

            while (i < (int)N_OF(sqls) && sqls[i] <= q) i++;
            q = sqls[i % (int)N_OF(sqls)];
            param_i_set(cfg_rtl_sql, q);
            rtl_spectrum_squelch(q);
            break;
        }
        default: return;
    }
    static const char *const names[CR_N] = {"STYLE", "STEP TIME", "STAY", "GO ON AFTER", "SWEEP", "A'S STEP",
                                            "B'S STEP", "B'S GAIN", "B'S SQUELCH"};
    cr_value(r, v, sizeof(v));
    snprintf(t, sizeof(t), "%s: %s", names[r], v);
    osd(t, cr_about(r), -1);
}

/* SCAN CFG's F2 and F3 on the radio it was opened from: HOLD the signal it stopped on, or GO ON from it; SKIP it (A
 * locks on it no more, B's scan passes it over, till the Sentry closes) */
static void cr_hold(void) {
    if (cfg_from == SCR_RTL) {
        if (!b_stop.on) {
            osd("NOTHING TO HOLD", rtl_scan ? "B'S SCAN IS STILL LOOKING: IT STOPS ON THE NEXT SIGNAL (SCAN STYLE)."
                                            : "B IS NOT SCANNING: F1 ON B STARTS IT.", -1);
            return;
        }
        if (b_stop.held) {
            b_stop.on = b_stop.held = false;
            scan_next = lv_tick_get();
            osd("GO ON", "B'S SCAN GOES ON TO THE NEXT SIGNAL.", -1);
        } else {
            b_stop.held = true;
            osd("HELD", "B STAYS ON THIS CHANNEL TILL F2 AGAIN (GO ON) OR F3 (SKIP).", -1);
        }
        return;
    }
    if (!trk.on) {
        osd("NOTHING TO HOLD", smode == MODE_AUTO ? "A IS STILL LOOKING: IT STOPS ON THE NEXT SIGNAL IT FINDS."
                                                  : "A ONLY MARKS SIGNALS IN THIS STYLE (BROWSE): A'S MFK PUSH STOPS "
                                                    "ON THE STRONGEST.", -1);
        return;
    }
    if (trk.held) {
        trk.held = false;
        track_end();
        osd("GO ON", "A GOES ON TO THE NEXT SIGNAL.", -1);
    } else {
        trk.held = true;
        osd("HELD", "A STAYS ON THIS SIGNAL TILL F2 AGAIN (GO ON) OR F3 (SKIP).", -1);
    }
}

static void cr_skip(void) {
    if (cfg_from == SCR_RTL) {
        if (!b_stop.on) {
            osd("NOTHING TO SKIP", "B IS NOT STOPPED ON A SIGNAL.", -1);
            return;
        }
        if (b_nskip < (int)N_OF(b_skip)) b_skip[b_nskip++] = b_stop.hz;
        b_stop.on = b_stop.held = false;
        scan_next = lv_tick_get();
        osd("SKIPPED", "B'S SCAN PASSES THIS CHANNEL OVER TILL THE SENTRY CLOSES.", -1);
        return;
    }
    if (!trk.on) {
        osd("NOTHING TO SKIP", "A IS NOT STOPPED ON A SIGNAL.", -1);
        return;
    }
    if (n_ignored < IGNORED) ignored[n_ignored++] = trk.hz;
    trk.held = false;
    track_end();
    osd("SKIPPED", "A LOCKS ON IT NO MORE TILL THE SENTRY CLOSES.", -1);
}

static void cr_tap_cb(lv_event_t *e) {
    int r = (int)(intptr_t)lv_event_get_user_data(e);

    if (scr != SCR_TRACK || r < 0 || r >= CR_N) return;
    if (r == cr_sel) cr_change(r);      /* the chosen one tapped again: changed, as the MFK's push */
    cr_sel = r;
    show_screen();
}

/* SCAN CFG: how the scans behave, a row each setting, the chosen one explained under them, the scans' NOW at the
 * bottom (the user, 2026-11-17: the old boxes were "super robust, but makes zero sense to me usability wise") */
static void build_track(lv_obj_t *p) {
    int y = BODY_Y + 2, rh = 22, h = 24 + CR_N * rh + 6;

    rect(p, 6, y - 2, SW - 12, h, 3, false);
    label(p, &pixel_16, amb, 14, y + 2, SW - 28, LV_TEXT_ALIGN_LEFT,
          "HOW THE SCANS BEHAVE, A'S X6200 AND B'S SDR ALIKE.   MFK: CHOOSE A ROW   PUSH OR TAP: CHANGE IT");
    for (int r = 0; r < CR_N; r++) {
        int ry = y + 24 + r * rh;

        cr_row_l[r] = label(p, &pixel_20, amb, 10, ry, SW - 20, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_set_height(cr_row_l[r], rh);
        lv_obj_set_style_pad_left(cr_row_l[r], 6, 0);
        lv_obj_set_style_bg_color(cr_row_l[r], amb, 0);
        lv_obj_add_flag(cr_row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cr_row_l[r], cr_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        cr_val_l[r] = label(p, &pixel_20, amb, 330, ry, SW - 340, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_add_flag(cr_val_l[r], LV_OBJ_FLAG_EVENT_BUBBLE);     /* a tap on the value is the row's */
        lv_obj_clear_flag(cr_val_l[r], LV_OBJ_FLAG_CLICKABLE);
    }
    int ay = y - 2 + h + 6, ah = 46;
    rect(p, 6, ay, SW - 12, ah, 3, false);
    cr_about_l = label(p, &pixel_16, amb, 14, ay + 5, SW - 28, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(cr_about_l, LV_LABEL_LONG_WRAP);
    cr_now_l = label(p, &pixel_16, amb, 14, ay + ah + 6, SW - 28, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_long_mode(cr_now_l, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(cr_now_l, scr_h - (ay + ah + 6) - 2);
}

static void build_bands(lv_obj_t *p) {
    int y = BODY_Y + 2, rh = 18;

    rect(p, 6, y - 2, SW - 12, scr_h - y - 2, 3, false);
    char h[128];

    snprintf(h, sizeof(h), BAND_ROW, ' ', "BAND", "MHZ", "WHAT YOU'LL HEAR", "BEST", "ANTENNA", "NOW", "RX");
    bd_head_l = label(p, &pixel_16, amb, 9, y + 2, 0, 0, h);
    rect(p, 6, y + 20, SW - 12, 3, 0, true);
    bd_rows = (scr_h - (y + 26) - 68) / rh;    /* three lines under them: the band, and what LISTEN sets */
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
    rect(p, 6, scr_h - 68, SW - 12, 3, 0, true);
    bd_about_l = label(p, &pixel_16, amb, 14, scr_h - 62, SW - 28, LV_TEXT_ALIGN_LEFT, "");
    lv_obj_set_height(bd_about_l, 56);  /* three lines, wrapped (CLIP would keep each on one line) */
}

static void build_log(lv_obj_t *p) {
    int y = BODY_Y + 2, rh = 18;

    rect(p, 6, y - 2, SW - 12, scr_h - y - 2, 3, false);
    char h[128];

    snprintf(h, sizeof(h), LOG_ROW, ' ', "FREQ", "BAND", "WHAT IT WAS", "BEST ", "ANTENNA", "LAST", "SEEN");
    lg_head_l = label(p, &pixel_16, amb, 9, y + 2, 0, 0, h);
    label(p, &pixel_12, amb, LOG_RX_X, y + 5, 0, 0, "RX: WHAT RECEIVES IT");
    rect(p, 6, y + 20, SW - 12, 3, 0, true);
    lg_rows = (scr_h - (y + 26) - 50) / rh;
    if (lg_rows > 20) lg_rows = 20;
    for (int r = 0; r < lg_rows; r++) {
        lg_row_l[r] = label(p, &pixel_16, amb, 9, y + 26 + r * rh, SW - 18, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(lg_row_l[r], true);        /* BEST: green open now, yellow closing, red closed */
        lv_obj_set_height(lg_row_l[r], rh);
        lv_label_set_long_mode(lg_row_l[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_bg_color(lg_row_l[r], amb, 0);
        lv_obj_add_flag(lg_row_l[r], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(lg_row_l[r], row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        lg_rx_l[r] = label(p, &pixel_12, amb, LOG_RX_X, y + 26 + r * rh + 3, SW - 9 - LOG_RX_X, LV_TEXT_ALIGN_LEFT,
                           "");        /* over the row's end, not clickable: a tap goes to the row */
        lv_label_set_recolor(lg_rx_l[r], true);
        lv_label_set_long_mode(lg_rx_l[r], LV_LABEL_LONG_CLIP);
    }
    rect(p, 6, scr_h - 50, SW - 12, 3, 0, true);
    lg_about_l = label(p, &pixel_16, amb, 14, scr_h - 44, SW - 28, LV_TEXT_ALIGN_LEFT, "");
}

static void build_decode(lv_obj_t *p) {
    int y = BODY_Y, h = scr_h - y - DLOW_H - 10, lw = 196;

    int tn = (h - 34) / 20;             /* the TYPE list's rows: as many as the box holds, 12 at most (tap_cb's 16) */

    box_make(&d_sel, p, 6, y, lw, h, "TYPE SELECT", DECODERS_N < tn ? DECODERS_N : tn, 3);
    rect(p, lw + 12, y, SW - lw - 18, h, 3, false);
    char hd[96];

    snprintf(hd, sizeof(hd), DEC_ROW, "KNOWN FREQ", "BAND", "BEST", "OPEN SETS", "NOW", "RX");
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

    int lh = 102, rx = lw + 12, rw = SW - lw - 18;     /* the load under what it does */

    box_make(&s_list, p, 6, y, lw, h, "SYSTEM", SYS_N, 5);
    box_make(&s_what, p, rx, y, rw, h - lh - 6, "WHAT IT DOES", 1, -1);
    lv_obj_set_height(s_what.opt[0], h - lh - 6 - 34);
    box_make(&s_load, p, rx, y + h - lh, rw, lh, "PROCESSOR, GRAPHICS, MEMORY", 0, -1);
    ld_w = (rw - 8 - 104 - 176) / MB_SEG * MB_SEG;
    for (int i = 0; i < 3; i++) {
        int ry = y + h - lh + 34 + i * 22;

        ld_val[i]   = label(p, &pixel_20, amb, rx + 8, ry - 4, 100, LV_TEXT_ALIGN_LEFT, "");
        ld_frame[i] = lv_img_buf_alloc(ld_w, MB_H, LV_IMG_CF_TRUE_COLOR);
        ld_img[i]   = lv_img_create(p);
        mbar_draw(ld_frame[i], ld_w, 0);
        lv_img_set_src(ld_img[i], ld_frame[i]);
        lv_obj_set_pos(ld_img[i], rx + 108, ry + 1);
        ld_more[i]  = label(p, &pixel_16, dim, rx + 116 + ld_w, ry - 2, rw - 124 - ld_w, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_long_mode(ld_more[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_height(ld_more[i], 18);
    }
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

/* Portrait, held up: 480 x 800, the title bar an L (down the right edge, where every screen has its bar at the
 * display's top, and across the top), the F keys' names in a column on the left where the keys are (160 px apart, F1
 * at the top under the bar, as the Walkie app has them), the rest under each other */
#define PW      480
#define PH      800
#define P_BAR   44                      /* the L's arms, as thick as every screen's title bar */
#define P_VX    (PW - 3 - P_BAR)        /* the arm down the right: the landscape bar's place (3 px from the edge) */
#define P_TOP   (6 + P_BAR + 6)         /* under the arm across the top */
#define P_KEY   160                     /* the keys' pitch: F1 at the top */
#define P_KEYLEN 80                     /* a physical F key's length along the screen's edge (its width with the radio
                                         * lying flat): F4's and F5's boxes as long, each centred on its key */
#define P_KW    76                      /* F5's box across: its name in two lines */
#define P_VW    30                      /* F4's across: "3D" */
#define P_KEYP  190                     /* the physical keys' pitch along the edge, and F1's middle (measured on the */
#define P_KEY0  6                       /* radio, 2026-11-14: F3 ~386, F4 ~577, F5 ~767 from the top held up) */
#define P_LAMP  P_KEYLEN                /* the lamp's row: F4's box and the lamp */
#define P_LAMPY (P_KEY0 + 3 * P_KEYP - P_KEYLEN / 2)
#define P_BOTY  (P_KEY0 + 4 * P_KEYP - P_KEYLEN / 2 < PH - 6 - P_KEYLEN ? P_KEY0 + 4 * P_KEYP - P_KEYLEN / 2 \
                                                                      : PH - 6 - P_KEYLEN)  /* F5's row, in the screen */
#define P_STATE (P_BOTY - 6 - (P_LAMPY + P_LAMP + 6))             /* the radio's state, between: three lines */
#define P_SX    (P_KW + 6)              /* beside F5 */
#define P_SW    (P_VX - 6 - P_SX)
#define P_FX    6                       /* beside no box (F1, F2, F3: CB's CH -/+ on F1 and F2): the widgets from the
                                         * screen's edge */
#define P_FW    (P_VX - 6 - P_FX)
/* The arm down the right as the landscape bar: its name's line 3 px under the bar's top, the state's 24 px under it
 * (their middles across the arm), the text from 60 to 740 along it as the bar's from x 60 to 740 */
#define P_VT    (PW - 3 - 3 - 10)
#define P_VS    (PW - 3 - 24 - 9)

#define VP_FFT  1024
#define VP_HOP  512
#define VP_HZ   4000            /* the voiceprint: 0-4 kHz up, the last seconds across */
#define VP_MAXH 300

typedef struct {
    box_t         ch, link, pow, sys;                 /* landscape: four boxes */
    lv_obj_t     *freq_l, *info_l, *msg_l;            /* portrait: the frequency, the state in lines, a word */
    lv_obj_t     *via_l;                              /* the radio serving the channel: VIA X6200, VIA RTL-SDR */
    lv_obj_t     *status_l, *batt_l;                  /* portrait: the title bar's state line, its battery icon */
    lv_obj_t     *vstatus_l, *vbatt_l;                /* and on the arm down the right, turned with the display */
    lv_obj_t     *ws_img, *ws_cap_l;                  /* portrait: the radio's spectrum around G's frequency */
    lv_img_dsc_t *ws_frame;
    int           ws_w, ws_h, ws_cap_x, ws_cap_y;
    int           cap_x, cap_y;                       /* the voiceprint's caption: its right edge, its top */
    lv_obj_t     *ab_arrow, *ab_tag;                  /* portrait: an arrow to the A/B key, while PTT is refused */
    lv_obj_t     *sm_img, *am_img;                    /* portrait: the S-meter and the audio as short bars */
    lv_img_dsc_t *sm_frame, *am_frame;
    int           mb_w, sm_lit, am_lit;
    lv_obj_t     *num_l, *band_l, *lamp, *lamp_l, *vp_img, *s_fill, *a_fill, *s_l, *a_l, *cap_l;
    lv_img_dsc_t *vp_frame;
    int           vp_w, vp_h, bar_w;
} wui_t;

static wui_t      w_land, w_port, *wui = &w_land;

/* G's picture: the X6200's spectrum 100 kHz around the dial, the trace over its last rows, these in 3D as A's
 * picture or flat as a waterfall (F4) */
#define WS_ROWS  48
#define WS_TRACE 34
static uint8_t  ws_rows[WS_ROWS][PW];
static float    ws_cols[PW];
static int      ws_head, ws_count;
static float    ws_floor = -300, ws_top = -60, ws_bot = -130;
static int64_t  ws_at, ws_span = 100000;    /* the frequency at the picture's middle, its width */
static uint32_t ws_seen;            /* the X6200's last frame drawn */
static bool     ws_3d = true;
static lv_obj_t  *g_scr, *g_prev_scr;           /* the portrait screen, and the one it took the place of */
static uint32_t   vp_pal[256];
static int16_t    vp_in[VP_FFT];
static int        vp_have;
static float      vp_floor = 20, vp_rms;
static uint32_t   ptt_ms;

/* A caption over a picture in a frame of its own (ink under it, an amber edge), so it reads over what is drawn: its
 * left edge at x (left), or its right edge */
static void tag_text(lv_obj_t *l, const char *t, int x, int y, bool left) {
    lv_coord_t w = lv_txt_get_width(t, strlen(t), lv_obj_get_style_text_font(l, 0), 0, LV_TEXT_FLAG_NONE) + 8;

    if (!strcmp(lv_label_get_text(l), t) && lv_obj_get_style_width(l, 0) == w) return;
    lv_label_set_text(l, t);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, left ? x : x - w, y);
}

static lv_obj_t *tag_make(lv_obj_t *p, lv_color_t c, const char *t, int x, int y, bool left) {
    lv_obj_t *l = lv_label_create(p);

    lv_obj_set_style_text_font(l, &pixel_16, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(l, ink, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l, 1, 0);
    lv_obj_set_style_border_color(l, c, 0);
    lv_obj_set_style_pad_top(l, 1, 0);
    lv_obj_set_style_pad_bottom(l, 1, 0);
    lv_label_set_text(l, "");
    tag_text(l, t, x, y, left);
    return l;
}

/* An arrow from (x0, y0) to its tip (x1, y1), hidden at first: one line, the shaft and the two strokes of its head
 * (pts: its five points, kept by the caller as the line keeps a pointer to them) */
static lv_obj_t *arrow_make(lv_obj_t *p, lv_point_t *pts, int x0, int y0, int x1, int y1) {
    float     dx = (float)(x1 - x0), dy = (float)(y1 - y0), l = sqrtf(dx * dx + dy * dy);
    float     bx = -dx / l * 16, by = -dy / l * 16, c = cosf(0.5f), sn = sinf(0.5f);
    lv_obj_t *ln = lv_line_create(p);

    pts[0] = (lv_point_t){(lv_coord_t)x0, (lv_coord_t)y0};
    pts[1] = (lv_point_t){(lv_coord_t)x1, (lv_coord_t)y1};
    pts[2] = (lv_point_t){(lv_coord_t)(x1 + bx * c - by * sn), (lv_coord_t)(y1 + bx * sn + by * c)};
    pts[3] = pts[1];
    pts[4] = (lv_point_t){(lv_coord_t)(x1 + bx * c + by * sn), (lv_coord_t)(y1 - bx * sn + by * c)};
    lv_line_set_points(ln, pts, 5);
    lv_obj_set_style_line_width(ln, 4, 0);
    lv_obj_set_style_line_color(ln, amb, 0);
    lv_obj_set_style_line_rounded(ln, true, 0);
    lv_obj_add_flag(ln, LV_OBJ_FLAG_HIDDEN);
    return ln;
}

/* A short bar: segments from the spectrum's deep orange to its yellow, lit up to level (0-1), the rest dim */
static void mbar_draw(lv_img_dsc_t *f, int w, float level) {
    uint32_t *px = (uint32_t *)f->data, bg = argb(ink);
    int       n  = w / MB_SEG, lit = (int)(level * n + 0.5f);

    for (int i = 0; i < w * MB_H; i++) px[i] = bg;
    for (int k = 0; k < n; k++) {
        uint32_t c = d3_pal[n > 1 ? k * 255 / (n - 1) : 255];

        c = k < lit ? 0xff000000 | c : scale_rgb(c, 0.2f);
        for (int y = 0; y < MB_H; y++)
            for (int x = k * MB_SEG; x < k * MB_SEG + MB_SEG - 2; x++) px[y * w + x] = c;
    }
}

/* Its name, the bar and its value (a_l, s_l), at x, y, w wide in all */
static void mbar_make(wui_t *u, lv_obj_t *p, int x, int y, int w, bool audio) {
    int bw = (w - 30 - 72) / MB_SEG * MB_SEG;

    label(p, &pixel_16, amb, x, y - 3, 28, LV_TEXT_ALIGN_LEFT, audio ? "AF" : "S");
    lv_img_dsc_t *f   = lv_img_buf_alloc(bw, MB_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_t     *img = lv_img_create(p);

    mbar_draw(f, bw, 0);
    lv_img_set_src(img, f);
    lv_obj_set_pos(img, x + 26, y);
    lv_obj_t *vl = label(p, &pixel_16, amb, x + 34 + bw, y - 3, w - 34 - bw, LV_TEXT_ALIGN_LEFT, "");    /* by its bar */

    u->mb_w = bw;
    if (audio) u->am_frame = f, u->am_img = img, u->a_l = vl, u->am_lit = -1;
    else u->sm_frame = f, u->sm_img = img, u->s_l = vl, u->sm_lit = -1;
}

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
    static float re[VP_FFT], im[VP_FFT], win[VP_FFT];
    float        col[VP_MAXH], sorted[VP_MAXH], bin_hz = 44100.0f / VP_FFT;
    int          w = wui->vp_w, h = wui->vp_h;
    uint32_t    *px = (uint32_t *)wui->vp_frame->data;

    if (win[VP_FFT / 2] == 0)           /* the Hann window once, not a cosf a sample 86 times a second */
        for (int i = 0; i < VP_FFT; i++) win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (VP_FFT - 1));
    for (int i = 0; i < VP_FFT; i++) {
        re[i] = vp_in[i] * win[i], im[i] = 0;
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
    vp_floor = 0.98f * vp_floor + 0.02f * float_kth(sorted, h, h / 3);     /* the hiss, followed slowly */
    if (g_gpu && wui == &w_port) {      /* one column into the GPU's ring: the picture is not moved a column (that
                                         * moved all of it, 86 times a second) */
        uint8_t lv[VP_MAXH];

        for (int y = 0; y < h; y++) {
            float v = (col[h - 1 - y] - vp_floor - 2) / 30;
            lv[y]   = v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255);
        }
        gpu_levels_col(3, vp_head, lv);
        vp_head = (vp_head + 1) % w;
        g_dirty = true;
        return;
    }
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

    w_sdr_step();
    if (!wui->vp_frame) return;
    bool from_rtl = w_sdr_hz != 0;      /* G on the SDR's channel: its audio, its level */

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
    if (cols && !(g_gpu && wui == &w_port)) lv_obj_invalidate(wui->vp_img);
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
    if (wui->sm_frame) {                /* the short bars, redrawn when a segment more or less is lit */
        int n = wui->mb_w / MB_SEG, ls = (int)(s * n + 0.5f), la = (int)(a * n + 0.5f);

        if (ls != wui->sm_lit) {
            wui->sm_lit = ls;
            mbar_draw(wui->sm_frame, wui->mb_w, s);
            lv_obj_invalidate(wui->sm_img);
        }
        if (la != wui->am_lit) {
            wui->am_lit = la;
            mbar_draw(wui->am_frame, wui->mb_w, a);
            lv_obj_invalidate(wui->am_img);
        }
        if (db >= -73) snprintf(t, sizeof(t), "S9+%d", (int)(db + 73));
        else snprintf(t, sizeof(t), "S%d", db <= -127 ? 0 : (int)((db + 127) / 6));
        lv_label_set_text(wui->s_l, t);
        snprintf(t, sizeof(t), "%d DB", vp_rms > 1 ? (int)(20 * log10f(vp_rms / 32768)) : -90);
        lv_label_set_text(wui->a_l, t);
    } else {
        lv_obj_set_width(wui->s_fill, (int)(s * wui->bar_w) + 1);
        lv_obj_set_width(wui->a_fill, (int)(a * wui->bar_w) + 1);
        if (from_rtl) snprintf(t, sizeof(t), "%s +%.0f DB", rtl_spectrum_dev_tag(),
                               rtl_spectrum_level() < 0 ? 0 : rtl_spectrum_level());
        else if (db >= -73) snprintf(t, sizeof(t), "S9+%d  %.0f DBM", (int)(db + 73), db);
        else snprintf(t, sizeof(t), "S%d  %.0f DBM", db <= -127 ? 0 : (int)((db + 127) / 6), db);
        lv_label_set_text(wui->s_l, t);
        snprintf(t, sizeof(t), "%d DBFS", vp_rms > 1 ? (int)(20 * log10f(vp_rms / 32768)) : -90);
        lv_label_set_text(wui->a_l, t);
    }
    /* the lamp: a voice (the audio up) or a carrier (the meter up); PTT refused, said in it */
    /* a signal: the audio 6 dB over its own floor (followed slowly: the hiss, the band's noise), or the stick's
     * squelch open where one is set */
    static float floor_rms = 1000;
    if (got) floor_rms = vp_rms < floor_rms ? 0.9f * floor_rms + 0.1f * vp_rms : 0.998f * floor_rms + 0.002f * vp_rms;
    bool on = from_rtl && param_i_get(cfg_rtl_sql) > 0 ? rtl_spectrum_squelch_open() : vp_rms > 2 * floor_rms + 100;
    /* a DMR call on the channel lights it too, whatever the audio: without the vocoder its voice is silence */
    rtl_dmr_t dmv;
    rtl_dsd_t ddv;
    bool      dmr = w_sdr_hz && rtl_spectrum_dmr(&dmv) && dmv.hz == w_sdr_hz &&
                    (dmv.slot[0].voice || dmv.slot[1].voice || dmv.slot[2].voice);
    bool      dsd = !dmr && w_sdr_hz && rtl_spectrum_dsd(&ddv) && ddv.hz == w_sdr_hz;     /* P25, D-STAR... */
    char      dlamp[32];

    if (dsd) snprintf(dlamp, sizeof(dlamp), g_portrait ? "RX  %s" : "RX\n%s", rtl_dsd_name(ddv.proto));
    bool ptt = ptt_ms && lv_tick_get() - ptt_ms < 1500;
    /* on the air: the X6200 (not on the SDR's channels, where it is held at 0 W whatever keys it), or the SDR */
    bool air   = w_sdr_hz ? w_sdr_ptt && rtl_spectrum_tx_state() == RTL_TX_ON : radio_get_state() == RADIO_TX;
    bool armed = w_sdr_hz ? g_sdr_armed : g_armed;

    on = on || dmr || dsd;
    lv_obj_set_style_bg_opa(wui->lamp, air || ptt || on || (armed && blink) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(wui->lamp_l, air || ptt || on || (armed && blink) ? ink : amb, 0);
    lv_label_set_text(wui->lamp_l, air ? (g_portrait ? "TX  ON AIR" : "TX\nON AIR")
                                   : ptt ? (g_portrait ? "TX LOCKED" : "TX\nLOCKED")
                                   : armed ? (g_portrait ? "TX  ARMED" : "TX\nARMED")
                                   : dmr ? (g_portrait ? "RX  DMR" : "RX\nDMR")
                                   : dsd ? dlamp
                                   : on ? (g_portrait ? "RX  SIGNAL" : "RX\nSIGNAL")
                                        : (g_portrait ? "RX  QUIET" : "RX\nQUIET"));
    char cap[48];

    snprintf(cap, sizeof(cap), "VOICEPRINT  %s  0-4 KHZ", air ? "TX" : from_rtl ? rtl_spectrum_dev_tag() : "RX");
    tag_text(wui->cap_l, cap, wui->cap_x, wui->cap_y, false);
    if (g_portrait && wui->msg_l && ptt_ms && lv_tick_get() - ptt_ms > 4000) {
        lv_label_set_text(wui->msg_l, "");
        if (wui->ab_arrow) lv_obj_add_flag(wui->ab_arrow, LV_OBJ_FLAG_HIDDEN);
        if (wui->ab_tag) lv_obj_add_flag(wui->ab_tag, LV_OBJ_FLAG_HIDDEN);
        ptt_ms = 0;
    }
}

/* The A/B (VFO) key on A and G: the transmitter armed for PTT (the Sentry's receive-only hold let go: the radio's own
 * TX rules again, its power and band limits), or locked again. A/B again or leaving A and G locks it */
static void walkie_arm(bool on) {
    char t[64];

    if (on == g_armed) return;
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

/* PTT pressed with TX locked (the Sentry is receive only): refused, and said what unlocks it and where that key is:
 * A/B on the top panel, on A or G (the screens that transmit) */
static void walkie_ptt(void) {
    lv_obj_t *a = g_portrait ? wui->ab_arrow : ab_arrow, *t = g_portrait ? wui->ab_tag : ab_tag;

    ptt_ms = lv_tick_get() | 1;
    if (a) lv_obj_clear_flag(a, LV_OBJ_FLAG_HIDDEN);    /* where the A/B key is */
    if (t) lv_obj_clear_flag(t, LV_OBJ_FLAG_HIDDEN);
    if (scr == SCR_SCAN || scr == SCR_WALKIE)
        w_say("TX LOCKED: PRESS A/B ON THE TOP PANEL TO UNLOCK, THEN PTT", "TX LOCKED",
              "PTT REFUSED. PRESS A/B ON THE TOP PANEL TO UNLOCK TX, THEN PTT. A/B AGAIN LOCKS IT.");
    else
        osd("TX LOCKED", "PTT REFUSED. TURN THE RING TO A OR G, THEN PRESS A/B ON THE TOP PANEL TO UNLOCK TX.", -1);
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
    bool        sdr;            /* the SDR's (a HackRF, a bladeRF): heard and sent through it, FM */
} wch_t;

static const struct {
    const char *name;
    int         x62;            /* the X6200's radio_mode_t, -2 SSB (its sideband by the band: w_lsb) */
} w_modes[] = {
    {"AM", radio_mode_am},
    {"FM", radio_mode_nfm},
    {"SSB", -2},
};
#define W_MODES (int)(sizeof(w_modes) / sizeof(w_modes[0]))
enum { WM_AM, WM_FM, WM_SSB };

static wch_t w_chs[160];
static int   w_n, w_mode = WM_AM;

static void w_add(const char *grp, const char *name, int32_t hz, int mode, bool home) {
    if (w_n < (int)N_OF(w_chs)) w_chs[w_n++] = (wch_t){grp, name, hz, (uint8_t)mode, home, false};
}

/* The SDR's FM simplex channels, Region 2's band plans (the owner's): each group's national calling channel first.
 * Only amateur bands (tx_ranges_sdr_check): no MURS, marine, FRS/GMRS or NOAA here, where nothing may be sent */
static const struct {
    const char *grp, *name;
    int32_t     hz;
} w_sdr_plan[] = {
    {"2 M FM", "CALL", 146520000}, {"2 M FM", "S2", 146550000}, {"2 M FM", "S3", 146580000},
    {"2 M FM", "S4", 147420000},   {"2 M FM", "S5", 147450000}, {"2 M FM", "S6", 147480000},
    {"2 M FM", "S7", 147510000},   {"2 M FM", "S8", 147540000}, {"2 M FM", "S9", 147570000},
    {"1.25 M FM", "CALL", 223500000}, {"1.25 M FM", "S2", 223420000}, {"1.25 M FM", "S3", 223460000},
    {"70 CM FM", "CALL", 446000000}, {"70 CM FM", "S2", 446500000}, {"70 CM FM", "S3", 445950000},
    {"70 CM FM", "S4", 446050000},   {"70 CM FM", "S5", 446100000},
    /* DMR's ham simplex channels (North America's; 433.450 Europe's), heard as DMR (the daemon's dmr.c), never
     * transmitted on in FM (w_sdr_refusal) */
    {"DMR", "CALL", 441000000}, {"DMR", "S2", 446500000}, {"DMR", "S3", 446075000},
    {"DMR", "S4", 433450000},   {"DMR", "2M", 145790000}, {"DMR", "2M2", 145510000},
    {"33 CM FM", "CALL", 927500000},
    {"23 CM FM", "CALL", 1294500000},
};

/* An SDR on the HOST port, its daemon up: G's plan has its groups. Any of them hears the channels (an RTL-SDR too); only
 * one that transmits (a HackRF, a bladeRF: rtl_spectrum_can_tx) is armed by A/B */
static bool w_sdr_here(void) {
    return rtl_spectrum_daemon() && rtl_spectrum_dev() != RTL_DEV_NONE;
}

/* D's band i as a group of its own with one channel, where D listens in it (its starting channel): if the X6200
 * tunes it (G is the X6200's alone: the stick's bands are B's) */
static void w_band(int i) {
    int m;

    if (i < 0 || i >= BANDS_N || !cfg_is_valid_hw_freq(bands[i].listen_hz) || bands[i].listen_hz >= 60000000) return;
    m = bands[i].mode;
    w_add(bands[i].name, "1", bands[i].listen_hz, m == radio_mode_nfm ? WM_FM : m == radio_mode_am ? WM_AM : WM_SSB,
          true);
}

static int band_named(const char *name) {
    for (int i = 0; i < BANDS_N; i++)
        if (!strcmp(bands[i].name, name)) return i;
    return -1;
}

static void w_plan(void) {
    static const char *const n40[] = {"01", "02", "03", "04", "05", "06", "07", "08", "09", "10", "11", "12", "13",
                                      "14", "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26",
                                      "27", "28", "29", "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
                                      "40"};

    static int sdr_in = -1;     /* the plan made with the SDR's groups (1) or without (0) */

    if (w_n && sdr_in == (int)w_sdr_here()) return;
    w_n    = 0;                     /* made again: an SDR came or went */
    sdr_in = w_sdr_here();
    for (int i = 0; i < BANDS_N && bands[i].hi < 26965; i++) w_band(i);     /* MF and HF, each its starting channel */
    for (int i = 0; i < 40; i++) w_add("CB", n40[i], cb_khz[i] * 1000, WM_AM, i == 18);
    w_band(band_named("10 M"));
    w_add("10 M FM", "S", 29600000, WM_FM, true);
    w_band(band_named("6 M"));
    w_add("6 M FM", "S", 52525000, WM_FM, true);
    for (int i = 0; sdr_in && i < (int)N_OF(w_sdr_plan); i++) {     /* the SDR's, after the X6200's */
        int64_t hz = w_sdr_plan[i].hz;

        if (hz < rtl_spectrum_min_hz() || hz > rtl_spectrum_max_hz() || w_n >= (int)N_OF(w_chs)) continue;
        w_chs[w_n++] = (wch_t){w_sdr_plan[i].grp, w_sdr_plan[i].name, w_sdr_plan[i].hz, WM_FM,
                               !strcmp(w_sdr_plan[i].name, "CALL"), true};
    }
    if (w_at >= w_n) w_at = -1;
}

/* The channel of the plan named so in its group (CB "19"), -1 none */
static int w_find(const char *grp, const char *name) {
    w_plan();
    for (int i = 0; i < w_n; i++)
        if (!strcmp(w_chs[i].grp, grp) && !strcmp(w_chs[i].name, name)) return i;
    return -1;
}

/* Where G starts for a frequency: the home channel of its band (D's), or of the nearest band between two */
static int w_start(int64_t hz) {
    int     b    = -1;
    int64_t best = INT64_MAX;

    w_plan();
    for (int i = 0; i < BANDS_N; i++) {
        int64_t lo = (int64_t)bands[i].lo * 1000, hi = (int64_t)bands[i].hi * 1000;
        int64_t d  = hz < lo ? lo - hz : hz > hi ? hz - hi : 0;
        bool    has = false;

        for (int k = 0; k < w_n && !has; k++) has = w_chs[k].hz >= lo && w_chs[k].hz <= hi;
        if (has && d < best) best = d, b = i;
    }
    if (b < 0) return w_find("CB", "19");
    int64_t lo = (int64_t)bands[b].lo * 1000, hi = (int64_t)bands[b].hi * 1000;
    int     first = -1;

    for (int k = 0; k < w_n; k++)
        if (w_chs[k].hz >= lo && w_chs[k].hz <= hi) {
            if (!strcmp(w_chs[k].grp, bands[b].name) || w_chs[k].home) return k;
            if (first < 0) first = k;
        }
    return first;
}

/* G is the X6200's alone (the stick only receives, and has B for that), read live: its dial and mode are G's
 * frequency and mode, G never tunes by itself, so A's scan moving the dial moves G and TX armed here is on the band A
 * was scanning */
static int64_t w_hz(void) {
    return w_sdr_hz ? w_sdr_hz : cparam_i_get(cfg_fg_freq);     /* on the SDR: its channel */
}

/* G's mode, the X6200's (CW and the data modes as SSB) */
static int w_mode_now(void) {
    if (w_sdr_hz) return WM_FM;         /* the SDR's walkie: narrow FM */
    int m = cparam_i_get(cfg_cur_mode);

    return m == radio_mode_am ? WM_AM : m == radio_mode_nfm ? WM_FM : WM_SSB;
}

static int w_match(void) {
    int64_t hz = w_hz();

    w_plan();
    for (int i = 0; i < w_n; i++)
        if (llabs(w_chs[i].hz - hz) <= 3000) return i;
    return -1;
}

/* A service with defined channels (on the X6200: CB): the arrows (BAND, CH -/+) step them; the other
 * groups are a place to start. The VFO always tunes the dial, fine, on CB too */
static bool w_chan(int i) {
    return i >= 0 && i < w_n && !strcmp(w_chs[i].grp, "CB");
}

/* The dial in CB's span (channel 1 to 40, a channel's width either side): on a channel or tuned between two */
static bool w_cb_here(void) {
    int64_t hz = w_hz();

    return hz >= 26965000 - 5000 && hz <= 27405000 + 5000;
}

/* SSB's sideband as hams use it: lower on 160, 80 and 40 m, upper everywhere else (60 m, the ship and utility
 * stations, 20 m up) */
static bool w_lsb(int64_t hz) {
    return (hz >= 1800000 && hz <= 2000000) || (hz >= 3500000 && hz <= 4000000) || (hz >= 7000000 && hz <= 7300000);
}

/* The modes F3 goes through in group g, own (its channels' mode) first: CB's AM, SSB and FM; the ham bands' SSB and
 * AM; one for the rest */
static int w_grp_modes(const char *g, int own, int *m) {
    static const char *const ham[] = {"160 M", "80 M", "40 M", "20 M", "17 M", "15 M", "12 M", "10 M", "6 M"};
    int                      n     = 0;

    m[n++] = own;
    if (!strcmp(g, "CB")) {
        m[n++] = WM_SSB;
        m[n++] = WM_FM;
    } else if (own == WM_SSB) {
        for (int k = 0; k < (int)N_OF(ham); k++)
            if (!strcmp(g, ham[k])) m[n++] = WM_AM;
    }
    return n;
}

/* The X6200's mode for m at hz */
static int w_x62(int m, int64_t hz) {
    int xm = w_modes[m].x62;

    return xm == -2 ? (w_lsb(hz) ? radio_mode_lsb : radio_mode_usb) : xm;
}

/* G chose by hand: the Sentry stops moving the dial, as for a turn of A's VFO (MAN-OVERRIDE) */
static void x62_by_hand(void) {
    if (smode == MODE_MAN) return;
    smode = MODE_MAN;
    if (routine == ROUT_SCAN) routine = ROUT_BAND;
    if (trk.on) trk.held = true;
}

/* A word on G's own line (held up), or the OSD */
static void w_say(const char *portrait, const char *title, const char *text) {
    if (g_portrait && wui->msg_l) lv_label_set_text(wui->msg_l, portrait);
    else osd(title, text, -1);
}

/* ---- G on the SDR: its channels heard through any SDR, sent through a HackRF or a bladeRF ----
 *
 * One of the plan's SDR channels chosen, G is the SDR's: tuned G_STICK_OFF over the channel (its middle unused), the
 * channel heard on the speaker in FM at B's volume, G's picture and voiceprint from it, the VFO moving it 5 kHz a
 * click. A/B arms its transmitter (the X6200 stays held at 0 W); PTT (the top key, or the hand mic's) transmits FM
 * from the hand mic while held, where tx_ranges_sdr_check allows and nowhere else. Leaving G, or an X6200 channel
 * chosen, ends it all and the X6200 is heard again */

static uint32_t w_sdr_gone_ms;          /* the SDR said it no longer transmits (pulled out): G leaves it 5 s on */
static uint32_t w_sdr_said_n;           /* the daemon's last word on the transmitter, seen */
static uint32_t w_sdr_quiet_ms;         /* transmitting with no voice from the mic since */

static void w_sdr_go(int64_t hz) {
    w_sdr_hz = hz;
    rtl_spectrum_squelch(param_i_get(cfg_rtl_sql));
    rtl_spectrum_tune(hz + G_STICK_OFF);
    stick_at  = hz + G_STICK_OFF;
    heard     = HEAR_RTL;
    hear_want = HEAR_RTL;
    hear_vol_refresh();
    rtl_spectrum_mute(false);
    rtl_spectrum_mux(false);
    rtl_spectrum_listen(hz, 1);
    rtl_spectrum_scope(scr == SCR_WALKIE);
}

/* The SDR's TX gain in the words of the screen: "TX 10 DB", "TX 47 DB + AMP" */
static void w_sdr_power(char *t, size_t n) {
    snprintf(t, n, "TX %d DB%s", w_sdr_gain, w_sdr_amp ? " + AMP" : "");
}

/* Its transmitter armed (on) or locked: the X6200 is never let go for it */
static void w_sdr_arm(bool on) {
    char t[80], pw[24];

    if (on == g_sdr_armed) return;
    if (!on && w_sdr_ptt) w_sdr_key(false, w_sdr_by);
    g_sdr_armed = on;
    w_sdr_power(pw, sizeof(pw));
    if (on) {
        if (routine == ROUT_SCAN || routine == ROUT_SWEEP) set_routine(ROUT_BAND);
        snprintf(t, sizeof(t), "%s ARMED: PTT TRANSMITS (%s)", rtl_spectrum_dev_tag(), pw);
    } else {
        snprintf(t, sizeof(t), "TX LOCKED AGAIN");
    }
    if (g_portrait && wui->msg_l) lv_label_set_text(wui->msg_l, t);
    else osd(on ? "TX ARMED" : "TX LOCKED",
             on ? "PTT (THE TOP KEY OR THE HAND MIC'S) TRANSMITS FM THROUGH THE SDR FROM THE HAND MIC, ONLY IN THE "
                  "AMATEUR BANDS. THE X6200 STAYS RECEIVE ONLY. A/B AGAIN, OR LEAVING G, LOCKS IT."
                : "THE SENTRY IS RECEIVE ONLY AGAIN.", -1);
    for (int i = 0; i < 5; i++) buttons_refresh(&btn[i]);
    show_walkie();
}

/* Why G's SDR may not transmit at its channel now, NULL when it may */
static const char *w_sdr_refusal(void) {
    static char only[40];

    if (!rtl_spectrum_can_tx()) {
        snprintf(only, sizeof(only), "THE %s ONLY RECEIVES", rtl_spectrum_dev_name());
        return only;
    }
    if (cfg_tx_block()) return "THE WR12 HOLDS TX";
    for (int i = 0; i < (int)N_OF(w_sdr_plan); i++)     /* FM on a DMR channel: never (no DMR transmitter here) */
        if (w_sdr_plan[i].hz == w_sdr_hz && !strcmp(w_sdr_plan[i].grp, "DMR")) return "A DMR CHANNEL: RECEIVE ONLY";
    {
        rtl_dmr_t m;

        if (rtl_spectrum_dmr(&m) && m.hz == w_sdr_hz) return "DMR ON THE CHANNEL: RECEIVE ONLY";
    }
    switch (tx_ranges_sdr_check((tx_ranges_t)param_i_get(cfg_tx_ranges), (int32_t)w_sdr_hz)) {
        case HAM_TX_OK: return NULL;
        case HAM_TX_OFF_RANGE: return "OUTSIDE THE TX RANGES (SETTINGS > TX CAL)";
        default: return "NOT AN AMATEUR BAND";
    }
}

/* PTT down or up on G's SDR channel: by 1 the top key, 2 the hand mic's (the base keyed itself, held at 0 W) */
static void w_sdr_key(bool down, uint8_t by) {
    char        t[96];
    const char *no;

    if (!down) {
        if (!w_sdr_ptt || by != w_sdr_by) return;
        rtl_spectrum_tx(0, 0, false);
        w_sdr_ptt = false;
        show_walkie();
        return;
    }
    if (w_sdr_ptt) return;
    if (!g_sdr_armed) {
        if (by == 1) walkie_ptt();      /* the same refusal as the X6200's: A/B first */
        return;
    }
    if ((no = w_sdr_refusal())) {
        snprintf(t, sizeof(t), "NO TX: %s", no);
        w_say(t, "NO TX", t);
        ptt_ms = lv_tick_get() | 1;
        return;
    }
    rtl_spectrum_tx(w_sdr_hz, w_sdr_gain, w_sdr_amp);
    w_sdr_ptt      = true;
    w_sdr_by       = by;
    w_sdr_quiet_ms = lv_tick_get();
    show_walkie();
}

/* F3 on G's SDR channels: the TX gain, a step up and round (a HackRF's amplifier the last step: its +14 dB) */
static void w_sdr_gain_next(void) {
    static const int steps[] = {0, 5, 10, 15, 20, 30, 40, 47};
    char             t[64], pw[24];
    int              i = 0;

    if (!w_sdr_hz) return;
    if (w_sdr_ptt) {
        w_say("NOT WHILE TRANSMITTING", "TX GAIN", "NOT WHILE TRANSMITTING.");
        return;
    }
    while (i < (int)N_OF(steps) && steps[i] <= w_sdr_gain) i++;
    if (w_sdr_amp) {                    /* past the top with the amplifier: back to nothing */
        w_sdr_amp  = false;
        w_sdr_gain = 0;
    } else if (i < (int)N_OF(steps)) {
        w_sdr_gain = steps[i];
    } else if (rtl_spectrum_dev() == RTL_DEV_HACKRF) {
        w_sdr_amp = true;               /* 47 dB, then its amplifier */
    } else {
        w_sdr_gain = 0;
    }
    w_sdr_power(pw, sizeof(pw));
    snprintf(t, sizeof(t), "%s%s", pw, w_sdr_amp ? ": AMP ON, +14 DB" : "");
    w_say(t, "TX GAIN", w_sdr_amp ? "THE HACKRF'S AMPLIFIER ON: ABOUT +14 DB. F3 AGAIN: BACK TO 0 DB, AMP OFF."
                                  : "THE SDR'S TRANSMIT GAIN FOR THE NEXT OVER. F3 STEPS IT.");
    show_walkie();
}

/* The VFO on G's SDR channel: 5 kHz a click, within what it tunes (where it may transmit is PTT's business) */
static void w_sdr_turn(int32_t diff) {
    int64_t hz = (w_sdr_hz + diff * 5000LL) / 5000 * 5000;

    if (hz < rtl_spectrum_min_hz() + G_STICK_OFF) hz = rtl_spectrum_min_hz() + G_STICK_OFF;
    if (hz > rtl_spectrum_max_hz() - G_STICK_OFF) hz = rtl_spectrum_max_hz() - G_STICK_OFF;
    w_sdr_go(hz);
    show_walkie();
}

/* G off the SDR (another screen, an X6200 channel, the SDR gone): its transmitter off and locked, the X6200 heard */
static void w_sdr_left(void) {
    if (!w_sdr_hz && !g_sdr_armed) return;
    if (w_sdr_ptt) {
        rtl_spectrum_tx(0, 0, false);
        w_sdr_ptt = false;
    }
    g_sdr_armed = false;
    if (heard == HEAR_RTL) hear_apply(HEAR_X6200);
    rtl_spectrum_scope(false);
}

/* Each 50 ms on G: the hand mic's PTT (the base keyed itself, held at 0 W) keys the SDR while armed; what the daemon
 * said of its transmitter; no voice from the mic; the SDR gone */
static void w_sdr_step(void) {
    char said[48];

    if (w_sdr_resume && !w_sdr_hz && w_sdr_here()) {    /* the SDR there after a restart: G where it was */
        w_plan();
        w_sdr_go(w_sdr_resume);
        w_at   = w_match();
        w_mode = WM_FM;
        show_walkie();
    }
    if (w_sdr_resume && lv_tick_get() > 20000) w_sdr_resume = 0;   /* not there by then: G stays the X6200's */
    if (!w_sdr_hz) return;
    if (g_sdr_armed) {
        bool base = radio_get_state() == RADIO_TX;

        if (base && !w_sdr_ptt) w_sdr_key(true, 2);
        else if (!base && w_sdr_ptt && w_sdr_by == 2) w_sdr_key(false, 2);
    }
    uint32_t n = rtl_spectrum_tx_said(said, sizeof(said));

    if (n != w_sdr_said_n) {
        char t[96];

        w_sdr_said_n = n;
        if (!strncmp(said, "NO ", 3)) {
            snprintf(t, sizeof(t), "NO TX (THE SDR): %s", said + 3);
            w_say(t, "NO TX", t);
        } else if (!strncmp(said, "END ", 4) && strcmp(said + 4, "ASKED") && strcmp(said + 4, "GONE")) {
            snprintf(t, sizeof(t), "TX ENDED: %s", !strcmp(said + 4, "IDLE") ? "NO MIC AUDIO FOR 2 S"
                                                 : !strcmp(said + 4, "TIMEOUT") ? "3 MINUTES IN ONE GO" : said + 4);
            w_say(t, "TX ENDED", t);
        }
        if (strncmp(said, "ON", 2) && w_sdr_ptt) {      /* it ended by itself: PTT again to go on */
            rtl_spectrum_tx(0, 0, false);
            w_sdr_ptt = false;
        }
        show_walkie();
    }
    if (w_sdr_ptt && rtl_spectrum_tx_state() == RTL_TX_ON) {
        if (rtl_spectrum_tx_mic_db() > -60) w_sdr_quiet_ms = lv_tick_get();
        else if (lv_tick_get() - w_sdr_quiet_ms > 3000) {
            w_say("TX: NO MIC AUDIO (THE HAND MIC CARRIES THE VOICE)", "NO MIC AUDIO",
                  "NOTHING FROM THE HAND MIC: IS IT PLUGGED IN? THE RADIO'S OWN MIC IS NOT HEARD BY LINUX.");
            w_sdr_quiet_ms = lv_tick_get();
        }
    }
    if (w_sdr_here() || w_sdr_ptt) {   /* still there (an RTL-SDR that only hears is there too) */
        w_sdr_gone_ms = 0;
    } else if (!w_sdr_gone_ms) {
        w_sdr_gone_ms = lv_tick_get() | 1;
    } else if (lv_tick_get() - w_sdr_gone_ms > 5000) {  /* pulled out: G back on the X6200, at its dial */
        w_sdr_gone_ms = 0;
        w_sdr_left();
        w_sdr_hz = 0;
        w_say("THE SDR WENT: G IS THE X6200'S AGAIN", "SDR GONE", "G IS THE X6200'S AGAIN.");
        walkie_enter();
        show_walkie();
    }
}

/* Channel i in mode m: the SDR's on the SDR, the X6200's on the X6200 (G off the SDR for it). Not while the SDR
 * transmits */
static void walkie_go(int i, int m) {
    const wch_t *c = &w_chs[i];

    if (w_sdr_ptt) {
        w_say("NOT WHILE TRANSMITTING", "TRANSMITTING", "LET GO OF PTT FIRST.");
        return;
    }
    if (c->sdr) {
        if (g_sdr_armed && w_sdr_hz && llabs(c->hz - w_sdr_hz) > 1000) w_sdr_arm(false);  /* armed for that one */
        w_sdr_go(c->hz);
        w_at   = i;
        w_mode = WM_FM;
        show_walkie();
        for (int k = 0; k < 5; k++) buttons_refresh(&btn[k]);
        return;
    }
    if (w_sdr_hz) {
        w_sdr_left();
        w_sdr_hz = 0;
    }
    w_sdr_resume = 0;                   /* chosen by hand */
    if (m < 0 || m >= W_MODES) m = c->mode;
    x62_by_hand();
    tune(c->hz, w_x62(m, c->hz));
    w_at   = i;
    w_mode = m;
    show_walkie();
    for (int k = 0; k < 5; k++) buttons_refresh(&btn[k]);
}

/* Mode m where the dial is */
static void w_mode_here(int m) {
    x62_by_hand();
    tune((int32_t)w_hz(), w_x62(m, w_hz()));
    w_mode = m;
    show_walkie();
    for (int k = 0; k < 5; k++) buttons_refresh(&btn[k]);
}

/* The next group (dir 1: the MFK clockwise, F5 past a group's last mode) or the one before, at its home channel in its own mode: every band
 * of D's the X6200 tunes is a group, so each has a channel to start on */
static void walkie_group(int dir) {
    int         i = w_at < 0 ? w_start(w_hz()) : w_at;
    const char *g;

    w_plan();
    if (i < 0) i = 0;
    g = w_chs[i].grp;
    if (dir > 0) {
        while (i < w_n && !strcmp(w_chs[i].grp, g)) i++;    /* the next group */
        if (i >= w_n) i = 0;
    } else {
        while (i >= 0 && !strcmp(w_chs[i].grp, g)) i--;    /* the last channel of the group before */
        if (i < 0) i = w_n - 1;
        while (i > 0 && !strcmp(w_chs[i - 1].grp, w_chs[i].grp)) i--;    /* its first */
    }
    for (int k = i; k < w_n && !strcmp(w_chs[k].grp, w_chs[i].grp); k++)
        if (w_chs[k].home) {
            i = k;
            break;
        }
    walkie_go(i, w_chs[i].mode);
}

/* F3 on G: the groups and each one's modes in turn (CB AM, CB SSB, CB FM, 10 M SSB, 10 M AM, 10 M FM...): the next
 * mode where the dial is while its group has one more, else the next group at its home channel */
static void walkie_group_mode(void) {
    int i = w_at >= 0 ? w_at : w_start(w_hz()), ms[4], n, k, now = w_mode_now();

    if (i < 0) return;
    n = w_grp_modes(w_chs[i].grp, w_chs[i].mode, ms);
    for (k = 0; k < n && ms[k] != now; k++) {}
    if (k == n) w_mode_here(ms[0]);     /* a mode not the group's (CW): its own first */
    else if (k + 1 < n) w_mode_here(ms[k + 1]);
    else walkie_group(1);
}

/* G shown: where the X6200 is, nothing tuned; the X6200 alone on the speaker (the stick's audio, alone or mixed, is
 * not G's; A or B puts its own LISTEN back when shown) */
static void walkie_enter(void) {
    w_plan();
    if (w_sdr_hz && w_sdr_here()) {     /* left on the SDR's channel: there again */
        w_sdr_go(w_sdr_hz);
        w_at   = w_match();
        w_mode = WM_FM;
        return;
    }
    w_sdr_hz = 0;
    if (heard == HEAR_RTL || heard == HEAR_BOTH) hear_apply(HEAR_X6200);
    w_at   = w_match();
    w_mode = w_mode_now();
}

/* The arrows on CB (BAND UP/DOWN, CH -/+): the next CB channel by number, the mode kept; tuned between two,
 * the nearest one that way; to: CB channel to (the MFK's push: 19) */
static void walkie_ch(int dir, int to) {
    int     i = w_match(), k = -1;
    int64_t hz = w_hz();
    char    cb[12];

    if (w_sdr_hz && !to) {              /* on the SDR: its group's channels, round; off them, the group's call */
        if (dial_held()) return;
        const char *g = i >= 0 ? w_chs[i].grp : NULL;

        if (!g) {
            for (int j = 0; j < w_n && k < 0; j++)
                if (w_chs[j].sdr && w_chs[j].home) k = j;
        } else {
            k = i;
            do {
                k = (k + dir + w_n) % w_n;
            } while (k != i && strcmp(w_chs[k].grp, g));
        }
        if (k >= 0) walkie_go(k, WM_FM);
        return;
    }
    if (to) {
        snprintf(cb, sizeof(cb), "%02d", to);
        if ((k = w_find("CB", cb)) >= 0) walkie_go(k, w_cb_here() ? w_mode_now() : w_chs[k].mode);
        return;
    }
    if (!w_cb_here()) return;
    if (w_chan(i)) {
        k = i;
        do {
            k = (k + dir + w_n) % w_n;
        } while (k != i && !w_chan(k));
    } else {                            /* between two: the nearest that way, round to the far end past the last */
        for (int j = 0; j < w_n; j++)
            if (w_chan(j) && (dir > 0 ? w_chs[j].hz > hz : w_chs[j].hz < hz) &&
                (k < 0 || llabs(w_chs[j].hz - hz) < llabs(w_chs[k].hz - hz)))
                k = j;
        for (int j = 0; k < 0 && j < w_n; j++)
            if (w_chan(j) && (k < 0 || (dir > 0 ? w_chs[j].hz < w_chs[k].hz : w_chs[j].hz > w_chs[k].hz))) k = j;
        if (k < 0) return;
    }
    walkie_go(k, w_mode_now());
}

/* G's arrows (BAND UP/DOWN, F1 and F2): a step either way. On CB and the SDR's groups the next channel (walkie_ch);
 * elsewhere the X6200's dial by A's step (SCAN CFG's STEP, 10 kHz unless set), to the step's next multiple that way,
 * the mode kept: the VFO tunes fine, the MFK goes from group to group */
static void walkie_arrow(int dir) {
    if (w_cb_here() || w_sdr_hz) {
        walkie_ch(dir, 0);
        return;
    }
    if (dial_held()) return;
    int32_t st = a_steps[a_step], fq = cparam_i_get(cfg_fg_freq);
    int32_t to = dir > 0 ? (fq / st + 1) * st : (fq - 1) / st * st;

    x62_by_hand();
    if (!tune(to, -1)) return;
    w_at = w_match();
    show_walkie();
    for (int k = 0; k < 5; k++) buttons_refresh(&btn[k]);
}

/* TX unlocked (armed) or on the air: the knobs that tune the X6200 are held, so a bump cannot move the transmitter */
static bool dial_held(void) {
    if (!g_armed && !g_sdr_armed && radio_get_state() != RADIO_TX) return false;
    w_say("DIAL LOCKED: LOCK TX (A/B) TO TUNE", "DIAL LOCKED",
          "TX IS UNLOCKED, SO THE TUNING KNOBS ARE LOCKED AGAINST A BUMP. LOCK TX (A/B) TO TUNE AGAIN.");
    return true;
}

/* The F keys' names on G: CH- and CH+ on a service's channels, STEP - and STEP + elsewhere (no box held up), nothing
 * on F3, the picture's view on F4 (a slim box), the group and mode on F5 */
static void walkie_key_label(int k, char *s, size_t n) {
    int i = w_match(), g;

    if (k <= 1) {
        snprintf(s, n, "%s", w_cb_here() || w_sdr_hz ? (k ? "CH +" : "CH -") : (k ? "STEP +" : "STEP -"));
    } else if (k == 2) {
        if (w_sdr_hz) w_sdr_power(s, n);
        else s[0] = 0;
    } else if (k == 3) {
        snprintf(s, n, "%s", ws_3d ? "3D" : "2D");
    } else {
        g = i >= 0 ? i : w_start(w_hz());
        snprintf(s, n, "%s\n%s", g >= 0 ? w_chs[g].grp : "-", w_modes[w_mode_now()].name);
    }
}

/* ---- G's picture: the X6200's spectrum around the dial ---- */

static uint8_t ws_level(float db) {
    int v = (int)((db - (ws_floor + 2)) * 255 / 34);
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

/* The picture drawn: the trace over its dB grid, the dial bracketed in the middle, then the last rows: in 3D (A's
 * perspective, the newest in front) or flat (a waterfall, the newest on top) */
static void ws_draw(void) {
    wui_t *u = wui;

    if (!u->ws_frame) return;
    uint32_t *px = (uint32_t *)u->ws_frame->data, bg = argb(ink), grid = argb(faint), line = argb(amb);
    int       w = u->ws_w, h = u->ws_h, th = WS_TRACE, rh = h - th, prev = -1;

    if (g_gpu) {        /* the GPU draws it (g_gpu_draw): the trace per column, as A/B's */
        static uint8_t tr[PW][4];
        int            hm = th - 1;

        for (int x = 0; x < w; x++) {
            int y = th - 1 - (int)((ws_cols[x] - ws_bot) / (ws_top - ws_bot) * (th - 1));
            y     = y < 0 ? 0 : y > th - 1 ? th - 1 : y;
            int y0 = prev < 0 ? y : prev < y ? prev : y, y1 = prev < 0 ? y : prev > y ? prev : y;

            tr[x][0] = (uint8_t)((y * 255 + hm / 2) / hm);
            tr[x][1] = (uint8_t)((y0 * 255 + hm / 2) / hm);
            tr[x][2] = (uint8_t)((y1 * 255 + hm / 2) / hm);
            tr[x][3] = ws_level(ws_cols[x]);
            prev     = y;
        }
        gpu_trace_set(2, (const uint8_t (*)[4])tr);
        g_dirty = true;
        return;
    }
    (void)rh;
    for (int i = 0; i < w * h; i++) px[i] = bg;
    for (int d = (int)ceilf(ws_bot / 10) * 10; d < ws_top; d += 10) {
        int y = th - 1 - (int)((d - ws_bot) / (ws_top - ws_bot) * (th - 1));

        if (y >= 0 && y < th)
            for (int x = 0; x < w; x += 3) px[y * w + x] = grid;
    }
    for (int x = 0; ws_count && x < w; x++) {
        int y = th - 1 - (int)((ws_cols[x] - ws_bot) / (ws_top - ws_bot) * (th - 1));

        y      = y < 0 ? 0 : y > th - 1 ? th - 1 : y;
        int      y0 = prev < 0 ? y : prev < y ? prev : y, y1 = prev < 0 ? y : prev > y ? prev : y;
        uint8_t  v    = ws_level(ws_cols[x]);
        uint32_t edge = 0xff000000 | d3_pal[v], body = scale_rgb(d3_pal[v / 2], 0.45f);

        for (int yy = y + 1; yy < th; yy++) px[yy * w + x] = yy == y + 1 ? scale_rgb(edge, 0.8f) : body;
        for (int yy = y0; yy <= y1; yy++) px[yy * w + x] = edge;
        prev = y;
    }
    for (int y = 2; y < th - 1; y++)     /* the dial */
        for (int dx = -8; dx <= 8; dx += 16) px[y * w + w / 2 + dx] = line;
    for (int dx = -8; dx <= 8; dx++)
        if (dx < -4 || dx > 4) px[2 * w + w / 2 + dx] = px[(th - 2) * w + w / 2 + dx] = line;
    for (int y = 2; y < th; y += 3) {    /* scan lines, as an old tube */
        uint32_t *r = px + y * w;
        for (int x = 0; x < w; x++) r[x] = 0xff000000 | ((r[x] >> 1) & 0x7f7f7f) | ((r[x] >> 2) & 0x3f3f3f);
    }
    if (ws_3d) {
        float cx = w / 2.0f;
        int   n  = ws_count < D3_ROWS ? ws_count : D3_ROWS;

        for (int r = n - 1; r >= 0; r--) {     /* the oldest, at the back, first */
            const uint8_t *lv     = ws_rows[(ws_head - 1 - r + WS_ROWS) % WS_ROWS];
            float          t      = (float)r / (D3_ROWS - 1);
            int            yb     = th + rh - 2 - (int)(t * rh * 0.6f);
            float          shrink = 1.0f - 0.36f * t, amp = rh * 0.62f * (1.0f - 0.3f * t), fade = 1.0f - 0.62f * t;
            int            x0 = (int)(cx - cx * shrink), x1 = (int)(cx + cx * shrink);

            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < w; x++) {
                int sx = (int)(cx + (x - cx) / shrink);

                if (sx < 0 || sx >= w) continue;
                uint8_t  v    = lv[sx];
                int      top  = yb - (int)(v * amp / 255);
                uint32_t edge = scale_rgb(d3_pal[v], fade), body = scale_rgb(d3_pal[v / 2], fade * 0.45f);

                if (top < th) top = th;
                px[top * w + x] = edge;
                if (top + 1 <= yb) px[(top + 1) * w + x] = scale_rgb(d3_pal[v], fade * 0.8f);
                for (int y = top + 2; y <= yb; y++) px[y * w + x] = body;
            }
        }
    } else {
        int rr = rh / WS_ROWS > 0 ? rh / WS_ROWS : 1;

        for (int r = 0; r < ws_count && th + (r + 1) * rr <= h; r++) {
            const uint8_t *lv = ws_rows[(ws_head - 1 - r + WS_ROWS) % WS_ROWS];

            for (int x = 0; x < w; x++) {
                uint32_t c = scale_rgb(d3_pal[lv[x]], 0.12f + 0.88f * lv[x] / 255.0f);

                for (int y = th + r * rr; y < th + (r + 1) * rr; y++) px[y * w + x] = c;
            }
        }
    }
    lv_obj_invalidate(u->ws_img);
}

/* A frame of the X6200's spectrum (the Sentry's own average) around the dial, 100 kHz: the trace each frame, a row
 * every second one (the stronger of the two), the rows moved aside when the dial moves */
static void ws_feed(void) {
    static float pend[PW], tmp[PW];
    static int   npend;
    wui_t       *u  = wui;
    int64_t      hz = w_hz();
    int          w;
    double       px_hz;

    if (!u->ws_frame) return;
    w     = u->ws_w;
    px_hz = (double)ws_span / w;
    if (w_sdr_hz) {                     /* on the SDR's channel: its rows */
        static float    sdb[8192];
        static uint32_t sseq;
        uint16_t        sn;
        uint32_t        srate;
        int64_t         sc;
        bool            sfresh;

        if (!rtl_spectrum_frame(sdb, N_OF(sdb), &sn, &srate, &sc, &sseq, &sfresh) || !sn || !srate) return;
        to_cols_n(sdb, sn, sc - srate / 2.0, (double)srate / sn, hz - ws_span / 2, px_hz, ws_cols, w);
    } else {
        if (!centre || !nbins || last_seq == ws_seen) return;
        ws_seen = last_seq;
        to_cols_n(avg_db, nbins, centre - span_hz / 2.0, bin_hz, hz - ws_span / 2, px_hz, ws_cols, w);
    }
    if (ws_at && ws_at != hz) {
        int dx = (int)lround((hz - ws_at) / px_hz);

        for (int r = 0; r < WS_ROWS; r++) {
            uint8_t *row = ws_rows[r];

            if (abs(dx) >= w) memset(row, 0, (size_t)w);
            else if (dx > 0) memmove(row, row + dx, (size_t)(w - dx)), memset(row + w - dx, 0, (size_t)dx);
            else if (dx < 0) memmove(row - dx, row, (size_t)(w + dx)), memset(row, 0, (size_t)-dx);
            if (g_gpu) gpu_levels_row(2, r, row);
        }
        npend = 0;
    }
    ws_at = hz;

    float top = -300;
    for (int x = 0; x < w; x++)
        if (ws_cols[x] > top) top = ws_cols[x];
    memcpy(tmp, ws_cols, (size_t)w * sizeof(float));
    float med = median(tmp, w);

    ws_floor = ws_floor < -250 ? med : ws_floor * 0.8f + med * 0.2f;
    float want_top = top + 4 > ws_floor + 30 ? top + 4 : ws_floor + 30, want_bot = ws_floor - 8;

    ws_top += (want_top - ws_top) * (want_top > ws_top ? 0.5f : 0.08f);
    ws_bot += (want_bot - ws_bot) * 0.1f;
    for (int x = 0; x < w; x++) pend[x] = npend ? fmaxf(pend[x], ws_cols[x]) : ws_cols[x];
    if (++npend >= 2) {
        npend = 0;
        for (int x = 0; x < w; x++) ws_rows[ws_head][x] = ws_level(pend[x]);
        if (g_gpu) gpu_levels_row(2, ws_head, ws_rows[ws_head]);
        ws_head = (ws_head + 1) % WS_ROWS;
        if (ws_count < WS_ROWS) ws_count++;
    }
    ws_draw();
}

/* G's two pictures by the GPU (G held up, Mesa there), into the display's plane under the GUI's holes: the
 * spectrum's trace (once it has rows), its rows in 3D or flat and the dial's bracket, as ws_draw; the voiceprint's
 * ring of columns, as vp_column made it. Both each time: the plane's two buffers take turns */
static void g_gpu_draw(void) {
    wui_t    *u  = &w_port;
    uint32_t  bg = argb(ink), line = argb(amb);
    lv_area_t a, v;

    if (!g_gpu || !g_portrait || wui != u || !u->ws_img || !u->vp_img || !gpu_frame_begin(true)) return;
    g_dirty = false;
    lv_obj_update_layout(u->ws_img);
    lv_obj_get_coords(u->ws_img, &a);
    lv_obj_get_coords(u->vp_img, &v);

    int w = u->ws_w, h = u->ws_h, th = WS_TRACE, rh = h - th, x = a.x1, y = a.y1, cx = x + w / 2;

    gpu_clear(x, y, w, h, bg);
    if (ws_count) gpu_trace(2, x, y, w, th, ws_bot, ws_top, argb(faint), bg);
    if (ws_3d) gpu_rows(2, x, y + th, w, rh, ws_head, ws_count, D3_ROWS);
    else gpu_flat(2, x, y + th, w, rh, ws_head, ws_count, rh / WS_ROWS > 0 ? rh / WS_ROWS : 1, bg);
    gpu_rect(cx - 8, y + 2, 1, th - 3, line);       /* the dial: its sides, their ends turned in */
    gpu_rect(cx + 8, y + 2, 1, th - 3, line);
    gpu_rect(cx - 8, y + 2, 4, 1, line);
    gpu_rect(cx + 5, y + 2, 4, 1, line);
    gpu_rect(cx - 8, y + th - 2, 4, 1, line);
    gpu_rect(cx + 5, y + th - 2, 4, 1, line);
    gpu_clear(v.x1, v.y1, u->vp_w, u->vp_h, vp_pal[0]);
    gpu_vp(3, v.x1, v.y1, u->vp_w, u->vp_h, vp_head);

    int x0 = LV_MIN(a.x1, v.x1), y0 = LV_MIN(a.y1, v.y1);

    gpu_frame_end(x0, y0, LV_MAX(a.x1 + w, v.x1 + u->vp_w) - x0, LV_MAX(a.y1 + h, v.y1 + u->vp_h) - y0);
}

/* F4 on G: the picture's rows in 3D or flat */
static void ws_view_next(void) {
    ws_3d   = !ws_3d;
    g_dirty = true;
    if (wui->ws_cap_l) tag_text(wui->ws_cap_l, ws_3d ? "100 KHZ  3D" : "100 KHZ  2D", wui->ws_cap_x, wui->ws_cap_y, false);
    ws_draw();
    buttons_refresh(&btn[3]);
}

static void show_walkie(void) {
    char           fr[40], nm[80], lk[40], tx[24], pw1[40], pw2[40], sy1[40], sy2[40], f[24];
    int64_t        hz = w_hz();
    int32_t        fq = (int32_t)hz;
    int            ch = -1, bi = band_of(hz);
    float          ext = 0, bat = 0;
    uint8_t        cap = 0;
    bool           chg = false;
    ident_t        id;
    static float   l_bat, l_ext;            /* the base's last word: its reports come and go between reads */
    static uint8_t l_cap;
    static bool    l_chg;

    if (!wui->num_l) return;
    w_plan();
    int  wi  = w_match();
    char via[64];
    static int said = -1;               /* what the keys said: told again when it changes */

    w_at   = wi;
    w_mode = w_mode_now();
    {
        int k = ((wi + 1) * W_MODES + w_mode) * 2 + (ws_3d ? 1 : 0);

        if (k != said) {
            said = k;
            for (int b = 0; b < 5; b++) buttons_refresh(&btn[b]);
        }
    }
    (void)ch;
    snprintf(f, sizeof(f), "%d.%03d.%02d", fq / 1000000, (fq / 1000) % 1000, (fq % 1000) / 10);
    rtl_dmr_t dm;
    rtl_dsd_t dd;
    bool      dmr = w_sdr_hz && rtl_spectrum_dmr(&dm) && dm.hz == w_sdr_hz;
    bool      dsd = !dmr && w_sdr_hz && rtl_spectrum_dsd(&dd) && dd.hz == w_sdr_hz;     /* P25, D-STAR, YSF... */

    snprintf(fr, sizeof(fr), "%s %s", f, dmr ? "DMR" : dsd ? rtl_dsd_name(dd.proto)
                                         : w_sdr_hz ? "FM" : mode_name(cparam_i_get(cfg_cur_mode)));
    identify_ex(fq, &id, false);
    snprintf(nm, sizeof(nm), "%s", id.name);
    if (dmr) {                          /* who talks, in the channel's name's place */
        int k = dm.slot[1].voice ? 1 : dm.slot[2].voice ? 2 : dm.slot[0].voice ? 0
              : dm.slot[1].lc ? 1 : dm.slot[2].lc ? 2 : 0;
        const rtl_dmr_slot_t *sl = &dm.slot[k];

        if (sl->lc)
            snprintf(nm, sizeof(nm), "%s%s %u FROM %u%s", k ? (k == 1 ? "TS1 " : "TS2 ") : "",
                     sl->flco == 3 ? "TO" : "TG", sl->dst, sl->src, sl->voice ? "" : " (ENDED)");
        else if (dm.cc >= 0)
            snprintf(nm, sizeof(nm), "DMR COLOUR CODE %d", dm.cc);
        else
            snprintf(nm, sizeof(nm), "DMR");
    } else if (dsd) {                   /* who talks, else the protocol's name */
        snprintf(nm, sizeof(nm), "%s", dd.call[0][0] ? dd.call[0] : dd.call[1][0] ? dd.call[1] : rtl_dsd_name(dd.proto));
    }
    snprintf(via, sizeof(via), "VIA %s", w_sdr_hz ? rtl_spectrum_dev_name() : "X6200");
    if (wi >= 0) {
        const wch_t *c = &w_chs[wi];
        bool         cb = !strcmp(c->grp, "CB");

        lv_label_set_text(wui->num_l, c->name);
        lv_label_set_text(wui->band_l, cb && !strcmp(c->name, "09") ? "CB EMERGENCY" : cb && !strcmp(c->name, "19")
                                       ? "CB TRUCKERS" : c->grp);
    } else {                            /* off the plan's channels: G's VFO, in its band */
        lv_label_set_text(wui->num_l, "VFO");
        lv_label_set_text(wui->band_l, bi >= 0 ? bands[bi].name : "OFF THE BANDS");
    }
    if (wui->via_l) lv_label_set_text(wui->via_l, via);
    snprintf(lk, sizeof(lk), "SQL %d  VOL %d", param_i_get(cfg_squelch), radio_change_vol(0));
    snprintf(tx, sizeof(tx), "%s", (w_sdr_hz ? g_sdr_armed : g_armed) ? "TX ARMED" : rx_held ? "TX LOCKED"
                                                                                           : "TX NOT LOCKED");
    char txp[24];                       /* what goes out: the X6200's watts, or the SDR's gain */

    if (w_sdr_hz) w_sdr_power(txp, sizeof(txp));
    else snprintf(txp, sizeof(txp), "TX %.0f W", param_f_get(cfg_pwr));
    clock_power_get(&ext, &bat, &cap, &chg);
    if (bat > 1 || ext > 1) l_bat = bat, l_ext = ext, l_cap = cap, l_chg = chg;
    if (l_bat > 1) snprintf(pw1, sizeof(pw1), "BAT %.1f V %d%%%s", l_bat, l_cap, l_chg ? " CHARGING" : "");
    else snprintf(pw1, sizeof(pw1), "NO BATTERY");
    snprintf(pw2, sizeof(pw2), "EXT %.1f V  %s", l_ext, txp);
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
        char info[256];                 /* three lines; the time: the bars'. Set when it is other text */

        snprintf(info, sizeof(info), "%s  %s\n%s  EXT %.1f V\n%s  %s  %s", lk, tx, pwc, l_ext, sy1, sy2, txp);
        lv_label_set_text(wui->info_l, info);
        lv_label_set_text(wui->ch.opt[0], nm);
        if (wui->status_l) {            /* the title bar's line, as the other screens' (show_head) */
            char        pre[96], line[224], tc[48] = "", dev[128] = "";
            const char *icon = "";
            int         cpu  = clock_cpu_temp();

            if (cpu != CLOCK_NO_TEMP) snprintf(tc, sizeof(tc), " %dC", cpu);
            if (l_bat > 1) {
                if (bc) snprintf(tc + strlen(tc), sizeof(tc) - strlen(tc), " #%06X %d%%#", (unsigned)bc, l_cap);
                else snprintf(tc + strlen(tc), sizeof(tc) - strlen(tc), " %d%%", l_cap);
                icon = batt_sym(l_ext, l_cap, l_chg);
            }
            dev_tags(dev, sizeof(dev));
            for (int compact = 0; compact < 2; compact++) {     /* too long for the bar: the time without its zone */
                char hs[16];

                strftime(hs, sizeof(hs), compact ? "%H:%M" : "%H:%M %Z", &tm);
                snprintf(pre, sizeof(pre), "%s  %s%s", (w_sdr_hz ? g_sdr_armed : g_armed) ? "TX ARMED"
                                                       : rx_held ? "RX ONLY" : "TX NOT LOCKED", hs, tc);
                snprintf(line, sizeof(line), "%s%s%s", pre, icon[0] ? "   " : "", dev);
                if (lv_txt_get_width(line, strlen(line), &pixel_16, 0, LV_TEXT_FLAG_RECOLOR) <= P_VX - 18) break;
            }
            lv_label_set_text(wui->status_l, line);
            lv_label_set_text(wui->batt_l, icon);
            lv_obj_set_style_text_color(wui->batt_l, bc ? lv_color_hex(bc) : amb, 0);
            if (icon[0]) {
                lv_coord_t w = lv_txt_get_width(line, strlen(line), &pixel_16, 0, LV_TEXT_FLAG_RECOLOR);
                lv_coord_t p = lv_txt_get_width(pre, strlen(pre), &pixel_16, 0, LV_TEXT_FLAG_RECOLOR);

                lv_obj_set_pos(wui->batt_l, 12 + (P_VX - 18 - w) / 2 + p + 6, 31);
            }
        }
        if (wui->vstatus_l) {           /* the arm down the right: every screen's line (show_head's), turned */
            char        pre[160], line[240];
            const char *icon;
            uint32_t    c;

            head_fit(pre, sizeof(pre), line, sizeof(line), &icon, &c, 680);
            lv_label_set_text(wui->vstatus_l, line);
            lv_label_set_text(wui->vbatt_l, icon);
            lv_obj_set_style_text_color(wui->vbatt_l, c ? lv_color_hex(c) : amb, 0);
            if (icon[0]) {              /* after the %, as show_head puts it: along the arm, down from its middle */
                lv_coord_t w  = lv_txt_get_width(line, strlen(line), &pixel_16, 0, LV_TEXT_FLAG_RECOLOR);
                lv_coord_t p  = lv_txt_get_width(pre, strlen(pre), &pixel_16, 0, LV_TEXT_FLAG_RECOLOR);
                lv_coord_t iw = lv_txt_get_width(icon, strlen(icon), &power_sym_14, 0, LV_TEXT_FLAG_NONE);
                lv_coord_t at = 400 - w / 2 + p + 6 + iw / 2;      /* the icon's middle along the arm */

                lv_obj_set_pos(wui->vbatt_l, P_VS - 10, at - 8);
            }
        }
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
static void vp_make(wui_t *u, lv_obj_t *p, int x, int y, int w, int h, bool direct) {
    rect(p, x - 4, y - 3, w + 8, h + 6, 3, false);
    u->vp_w     = w;
    u->vp_h     = h;
    u->vp_frame = lv_img_buf_alloc(w, h, LV_IMG_CF_TRUE_COLOR);
    if (direct) {       /* the GPU draws it on the display's plane (g_gpu_draw): a hole in the GUI over it */
        u->vp_img = lv_obj_create(p);
        lv_obj_remove_style_all(u->vp_img);
        lv_obj_set_size(u->vp_img, w, h);
        lv_obj_add_event_cb(u->vp_img, direct_hole_cb, LV_EVENT_DRAW_MAIN_END, NULL);
        gpu_slot(3, w, h);
    } else {
        u->vp_img = lv_img_create(p);
        lv_img_set_src(u->vp_img, u->vp_frame);
    }
    lv_obj_set_pos(u->vp_img, x, y);
    {
        uint32_t *px = (uint32_t *)u->vp_frame->data;
        for (int i = 0; i < w * h; i++) px[i] = vp_pal[0];
    }
    for (int k = 1; k <= 3; k++) {      /* 1, 2, 3 kHz */
        char t[8];

        snprintf(t, sizeof(t), "%dK", k);
        tag_make(p, dim, t, x + 3, y + h - k * h * 1000 / VP_HZ - 9, true);
    }
    u->cap_x = x + w - 3;
    u->cap_y = y + 3;
    u->cap_l = tag_make(p, amb, "VOICEPRINT  RX  0-4 KHZ", u->cap_x, u->cap_y, false);
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
    if (gpu_ok()) gpu_palette(1, vp_pal);
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
    vp_make(u, p, 176, my + 3, 612, vh, false);

    int by = my + vh + 12, bw = (SW - 12 - 6) / 2;

    bar_make(u, p, 6, by, bw, scr_h - by - 6, false);
    bar_make(u, p, 6 + bw + 6, by, bw, scr_h - by - 6, true);
}


/* The radio's keys and its main knob go to the screen shown: held up, G's. They go on to the main screen, where the
 * Sentry has them (its front_key, its rotary_cb), as the Walkie app sends them on. A knob's turn is the receiver's to
 * free */
static void g_fwd_cb(lv_event_t *e) {
    if (g_prev_scr && lv_obj_is_valid(g_prev_scr))
        lv_event_send(g_prev_scr, lv_event_get_code(e), lv_event_get_param(e));
    else if (lv_event_get_code(e) == EVENT_ROTARY)
        free(lv_event_get_param(e));
}

/* A label turned a quarter to the right about its own middle, that middle at (cx, cy): read with the radio lying as
 * for the other screens */
static lv_obj_t *label_turned(lv_obj_t *p, const lv_font_t *f, int cx, int cy, int w, int h, const char *s) {
    lv_obj_t *l = label(p, f, amb, cx - w / 2, cy - h / 2, w, LV_TEXT_ALIGN_CENTER, s);

    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(l, h);
    lv_obj_set_style_transform_pivot_x(l, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(l, h / 2, 0);
    lv_obj_set_style_transform_angle(l, 900, 0);
    return l;
}

/* A letter badge of the bar, the letter turned with the display (as every screen's), the circle at x, y */
static void badge_turned(int x, int y) {
    lv_obj_t *c = rect(g_scr, x, y, 36, 36, 3, false);
    lv_obj_t *l;

    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    l = label(g_scr, &pixel_28, amb, x, y + 3, 36, LV_TEXT_ALIGN_CENTER, "G");
    lv_obj_set_height(l, 28);
    lv_obj_set_style_transform_pivot_x(l, 18, 0);      /* about the circle's middle */
    lv_obj_set_style_transform_pivot_y(l, 15, 0);
    lv_obj_set_style_transform_angle(l, 900, 0);
}


static void build_walkie_portrait(void) {
    wui_t    *u = &w_port;
    lv_obj_t *o;

    g_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(g_scr);
    lv_obj_set_size(g_scr, PW, PH);
    lv_obj_set_style_bg_color(g_scr, ink, 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_scr, g_fwd_cb, EVENT_KEYPAD, NULL);
    lv_obj_add_event_cb(g_scr, g_fwd_cb, EVENT_ROTARY, NULL);
    /* the left column: the radio's own F buttons, moved here while G shows (walkie_show), as the Walkie app does; F1
     * under the bar */

    /* The title bar, an L to the screen's edges: down the right, where every screen has its bar at the display's
     * top (read with the radio lying, G at both ends as the others' letters), and across the top (read held up).
     * Its outline: the corner, the arm down (its left edge from the arm across's bottom edge) and the arm across */
    o = rect(g_scr, P_VX, 6, P_BAR, P_BAR, 3, false);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT, 0);
    o = rect(g_scr, P_VX, 6 + P_BAR - 3, P_BAR, PH - 6 - (6 + P_BAR - 3), 3, false);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM, 0);
    o = rect(g_scr, 6, 6, P_VX - 6, P_BAR, 3, false);
    lv_obj_set_style_border_side(o, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM, 0);
    /* across: the name and the walkie's own line (the transmitter's lock, the time, the processor, the battery, the
     * devices on USB), held up */
    char gt[96];
    title_text(gt, sizeof(gt), "WALKIE TALKIE", NULL, &pixel_20, P_VX - 18);
    label(g_scr, &pixel_20, amb, 12, 9, P_VX - 18, LV_TEXT_ALIGN_CENTER, gt);
    u->status_l = label(g_scr, &pixel_16, amb, 12, 30, P_VX - 18, LV_TEXT_ALIGN_CENTER, "");
    lv_label_set_long_mode(u->status_l, LV_LABEL_LONG_CLIP);
    lv_obj_set_height(u->status_l, 18);
    lv_label_set_recolor(u->status_l, true);
    u->batt_l = lv_label_create(g_scr);
    lv_obj_set_style_text_font(u->batt_l, &power_sym_14, 0);
    lv_obj_set_style_text_color(u->batt_l, amb, 0);
    lv_label_set_text(u->batt_l, "");
    /* down: as every screen's bar, turned with the display: its name, the title bar's line (show_head's), G at both
     * ends */
    title_text(gt, sizeof(gt), "WALKIE TALKIE", NULL, &pixel_20, 680);
    label_turned(g_scr, &pixel_20, P_VT, 400, 680, 20, gt);
    u->vstatus_l = label_turned(g_scr, &pixel_16, P_VS, 400, 680, 18, "");
    lv_label_set_recolor(u->vstatus_l, true);
    u->vbatt_l = label(g_scr, &power_sym_14, amb, P_VS - 10, 392, 20, LV_TEXT_ALIGN_CENTER, "");
    lv_obj_set_height(u->vbatt_l, 16);
    lv_obj_set_style_transform_pivot_x(u->vbatt_l, 10, 0);
    lv_obj_set_style_transform_pivot_y(u->vbatt_l, 8, 0);
    lv_obj_set_style_transform_angle(u->vbatt_l, 900, 0);
    badge_turned(PW - 43, 12);          /* the display's top left (held up, the top right): every screen's letter */
    badge_turned(PW - 43, PH - 50);     /* and the bar's other end */

    /* F1, F2 and F3 have no box (CH -/+ or STEP -/+ on F1 and F2, as the arrows): the channel, the spectrum and the
     * voiceprint run from the screen's edge, the spectrum and the voiceprint as tall as each other; F4's box, as
     * small as "3D", beside its key in the lamp's row; under it the radio's state, then F5's box and the messages */
    int ys = P_TOP + 92 + 6, yv, hh = (P_LAMPY - 6 - ys - 6) / 2;

    /* the channel: its number big, beside it the radio, the group and the frequency; under them the S-meter and the
     * audio as short bars in the spectrum's colours */
    rect(g_scr, P_FX, P_TOP, P_FW, 92, 3, false);
    u->num_l  = label(g_scr, &pixel_44, amb, P_FX + 4, P_TOP + 10, 120, LV_TEXT_ALIGN_CENTER, "--");
    u->via_l  = label(g_scr, &pixel_16, amb, P_FX + 130, P_TOP + 6, P_FW - 138, LV_TEXT_ALIGN_LEFT, "");
    lv_label_set_recolor(u->via_l, true);
    u->band_l = label(g_scr, &pixel_20, amb, P_FX + 130, P_TOP + 24, P_FW - 138, LV_TEXT_ALIGN_LEFT, "");
    u->freq_l = label(g_scr, &pixel_20, amb, P_FX + 130, P_TOP + 46, P_FW - 138, LV_TEXT_ALIGN_LEFT, "");
    u->info_l = NULL;
    {
        int half = (P_FW - 24) / 2;

        mbar_make(u, g_scr, P_FX + 8, P_TOP + 74, half, false);
        mbar_make(u, g_scr, P_FX + 16 + half, P_TOP + 74, half, true);
    }
    /* the X6200's spectrum around the dial (3D or 2D: F4), edge to edge */
    {
        int y = ys, h = hh;

        rect(g_scr, P_FX, y, P_FW, h, 3, false);
        u->ws_w     = P_FW - 6;
        u->ws_h     = h - 6;
        u->ws_frame = lv_img_buf_alloc(u->ws_w, u->ws_h, LV_IMG_CF_TRUE_COLOR);
        g_gpu       = rtl_gpu && gpu_slot(2, u->ws_w, WS_ROWS);    /* the GPU there: it draws G's two pictures */
        if (g_gpu) {
            u->ws_img = lv_obj_create(g_scr);
            lv_obj_remove_style_all(u->ws_img);
            lv_obj_set_size(u->ws_img, u->ws_w, u->ws_h);
            lv_obj_add_event_cb(u->ws_img, direct_hole_cb, LV_EVENT_DRAW_MAIN_END, NULL);
            for (int r = 0; r < WS_ROWS; r++) gpu_levels_row(2, r, ws_rows[r]);
        } else {
            u->ws_img = lv_img_create(g_scr);
            lv_img_set_src(u->ws_img, u->ws_frame);
        }
        lv_obj_set_pos(u->ws_img, P_FX + 3, y + 3);
        /* its captions framed, at the two ends of the trace, clear of the dial's bracket in the middle */
        tag_make(g_scr, amb, "SPECTRUM", P_FX + 6, y + 6, true);
        u->ws_cap_x = P_FX + P_FW - 6;
        u->ws_cap_y = y + 6;
        u->ws_cap_l = tag_make(g_scr, amb, ws_3d ? "100 KHZ  3D" : "100 KHZ  2D", u->ws_cap_x, u->ws_cap_y, false);
        ws_draw();
    }
    /* the voiceprint, edge to edge, as tall as the spectrum, down to the lamp's row */
    yv = ys + hh + 6;
    vp_make(u, g_scr, P_FX + 4, yv + 3, P_FW - 8, P_LAMPY - 6 - yv - 6, g_gpu);
    /* the lamp's row: F4's box (buttons_vertical_place) beside its key, the lamp */
    u->lamp = rect(g_scr, P_FX + P_VW + 6, P_LAMPY, P_FW - P_VW - 6, P_LAMP, 3, false);
    lv_obj_set_style_bg_color(u->lamp, amb, 0);
    u->lamp_l = label(g_scr, &pixel_28, amb, P_FX + P_VW + 6, P_LAMPY + (P_LAMP - 28) / 2, P_FW - P_VW - 6,
                      LV_TEXT_ALIGN_CENTER, "RX  QUIET");
    /* the radio's state, edge to edge */
    {
        int y = P_LAMPY + P_LAMP + 6;

        rect(g_scr, P_FX, y, P_FW, P_STATE, 3, false);
        u->info_l = label(g_scr, &pixel_20, amb, P_FX + 8, y + (P_STATE - 60) / 2, P_FW - 16, LV_TEXT_ALIGN_LEFT, "");
        lv_label_set_recolor(u->info_l, true);      /* the battery's line coloured as it runs down */
    }
    /* the bottom row: F5's box (buttons_vertical_place), beside it what is on the dial and a word for what was
     * refused (and what to press instead) */
    rect(g_scr, P_SX, P_BOTY, P_SW, PH - 6 - P_BOTY, 3, false);
    u->ch.opt[0] = label(g_scr, &pixel_16, dim, P_SX + 8, P_BOTY + 6, P_SW - 16, LV_TEXT_ALIGN_LEFT, "");     /* what */
    lv_label_set_long_mode(u->ch.opt[0], LV_LABEL_LONG_SCROLL_CIRCULAR);
    u->ch.n  = 1;
    u->msg_l = label(g_scr, &pixel_20, amb, P_SX + 8, P_BOTY + 26, P_SW - 16, LV_TEXT_ALIGN_LEFT, "");
    /* PTT refused: an arrow under the words, right into the bar's far G, the A/B key beyond it on the top panel */
    {
        static lv_point_t pts[5];

        u->ab_arrow = arrow_make(g_scr, pts, PW - 140, PH - 32, PW - 46, PH - 32);
        u->ab_tag   = tag_make(g_scr, amb, "A/B", PW - 146, PH - 41, false);
        lv_obj_add_flag(u->ab_tag, LV_OBJ_FLAG_HIDDEN);
    }
}

/* G shown (on) or left: held up in portrait where the build can, its own screen in place of the Sentry's */
static void walkie_show(bool on) {
    if (on && !g_portrait && g_scr && main_display_portrait(true)) {
        g_portrait = true;
        wui        = &w_port;
        g_prev_scr = lv_scr_act();
        {   /* the F keys reach only buttons on the screen shown; only F3 and F4 named (the others' keys work) */
            static const lv_coord_t ws[5] = {0, 0, 0, P_VW, P_KW};

            buttons_vertical_w(g_scr, ws, P_KEY, P_TOP);
            buttons_vertical_place(3, P_LAMPY, P_VW, P_KEYLEN);    /* F4: as long as its key, beside it */
            buttons_vertical_place(4, P_BOTY, P_KW, P_KEYLEN);     /* F5: the same */
        }
        for (int i = 0; i < 5; i++) buttons_refresh(&btn[i]);
        lv_scr_load(g_scr);
        g_dirty = true;                 /* the GPU's pictures drawn at the next tick (g_gpu_draw) */
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
    /* PTT refused: an arrow from the OSD up to the bar's right end, where the A/B key is on the top panel */
    {
        static lv_point_t pts[5];

        ab_arrow = arrow_make(root, pts, 690, 140, SW - 38, 52);
        ab_tag   = tag_make(root, amb, "A/B", 694, 112, false);
        lv_obj_add_flag(ab_tag, LV_OBJ_FLAG_HIDDEN);
    }
    osd_hide_cb(lv_timer_create(osd_hide_cb, 100000, NULL));

    sp_box = rect(root, 60, 100, SW - 120, 200, 3, false);
    lv_obj_add_flag(sp_box, LV_OBJ_FLAG_CLICKABLE);         /* a tap beside its rows is not the picture's */
    sp_head_l = label(sp_box, &pixel_20, amb, 12, 12, SW - 150, LV_TEXT_ALIGN_LEFT, "");
    for (int r = 0; r < BU_N; r++) {
        lv_obj_t *l = label(sp_box, &pixel_20, amb, 9, 50 + r * 28, SW - 144, LV_TEXT_ALIGN_LEFT, "");

        lv_obj_set_style_bg_color(l, amb, 0);
        lv_obj_set_style_pad_ver(l, 2, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(l, sp_row_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        sp_row_l[r] = l;
    }
    sp_foot_l = label(sp_box, &pixel_20, amb, 12, 0, SW - 150, LV_TEXT_ALIGN_LEFT,
                      "MFK: CHOOSE   PUSH OR A TAP: USE IT   BACK: AS IT IS\nF4 (LISTEN) GOES THROUGH THEM LATER");
    lv_obj_add_flag(sp_box, LV_OBJ_FLAG_HIDDEN);
    sp_on = false;

    pp_box = rect(root, 30, 56, SW - 60, 200, 3, false);
    lv_obj_add_flag(pp_box, LV_OBJ_FLAG_CLICKABLE);
    pp_head_l = label(pp_box, &pixel_20, amb, 12, 12, SW - 90, LV_TEXT_ALIGN_LEFT, "");
    for (int r = 0; r < SDR_POOL_UNITS; r++) {
        pp_row_l[r] = label(pp_box, &pixel_20, amb, 9, 50 + r * 28, SW - 84, LV_TEXT_ALIGN_LEFT, "");
        lv_obj_set_style_bg_color(pp_row_l[r], amb, 0);
        lv_obj_set_style_pad_ver(pp_row_l[r], 2, 0);
    }
    pp_foot_l = label(pp_box, &pixel_20, amb, 12, 0, SW - 90, LV_TEXT_ALIGN_LEFT,
                      "MFK: CHOOSE   F1/PUSH: USE ON B   F2: STOP/START   F3: HEAR/MUTE\nF4: WATCH B'S CHANNEL   "
                      "F5, BACK OR APP: CLOSE");
    lv_obj_add_flag(pp_box, LV_OBJ_FLAG_HIDDEN);
    pp_on = false;

    help_frame = rect(root, 14, 52, SW - 28, scr_h - 56, 3, false);
    lv_obj_add_flag(help_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(help_frame, help_tap_cb, LV_EVENT_CLICKED, NULL);
    help_l = label(root, &pixel_16, amb, 26, 60, SW - 52, LV_TEXT_ALIGN_LEFT,
                   "CONTROLS IN THE SENTRY                        ANY KEY OR A TAP CLOSES THIS\n"
                   "\n"
                   "VFO KNOB       A, G: THE X6200 (MAN-OVERRIDE); B: THE SDR'S CHANNEL, F2 HELD: PRESETS\n"
                   "MFK KNOB       CHOOSES A TARGET, A BAND, A FREQUENCY. PUSH: TRACK, LISTEN, OPEN\n"
                   "MFK RING       A X6200 B VHF/UHF C LOG D BANDS E DECODE F SYSTEM G WALKIE H AIRSPACE\n"
                   "A AND B        F1 SCAN  F2 BAND  F3 VIEW  F4 LISTEN  F5 SCAN CFG; MFK PANS (SPUN: 3 OR 8 STEPS A CLICK)\n"
                   "VOL KNOB       VOLUME, PUSH: SQUELCH, PUSH: RF GAIN (B: THE SDR'S); VOLUME AGAIN AFTER 5 S\n"
                   "LISTEN         A: X6200, THE SDR, BOTH, OFF; B: X6200, EACH SDR THAT TUNES IT, BOTH, OFF\n"
                   "F4 HELD ON B   + VFO: B'S SDRS (USED ON LETTING GO); ALONE: WATCH B'S CHANNEL ON A SPARE RTL\n"
                   "APP HELD       SDR POOL: EACH SDR, ITS USER; USE ON B, STOP/START, HEAR/MUTE (ANY SCREEN, H TOO)\n"
                   "BAND UP/DOWN   THE NEXT BAND, AS F2 (B: THE NEXT PRESET; G: A CHANNEL OR A STEP, MFK: GROUP)\n"
                   "AM  CW  SSB    THE MODE: AM BROADCAST AND CB, SSB HAM VOICE, CW MORSE\n"
                   "DFN            NOISE REDUCTION ON/OFF. HOLD: NOISE BLANKER (CLICKS AND POPS)\n"
                   "DFL            FILTER WIDE, NORMAL, NARROW (NARROW: LESS NOISE, MUFFLED)\n"
                   "FST            HOW FAR ONE CLICK OF THE VFO KNOB MOVES\n"
                   "ATU            ANTENNA TUNER IN/OUT. TUNING ITSELF TRANSMITS: NOT HERE\n"
                   "A/B            ON A, G: ARMS TX FOR PTT, KNOBS LOCKED (G'S HACKRF CHANNELS: ITS TX)\n"
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
#if USE_DRM
    drm_wf_claim(rtl_direct);           /* the plane is the Sentry's picture's while it is open (drm_wf_rotate) */
#endif
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
    sdr_here    = adsb_rx_sdr_present();    /* before A's LISTEN is put on the speaker */
    to_airspace = false;
    opened_ms   = lv_tick_get();
    g_onx = g_onr = -1;
    g_stick = false;
    if (g_sel >= nhits) g_sel = 0;
    nbins = 0;
    centre = anchor = 0;
    cursor_hz = 0;
    memset(&trk, 0, sizeof(trk));
    memset(visited, 0, sizeof(visited));
    n_ignored = logged = 0;
    b_nskip   = 0;                      /* B's skips too: forgotten as the Sentry opens, as A's */
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

    state_restore();                    /* the GUI restarted: the screen, the scans as they were */
    {
        scr_t s = open_at;

        open_at = SCR_SCAN;
        go(SCR_SCAN);
        if (s == SCR_TRACK && cfg_from == SCR_RTL) go(SCR_RTL);     /* B's SCAN CFG: from B, as its F5 */
        if (s != SCR_SCAN) go(s);       /* from A, as a turn of the ring would (G held up, B's picture) */
    }
    if (open_pool) {                    /* APP held on H: the pool's panel, over F */
        open_pool = false;
        pp_open();
    }
}

static void destruct_cb(void) {
#if USE_DRM
    drm_wf_claim(false);                /* the main screen's waterfall and spectrum on the plane again */
#endif
    if (timer) lv_timer_del(timer);
    if (blink_timer) lv_timer_del(blink_timer);
    if (rtl_timer) lv_timer_del(rtl_timer);
    timer = blink_timer = rtl_timer = NULL;
    f2_roll = f2_rolled = false;
    f4_roll = f4_rolled = false;
    f4_pick = -1;
    if (scr == SCR_LOG) g_leave(false);
    audio_scope(false);
    walkie_show(false);                 /* landscape again before anything else draws */
    walkie_arm(false);                  /* the transmitter locked (the hold let go below with the rest) */
    radio_set_vol_override(-1);         /* the speaker at the X6200's volume again */
    /* and the X6200's receiver on it: the stick's audio is the Sentry's (nothing outside it shows the stick's channel
     * or turns it), and OFF's silence would leave the classic radio and the apps mute. On AIRSPACE, the Sentry's H,
     * OFF stays silent (AIRSPACE lets it go when it closes) */
    rtl_spectrum_mux(false);
    rtl_spectrum_rds_watch(0);
    rtl_spectrum_listen(0, 0);
    rtl_spectrum_mute(to_airspace && heard == HEAR_OFF);
    to_airspace = false;
    for (int i = 0; i < 2; i++) {
        wui_t *u = i ? &w_port : &w_land;

        if (u->vp_frame) {
            lv_img_cache_invalidate_src(u->vp_frame);
            lv_img_buf_free(u->vp_frame);
        }
        if (u->ws_frame) {
            lv_img_cache_invalidate_src(u->ws_frame);
            lv_img_buf_free(u->ws_frame);
        }
        for (int k = 0; k < 2; k++) {
            lv_img_dsc_t *f = k ? u->am_frame : u->sm_frame;

            if (f) {
                lv_img_cache_invalidate_src(f);
                lv_img_buf_free(f);
            }
        }
        memset(u, 0, sizeof(*u));
    }
    for (int i = 0; i < 3; i++) {       /* F SYSTEM's load bars */
        if (ld_frame[i]) {
            lv_img_cache_invalidate_src(ld_frame[i]);
            lv_img_buf_free(ld_frame[i]);
        }
        ld_frame[i] = NULL;
        ld_img[i] = ld_val[i] = ld_more[i] = NULL;
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

/* ---- Across a restart of the GUI (gui_state.c) ---- */

/* A stick frequency kept: in Hz (key hz, 32 bits), or in kHz (key khz) past 2.1 GHz; 0 none, or outside what any SDR
 * here tunes (the device's own range is rtl_tune_to's: it may not have said yet) */
static int64_t state_hz(const char *hz, const char *khz) {
    int64_t v = gui_state_get(hz, 0);

    if (!v) v = (int64_t)gui_state_get(khz, 0) * 1000;
    return v >= RTL_SPEC_ANY_MIN_HZ && v <= RTL_SPEC_ANY_MAX_HZ ? v : 0;
}

#define SWEEP_KEYS ((BANDS_N + 29) / 30)

/* What the Sentry has going, kept every half second (blink_cb): its screen, A's and B's scans and C's, the bands
 * marked for A's sweep, the choices on each screen. Not an armed transmitter (G's A/B), a lock on a signal, help or
 * the OSD, nor what C was playing (its scan plays again). The dial, the volumes, LISTEN and the views are settings */
static void state_keep(void) {
    char k[16];

    gui_state_set("s.scr", scr);
    gui_state_set("s.cfgfrom", cfg_from);
    gui_state_set("s.mode", smode == MODE_AUTO ? MODE_AUTO : MODE_SEMI);   /* MAN-OVERRIDE: SEMI, the dial kept */
    gui_state_set("s.rout", routine);
    gui_state_set("s.aband", scan_a_band);
    gui_state_set("s.astep", a_step);
    gui_state_set("s.rscan", rtl_scan);
    gui_state_set("s.rtlhz", rtl_hz <= INT32_MAX ? (int32_t)rtl_hz : 0);
    gui_state_set("s.lhz", listen_hz <= INT32_MAX ? (int32_t)listen_hz : 0);
    gui_state_set("s.rtlk", (int32_t)(rtl_hz / 1000));         /* past 2.1 GHz too (a HackRF, a bladeRF) */
    gui_state_set("s.gsdr", (int32_t)(w_sdr_hz / 1000));       /* G on the SDR's channel, kHz; its TX gain (never */
    gui_state_set("s.sdrg", w_sdr_gain);                       /* its amplifier: off after a restart) */
    gui_state_set("s.lk", (int32_t)(listen_hz / 1000));
    gui_state_set("s.preset", rtl_preset);
    gui_state_set("s.fband", f_band);
    gui_state_set("s.dwell", scan_dwell_ms);
    gui_state_set("s.stay", scan_stay);         /* SCAN CFG's STAY ON A SIGNAL and GO ON AFTER (the style with them) */
    gui_state_set("s.quiet", scan_quiet);
    gui_state_set("s.gscan", g_scan);
    gui_state_set("s.gsel", g_sel);
    gui_state_set("s.bsel", band_sel);
    gui_state_set("s.tabs", 2);                 /* D's and E's tables as saved: 2 since WX SAT and E's new types */
    gui_state_set("s.dsel", dec_sel);
    gui_state_set("s.drow", dec_row);
    gui_state_set("s.sys", sys_sel);
    gui_state_set("s.3d", ws_3d);
    for (int j = 0; j < SWEEP_KEYS; j++) {
        int32_t m = 0;

        for (int i = j * 30; i < BANDS_N && i < j * 30 + 30; i++) m |= (int32_t)band_rt[i].sweep << (i - j * 30);
        snprintf(k, sizeof(k), "s.sweep%d", j);
        gui_state_set(k, m);
    }
}

/* At the first opening after a restart that was in the Sentry (construct_cb, after its defaults, before its screen
 * is shown): what state_keep kept, each checked against what it may be. The scans go on from where they were */
static void state_restore(void) {
    static bool done;
    uint32_t    now = lv_tick_get();
    int32_t     v;
    char        k[16];

    if (done) return;
    done = true;
    if (!gui_state_restoring()) return;
    if (gui_state_start_place() != GUI_PLACE_SENTRY || gui_state_get("s.scr", -1) < 0) {
        /* Back in an app or the classic screen: the Sentry opened later as ever, but B's channel as it was (the
         * stick's tuning is a setting, its channel was not: a restart from AIRSPACE lost it, 2026-11-15) */
        int64_t lv = state_hz("s.lhz", "s.lk");

        if (lv && llabs(lv - rtl_hz) <= rtl_view_hz() / 2) listen_hz = lv;
        return;
    }

    v       = gui_state_get("s.scr", SCR_SCAN);
    open_at = v >= 0 && v < SCR_N ? (scr_t)v : SCR_SCAN;
    cfg_from = gui_state_get("s.cfgfrom", SCR_SCAN) == SCR_RTL ? SCR_RTL : SCR_SCAN;
    v = gui_state_get("s.dwell", scan_dwell_ms);
    if (v == 1000 || v == 2000 || v == 4000 || v == 8000) scan_dwell_ms = v;
    v = gui_state_get("s.astep", a_step);
    if (v >= 0 && v < N_OF(a_steps)) a_step = v;
    for (int j = 0; j < SWEEP_KEYS; j++) {
        snprintf(k, sizeof(k), "s.sweep%d", j);
        if ((v = gui_state_get(k, -1)) == -1) continue;
        for (int i = j * 30; i < BANDS_N && i < j * 30 + 30; i++) band_rt[i].sweep = (v >> (i - j * 30)) & 1;
    }
    /* A state from before WX SAT went in after AIR, and E's types were put in order (2026-11-17): its band numbers
     * from WX SAT's on one up, its type by its name */
    bool old_tabs = gui_state_get("s.tabs", 1) < 2;
    int  wx       = 0;

    while (wx < BANDS_N - 1 && strcmp(bands[wx].name, "WX SAT")) wx++;
#define BAND_LOAD(v) ((v) >= 0 && old_tabs && (v) >= wx ? (v) + 1 : (v))
    v = BAND_LOAD(gui_state_get("s.bsel", band_sel));
    if (v >= 0 && v < BANDS_N) band_sel = v;
    v = gui_state_get("s.dsel", dec_sel);
    if (old_tabs && v >= 0 && v < 8) {
        static const char *const was[] = {"FT8", "FT4", "WSPR", "CW", "SSTV", "NAVTEX", "WEFAX", "JS8"};
        int                      k     = 0;

        while (k < DECODERS_N - 1 && strcmp(decoders[k].name, was[v])) k++;
        v = k;
    }
    if (v >= 0 && v < DECODERS_N) dec_sel = v;
    v = gui_state_get("s.drow", 0);
    dec_row = v >= 0 && v < decoders[dec_sel].n ? v : 0;
    v = gui_state_get("s.sys", sys_sel);
    if (v >= 0 && v < SYS_N) sys_sel = v;
    v = gui_state_get("s.gsel", g_sel);
    if (v >= 0 && v < nhits) g_sel = v;
    ws_3d = gui_state_get("s.3d", ws_3d) != 0;
    v = gui_state_get("s.gsdr", 0);     /* G back on the SDR's channel when it is there as G opens (walkie_enter) */
    if (v >= 144000 && v <= 1300000) w_sdr_resume = (int64_t)v * 1000;
    v = gui_state_get("s.sdrg", w_sdr_gain);
    if (v >= 0 && v <= 47) w_sdr_gain = v;

    /* B: its preset or C's band, its tuning, the channel heard (hear_apply, as the screen opens, puts it on) */
    v = BAND_LOAD(gui_state_get("s.fband", -1));
    f_band = v >= 0 && v < BANDS_N ? v : -1;
    v = gui_state_get("s.preset", rtl_preset);
    if (v >= 0 && v < RTL_PRESETS) rtl_preset = v;
    int64_t sv = state_hz("s.rtlhz", "s.rtlk");

    if (sv) rtl_tune_to(sv, false);
    sv = state_hz("s.lhz", "s.lk");
    if (sv && llabs(sv - rtl_hz) <= rtl_view_hz() / 2) listen_hz = sv;
    if (gui_state_get("s.rscan", 0)) {  /* on from the picture it was at, after a dwell there */
        rtl_scan  = true;
        scan_next = now + SCAN_DWELL_MS;
        scan_pick = now + SCAN_PICK_MS;
    }

    /* A: AUTO or SEMI, its band scan (on from the dial: the base kept it) or the sweep of the marked bands */
    /* the scans' style: STAY and GO ON AFTER as kept (STEP TIME is s.dwell above); a state of before them: AUTO as
     * SAMPLER (it stayed a while on each), SEMI as BROWSE. A's mode from the stay */
    v = gui_state_get("s.stay", -1);
    if (v >= 0 && v < STAY_N) scan_stay = v;
    else scan_stay = gui_state_get("s.mode", MODE_SEMI) == MODE_AUTO ? STAY_15S : STAY_NONE;
    v = gui_state_get("s.quiet", scan_quiet);
    if (v >= 0 && v < (int)N_OF(quiet_ms)) scan_quiet = v;
    smode = scan_stay == STAY_NONE ? MODE_SEMI : MODE_AUTO;
    if (smode == MODE_AUTO) sweep_next = 0;
    v = BAND_LOAD(gui_state_get("s.aband", -1));
    if (v >= 0 && v < BANDS_N) scan_a_band = v;
#undef BAND_LOAD
    switch (gui_state_get("s.rout", ROUT_BAND)) {
        case ROUT_SCAN:
            if (scan_a_band >= 0 && cfg_is_valid_hw_freq(bands[scan_a_band].listen_hz)) {
                routine     = ROUT_SCAN;
                anchor      = dial_centre();
                scan_a_next = now + scan_dwell_ms;
            }
            break;
        case ROUT_SWEEP:
            routine    = ROUT_SWEEP;
            sweep_i    = -1;
            sweep_next = 0;
            break;
        default:
            break;
    }
    g_scan = gui_state_get("s.gscan", 0) != 0;
    g_nx = g_nr = 0;
    fprintf(stderr, "gui state: the Sentry back on screen %d, A's routine %d (%s), B's scan %s, C's %s\n", open_at,
            routine, smode == MODE_AUTO ? "AUTO" : "SEMI", rtl_scan ? "on" : "off", g_scan ? "on" : "off");
}

/* The Sentry on one of its screens by letter ("A" to "G"; "H" is AIRSPACE, "I" the direction finder): their rings come
 * back so */
/* The SDR pool's panel from outside the Sentry (APP held on H, AIRSPACE): the Sentry opened on F with it */
void app_sentry_open_pool(void) {
    if (dialog_type_is_run(dialog_sentry)) {
        pp_open();
        return;
    }
    open_pool = true;
    open_at   = SCR_SYSTEM;
    main_screen_start_app(ACTION_APP_SENTRY);
}

void app_sentry_open_at(char letter) {
    static const scr_t by[] = {SCR_SCAN, SCR_RTL, SCR_LOG, SCR_BANDS, SCR_DECODE, SCR_SYSTEM, SCR_WALKIE};

    if (letter == 'H') {
        to_airspace = dialog_type_is_run(dialog_sentry);    /* the Sentry closing for its H */
        main_screen_start_app(ACTION_APP_ADSB);
        return;
    }
    if (letter == 'I') {                /* the direction finder (app_df.c), after H while a Kraken is on USB */
        to_airspace = dialog_type_is_run(dialog_sentry);    /* as for H: the Sentry's speaker left as it was */
        main_screen_start_app(ACTION_APP_DF);
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
