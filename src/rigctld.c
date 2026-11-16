/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the network's rigctld (rigctld.h). Its answers are those of Hamlib's own rigctld 4.7.2 where they
 *  differ from what the protocol seems to say, found against Hamlib's NET rigctl client (rigctl -m 2): "\dump_state"
 *  as Hamlib's (protocol 1, then key=value lines and "done"); "\get_lock_mode" answered with its value and RPRT 0,
 *  without which Hamlib waits 20 s before each set mode and then leaves it unsent.
 *
 *  What it sets goes as the TCI server's does (tci_server.c apply_cmd): on the LVGL thread, not under LOCK, a mode not
 *  held by an app, a frequency the radio tunes; never while the radio transmits or a digital app holds the radio
 */

#include "rigctld.h"

#include "board.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "scheduler.h"
#include "screen_light.h"
#include "screen_main.h"
#include "smeter.h"
#include "tci_server.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define WAIT_MS 1500        /* the LVGL thread's answer to a set, at most */

/* Hamlib's modes, in the radio's order (radio_mode_t), and their bits in \dump_state */
static const struct {
    const char *name;
    unsigned    bit;
} modes[] = {
    {"LSB", 0x8},     {"PKTLSB", 0x400}, {"USB", 0x4}, {"PKTUSB", 0x800}, {"CW", 0x2},
    {"CWR", 0x80},    {"AM", 0x1},       {"FM", 0x20}, {"SAM", 0x10000},  {"WFM", 0x40},
};
#define HF_MODES  (0x8 | 0x400 | 0x4 | 0x800 | 0x2 | 0x80 | 0x1 | 0x20)
#define SAM_WFM   (0x10000 | 0x40)
#define VHF_MODES (0x1 | 0x20 | 0x40)
#define LEVELS    (0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x40000000)    /* PREAMP ATT AF RF SQL STRENGTH */

bool rigctld_network_on(void) {
    return tci_server_get_enabled();
}

static int n_modes(void) {
    return radio_caps() & HW_CAP_SAM_WFM ? 10 : 8;
}

static int mode_of(const char *name) {
    for (int i = 0; i < n_modes(); i++)
        if (!strcasecmp(name, modes[i].name))
            return i;
    return -1;
}

/* The passband as Hamlib has it: SSB, data and CW the two edges apart; AM, FM, SAM and WFM twice the high edge, the
 * low one 0 there (tci_server.c) */
static int width_of(int mode) {
    int lo = cparam_i_get(cfg_cur_filter_low), hi = cparam_i_get(cfg_cur_filter_high);

    return mode >= radio_mode_am ? 2 * hi : hi - lo;
}

/* ---- Sets, on the LVGL thread ---- */

typedef enum { SET_FREQ, SET_MODE, SET_VFO } set_what_t;
typedef struct {
    set_what_t what;
    int32_t    a, b;
    unsigned   id;
} set_t;

static atomic_uint set_done;
static atomic_int  set_result;

/* Why the radio is not to be changed now, NULL it may be */
static const char *refused(void) {
    if (radio_get_state() == RADIO_TX) return "on the air";
    if (main_screen_digital_app()) return "an app holds the radio";
    return NULL;
}

static void set_cb(void *arg) {
    const set_t *s = arg;
    int          r = 0;

    if (refused()) {
        r = -9;                                     /* RIG_ERJCTED */
    } else if (s->what == SET_FREQ) {
        if (main_screen_freq_locked() || !cfg_is_valid_hw_freq(s->a)) {
            r = -9;
        } else {
            backlight_tick();                       /* tuned from a program: the screen wakes as for a key */
            if (s->a != cparam_i_get(cfg_fg_freq)) main_screen_set_freq((uint64_t)s->a);
            r = cparam_i_get(cfg_fg_freq) == s->a ? 0 : -9;
        }
    } else if (s->what == SET_MODE) {
        if (main_screen_mode_locked()) {
            r = s->a == cparam_i_get(cfg_cur_mode) ? 0 : -9;
        } else {
            if (s->a != cparam_i_get(cfg_cur_mode)) cparam_i_set(cfg_cur_mode, s->a);
            /* a passband: SSB and data the low edge kept, the high one moved to make it; CW about its middle */
            if (s->b > 0 && (s->a == radio_mode_cw || s->a == radio_mode_cwr)) {
                int mid = (cparam_i_get(cfg_cur_filter_low) + cparam_i_get(cfg_cur_filter_high)) / 2;
                int lo  = mid - s->b / 2 > 0 ? mid - s->b / 2 : 0;
                cparam_i_set(cfg_cur_filter_low, lo);
                cparam_i_set(cfg_cur_filter_high, lo + s->b);
            } else if (s->b > 0 && s->a < radio_mode_am) {
                int lo = cparam_i_get(cfg_cur_filter_low);
                cparam_i_set(cfg_cur_filter_high, lo + s->b);
            } else if (s->b > 0) {
                cparam_i_set(cfg_cur_filter_high, s->b / 2);
            }
        }
    } else {                                        /* SET_VFO: A 0, B 1 */
        if (param_i_get(cfg_band_current_vfo) != s->a) radio_toggle_vfo();
        r = param_i_get(cfg_band_current_vfo) == s->a ? 0 : -9;
    }
    atomic_store(&set_result, r);
    atomic_store(&set_done, s->id);
}

/* Hamlib's code for the set: 0 done */
static int set(set_what_t what, int32_t a, int32_t b) {
    static atomic_uint last;
    set_t              s = {.what = what, .a = a, .b = b, .id = atomic_fetch_add(&last, 1) + 1};

    if (!tci_server_get_trust_operate()) return -9;         /* Listen only */
    if (!scheduler_put(set_cb, &s, sizeof(s))) return -9;
    for (int ms = 0; atomic_load(&set_done) != s.id; ms += 5) {
        if (ms >= WAIT_MS) return -5;                       /* RIG_ETIMEOUT: the LVGL thread busy */
        usleep(5000);
    }
    return atomic_load(&set_result);
}

/* ---- Answers ---- */

static size_t dump_state(char *r, size_t n) {
    bool   sw = radio_caps() & HW_CAP_SAM_WFM;     /* the X6200: SAM, WFM, 87.5-137 MHz */
    size_t k  = 0;
    char   ver[48];

    snprintf(ver, sizeof(ver), "GoldCorpHF %s", sw ? "X6200" : "X6100");
#define P(...) k += (size_t)snprintf(r + k, k < n ? n - k : 0, __VA_ARGS__)
    P("1\n2\n2\n");                                 /* protocol, the model (NET rigctl), ITU region */
    P("100000.000000 56000000.000000 0x%x -1 -1 0x3 0x1\n", HF_MODES | (sw ? 0x10000 : 0));
    if (sw) P("87500000.000000 137000000.000000 0x%x -1 -1 0x3 0x1\n", VHF_MODES);
    P("0 0 0 0 0 0 0\n");
    P("0 0 0 0 0 0 0\n");                           /* no TX ranges: this rigctld does not transmit */
    P("0x%x 10\n0x%x 100\n0x%x 1000\n0 0\n", HF_MODES | (sw ? SAM_WFM : 0), HF_MODES | (sw ? SAM_WFM : 0),
      HF_MODES | (sw ? SAM_WFM : 0));
    P("0xc 2700\n0xc 2400\n0xc 1800\n0xc00 3000\n0x82 500\n0x82 250\n0x82 1000\n0x10001 6000\n0x10001 9000\n"
      "0x20 12000\n");
    if (sw) P("0x40 230000\n");
    P("0 0\n");
    P("0\n0\n0\n0\n");                              /* max RIT, XIT, IF shift; announces */
    P("10 \n10 \n");                                /* the preamp, the attenuator, dB */
    P("0x0\n0x0\n0x%x\n0x0\n0x0\n0x0\n", LEVELS);   /* funcs get/set, levels get/set, parms get/set */
    P("vfo_ops=0x0\nptt_type=0x0\ntargetable_vfo=0x0\nhas_set_vfo=1\nhas_get_vfo=1\nhas_set_freq=1\nhas_get_freq=1\n"
      "timeout=0\nrig_model=2\nrigctld_version=%s rigctld\ndone\n", ver);
#undef P
    return k;
}

/* The S-meter as Hamlib's STRENGTH: dB over S9 (-73 dBm below 30 MHz, -93 dBm above) */
static int strength(void) {
    int db = meter_get_raw_db();

    return db - (cparam_i_get(cfg_fg_freq) < 30000000 ? -73 : -93);
}

bool rigctld_answer(const char *line, char *reply, size_t size) {
    char  w[4][32] = {{0}};
    int   n        = sscanf(line, "%31s %31s %31s %31s", w[0], w[1], w[2], w[3]);
    const char *c  = w[0];

    reply[0] = 0;
    if (n < 1) return true;
#define IS(s, l) (!strcmp(c, s) || !strcmp(c, l))
    if (IS("q", "\\quit") || !strcmp(c, "Q") || !strcmp(c, "exit")) return false;
    if (!strcmp(c, "\\chk_vfo")) {
        snprintf(reply, size, "0\n");
    } else if (!strcmp(c, "\\dump_state")) {
        dump_state(reply, size);
    } else if (IS("f", "\\get_freq")) {
        snprintf(reply, size, "%d\n", (int)cparam_i_get(cfg_fg_freq));
    } else if (IS("F", "\\set_freq")) {
        double hz = n >= 2 ? strtod(w[1], NULL) : 0;
        snprintf(reply, size, "RPRT %d\n", hz >= 1 && hz < 2147483647.0 ? set(SET_FREQ, (int32_t)llround(hz), 0) : -1);
    } else if (IS("m", "\\get_mode")) {
        int m = cparam_i_get(cfg_cur_mode);
        snprintf(reply, size, "%s\n%d\n", m >= 0 && m < 10 ? modes[m].name : "USB", width_of(m));
    } else if (IS("M", "\\set_mode")) {
        int m = n >= 2 ? mode_of(w[1]) : -1, pb = n >= 3 ? atoi(w[2]) : -1;
        snprintf(reply, size, "RPRT %d\n", m < 0 ? -1 : set(SET_MODE, m, pb));   /* RIG_EINVAL: not a mode here */
    } else if (IS("v", "\\get_vfo")) {
        snprintf(reply, size, "%s\n", param_i_get(cfg_band_current_vfo) ? "VFOB" : "VFOA");
    } else if (IS("V", "\\set_vfo")) {
        int v = n < 2 ? -1 : !strcasecmp(w[1], "VFOA") || !strcasecmp(w[1], "currVFO") || !strcasecmp(w[1], "Main") ? 0
                         : !strcasecmp(w[1], "VFOB") || !strcasecmp(w[1], "Sub") ? 1 : -1;
        snprintf(reply, size, "RPRT %d\n", v < 0 ? -1 : set(SET_VFO, v, 0));
    } else if (IS("t", "\\get_ptt")) {
        snprintf(reply, size, "%d\n", radio_get_state() == RADIO_TX ? 1 : 0);
    } else if (IS("T", "\\set_ptt")) {
        snprintf(reply, size, "RPRT %d\n", n >= 2 && atoi(w[1]) == 0 ? 0 : -9);     /* receive only */
    } else if (IS("s", "\\get_split_vfo")) {
        snprintf(reply, size, "0\nVFOA\n");
    } else if (IS("S", "\\set_split_vfo")) {
        snprintf(reply, size, "RPRT %d\n", n >= 2 && atoi(w[1]) == 0 ? 0 : -9);
    } else if (IS("l", "\\get_level")) {
        if (n < 2) snprintf(reply, size, "RPRT -1\n");
        else if (!strcasecmp(w[1], "STRENGTH")) snprintf(reply, size, "%d\n", strength());
        else if (!strcasecmp(w[1], "AF")) snprintf(reply, size, "%.6f\n", param_i_get(cfg_volume) / (double)BOARD_VOL_MAX);
        else if (!strcasecmp(w[1], "RF")) snprintf(reply, size, "%.6f\n", param_i_get(cfg_rfgain) / (double)BOARD_RFG_MAX);
        else if (!strcasecmp(w[1], "SQL")) snprintf(reply, size, "%.6f\n", param_i_get(cfg_squelch) / (double)BOARD_SQL_MAX);
        else if (!strcasecmp(w[1], "PREAMP")) snprintf(reply, size, "%d\n", cparam_i_get(cfg_cur_pre) ? 10 : 0);
        else if (!strcasecmp(w[1], "ATT")) snprintf(reply, size, "%d\n", cparam_i_get(cfg_cur_att) ? 10 : 0);
        else snprintf(reply, size, "RPRT -11\n");
    } else if (!strcmp(c, "\\get_powerstat")) {
        snprintf(reply, size, "1\n");
    } else if (!strcmp(c, "\\get_lock_mode")) {
        snprintf(reply, size, "0\nRPRT 0\n");      /* with its RPRT, as Hamlib's own: else a 20 s wait */
    } else if (!strcmp(c, "\\set_lock_mode")) {
        snprintf(reply, size, "RPRT 0\n");
    } else if (IS("_", "\\get_info")) {
        snprintf(reply, size, "GoldCorpHF %s\n", radio_caps() & HW_CAP_SAM_WFM ? "X6200" : "X6100");
    } else {
        snprintf(reply, size, "RPRT -11\n");       /* RIG_ENAVAIL */
    }
#undef IS
    return true;
}
