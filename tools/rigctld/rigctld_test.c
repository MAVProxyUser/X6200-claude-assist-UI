/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of src/rigctld.c, the network's Hamlib NET rigctl (#178), against a pretend radio: what Hamlib's client
 *  needs to hear to open it (\chk_vfo, \dump_state as Hamlib's own rigctld lays it out, \get_lock_mode with its RPRT,
 *  else Hamlib waits 20 s before each set mode), the reads and sets, and every refusal: PTT, Listen only, LOCK, an
 *  app holding the radio, the radio on the air, a frequency it does not tune, a mode it has not. `make` builds it and
 *  runs it
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- The pretend radio ---- */

struct ParamInt {
    int32_t v;
};
struct ComputedParamInt {
    int32_t v;
};
static ParamInt         p_vol = {47}, p_rfg = {39}, p_sql = {0}, p_vfo = {0};
static ComputedParamInt c_freq = {7880000}, c_mode = {radio_mode_usb}, c_lo = {100}, c_hi = {3000}, c_pre = {1},
                        c_att = {0};
ParamInt         *cfg_volume = &p_vol, *cfg_rfgain = &p_rfg, *cfg_squelch = &p_sql, *cfg_band_current_vfo = &p_vfo;
ComputedParamInt *cfg_fg_freq = &c_freq, *cfg_cur_mode = &c_mode, *cfg_cur_filter_low = &c_lo,
                 *cfg_cur_filter_high = &c_hi, *cfg_cur_pre = &c_pre, *cfg_cur_att = &c_att;

int32_t param_i_get(const ParamInt *p) { return p->v; }
void    param_i_set(ParamInt *p, int32_t v) { p->v = v; }
int32_t cparam_i_get(const ComputedParamInt *p) { return p->v; }
void    cparam_i_set(ComputedParamInt *p, int32_t v) { p->v = v; }

static bool        on_air, freq_lock, mode_lock, trust = true, sam_wfm = true;
static const char *app;
static int16_t     raw_db = -100;

bool          cfg_is_valid_hw_freq(int32_t f) { return f >= 100000 && f <= 56000000; }
radio_state_t radio_get_state(void) { return on_air ? RADIO_TX : RADIO_RX; }
uint32_t      radio_caps(void) { return sam_wfm ? HW_CAP_SAM_WFM : 0; }
radio_vfo_t   radio_toggle_vfo(void) {
    p_vfo.v = !p_vfo.v;
    return (radio_vfo_t)p_vfo.v;
}
bool        main_screen_freq_locked(void) { return freq_lock; }
bool        main_screen_mode_locked(void) { return mode_lock; }
void        main_screen_set_freq(uint64_t f) { c_freq.v = (int32_t)f; }
const char *main_screen_digital_app(void) { return app; }
bool        backlight_tick(void) { return true; }
int16_t     meter_get_raw_db(void) { return raw_db; }
bool        tci_server_get_enabled(void) { return true; }
bool        tci_server_get_trust_operate(void) { return trust; }

/* The LVGL thread at once: rigctld.c waits for what it put there, and here it is done before the wait */
bool scheduler_put(scheduler_fn_t fn, void *arg, size_t n) {
    (void)n;
    fn(arg);
    return true;
}

/* ---- The test ---- */

static int  cases, wrong;
static char out[4096];

static const char *ask(const char *line) {
    if (!rigctld_answer(line, out, sizeof(out))) snprintf(out, sizeof(out), "<quit>");
    return out;
}

static void is(const char *line, const char *want, const char *what) {
    const char *got = ask(line);

    cases++;
    if (strcmp(got, want)) {
        wrong++;
        printf("WRONG  %s: \"%s\" gave \"%s\", not \"%s\"\n", what, line, got, want);
    }
}

static void ok(bool cond, const char *what) {
    cases++;
    if (!cond) {
        wrong++;
        printf("WRONG  %s\n", what);
    }
}

int main(void) {
    /* opening, as Hamlib 4.7.2's NET rigctl does it */
    is("\\get_powerstat", "1\n", "powered");
    is("\\chk_vfo", "0\n", "no VFO mode");
    ask("\\dump_state");
    ok(!strncmp(out, "1\n2\n2\n", 6), "dump_state: protocol 1, model 2, region 2 first");
    ok(strstr(out, "100000.000000 56000000.000000 0x") != NULL, "dump_state: HF 0.1-56 MHz");
    ok(strstr(out, "87500000.000000 137000000.000000 0x61 ") != NULL, "dump_state: the X6200's 87.5-137 MHz, AM FM WFM");
    {
        const char *rx_end = strstr(out, "0 0 0 0 0 0 0\n");
        ok(rx_end && !strncmp(rx_end + 14, "0 0 0 0 0 0 0\n", 14), "dump_state: no TX ranges after the RX ones");
    }
    ok(strstr(out, "0x4000003b\n") != NULL, "dump_state: the levels it reads (PREAMP ATT AF RF SQL STRENGTH)");
    ok(strstr(out, "rig_model=2\n") && strlen(out) > 5 && !strcmp(out + strlen(out) - 5, "done\n"), "dump_state: ends in done");
    sam_wfm = false;
    ask("\\dump_state");
    ok(!strstr(out, "87500000.000000"), "dump_state: an X6100 has no 87.5-137 MHz");
    sam_wfm = true;
    is("\\get_lock_mode", "0\nRPRT 0\n", "get_lock_mode with its RPRT (else Hamlib waits 20 s and sends no mode)");
    is("\\set_lock_mode 0", "RPRT 0\n", "set_lock_mode");

    /* reads */
    is("f", "7880000\n", "get_freq");
    is("\\get_freq", "7880000\n", "get_freq, long");
    is("m", "USB\n2900\n", "get_mode: USB, its passband");
    is("v", "VFOA\n", "get_vfo");
    is("t", "0\n", "get_ptt");
    is("s", "0\nVFOA\n", "get_split_vfo");
    is("l STRENGTH", "-27\n", "STRENGTH: -100 dBm below 30 MHz is 27 dB under S9");
    is("l AF", "0.470000\n", "AF");
    is("l RF", "0.390000\n", "RF");
    is("l SQL", "0.000000\n", "SQL");
    is("l PREAMP", "10\n", "PREAMP on");
    is("l ATT", "0\n", "ATT off");
    is("l NOTCHF", "RPRT -11\n", "a level it has not");

    /* sets */
    is("F 7074000", "RPRT 0\n", "set_freq");
    ok(c_freq.v == 7074000, "set_freq made");
    is("F 7074000.000000", "RPRT 0\n", "set_freq as Hamlib writes it");
    is("M PKTUSB 3000", "RPRT 0\n", "set_mode PKTUSB 3000");
    ok(c_mode.v == radio_mode_usb_dig && c_hi.v - c_lo.v == 3000, "PKTUSB is USB-D, 3000 Hz from the low edge");
    is("M CW 500", "RPRT 0\n", "set_mode CW 500");
    ok(c_mode.v == radio_mode_cw && c_hi.v - c_lo.v == 500, "CW 500 Hz about the passband's middle");
    is("M AM 6000", "RPRT 0\n", "set_mode AM 6000");
    ok(c_mode.v == radio_mode_am && c_hi.v == 3000, "AM: the high edge half the passband");
    is("m", "AM\n6000\n", "get_mode AM: twice the high edge");
    is("M WFM 0", "RPRT 0\n", "WFM on the X6200");
    sam_wfm = false;
    is("M SAM 0", "RPRT -1\n", "no SAM on an X6100");
    sam_wfm = true;
    is("M XYZ 0", "RPRT -1\n", "not a mode");
    is("V VFOB", "RPRT 0\n", "set_vfo B");
    is("v", "VFOB\n", "VFO B");
    is("V VFOA", "RPRT 0\n", "set_vfo A");
    is("V VFOQ", "RPRT -1\n", "not a VFO");
    is("F 99", "RPRT -9\n", "a frequency the radio does not tune");
    is("F abc", "RPRT -1\n", "not a number");

    /* refusals: nothing changes */
    c_freq.v = 7880000;
    is("T 1", "RPRT -9\n", "PTT refused: receive only");
    is("T 0", "RPRT 0\n", "PTT off is no harm");
    is("S 1 VFOB", "RPRT -9\n", "split refused");
    trust = false;
    is("F 7074000", "RPRT -9\n", "Listen only: no set_freq");
    is("M LSB 2400", "RPRT -9\n", "Listen only: no set_mode");
    is("f", "7880000\n", "Listen only: still read");
    trust = true;
    freq_lock = true;
    is("F 7074000", "RPRT -9\n", "LOCK: no set_freq");
    freq_lock = false;
    app = "FT8";
    is("F 7074000", "RPRT -9\n", "an app holds the radio");
    app = NULL;
    on_air = true;
    is("F 7074000", "RPRT -9\n", "on the air: no set_freq");
    is("t", "1\n", "on the air: get_ptt");
    on_air = false;
    ok(c_freq.v == 7880000, "after the refusals: the frequency as it was");
    mode_lock = true;
    c_mode.v  = radio_mode_usb;
    is("M LSB 0", "RPRT -9\n", "the mode held");
    is("M USB 0", "RPRT 0\n", "the mode held: the one it is in is fine");
    mode_lock = false;

    /* the rest */
    c_freq.v = 145500000;
    is("l STRENGTH", "-7\n", "STRENGTH above 30 MHz: S9 is -93 dBm");
    is("_", "GoldCorpHF X6200\n", "get_info");
    is("\\get_rig_info", "RPRT -11\n", "what it does not know");
    is("", "", "an empty line: no answer");
    is("q", "<quit>", "quit");

    printf("rigctld: %d of %d cases as they should be\n", cases - wrong, cases);
    return wrong != 0;
}
