/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test: GMRS on the radio's own receiver and transmitter, through the real settings (a database made from
 *  sql/params.sql, as cfg_api_init loads it on the radio) and the real cfg_tx_refusal, for each model:
 *  - as the radio comes: its own receiver reaches no GMRS channel (the X6100 stops at 55 MHz, the X6200 at 137), so
 *    F's GMRS panel and G offer it only through an SDR, and nothing is sent there (the hardware does not);
 *  - the X6200 with the WR12 on: every channel and repeater input received (w_gmrs_x62), none sent (receive only);
 *  - a transverter whose range takes GMRS in (either model): received, and the hardware would send there, yet every
 *    channel and input is refused with GMRS's own words, with TX outside ham bands on, and with each TX ranges
 *    profile; a 70 cm transverter's 436 MHz still sends, and nothing on the edges outside GMRS's spans is called GMRS.
 *    gmrs_tx_x6100 / gmrs_tx_x6200 <params.sql>
 */

#include "cfg/cfg_api.h"
#include "cfg/gmrs.h"
#include "cfg/ham_bands.h"
#include "cfg/settings_manager.h"
#include "cfg/settings_manager_api.h"
#include "cfg/tx_ranges.h"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

static int fails, checks;

static void check(bool ok, const char *what, long v) {
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL %s: %ld\n", what, v);
    }
}

/* Every channel and repeater input, and each one's edges (half its bandwidth either side) */
static std::vector<int32_t> gmrs_freqs() {
    std::vector<int32_t> v;

    for (int ch = 1; ch <= GMRS_CHANNELS; ch++) {
        const gmrs_ch_t *c = gmrs_ch(ch);

        for (int32_t f : {c->hz, c->in_hz}) {
            if (!f) continue;
            v.push_back(f);
            v.push_back(f - c->bw_hz / 2);
            v.push_back(f + c->bw_hz / 2);
        }
    }
    return v;
}

/* The radio's own receiver reaches all of them (rx) or none (!rx); the hardware would send on all or none (tx) */
static void reach(const std::vector<int32_t> &fs, bool rx, bool tx, const char *what) {
    for (int32_t f : fs) {
        check(cfg_is_valid_hw_freq(f) == rx, what, f);
        check(cfg_is_tx_freq(f) == tx, what, f);
    }
}

/* None of them sent on: refused for why, never HAM_TX_OK; with GMRS's words where why is HAM_TX_GMRS */
static void refused(const std::vector<int32_t> &fs, ham_tx_t why, const char *what) {
    for (int32_t f : fs) {
        const char *no = cfg_tx_refusal(f);

        check(no != nullptr, what, f);
        check(cfg_tx_check(f) == why, what, f);
        if (why == HAM_TX_GMRS) check(no && strstr(no, "GMRS") != nullptr, "the refusal names GMRS", f);
    }
}

static void transverter(bool on) {
    cfg_sm.p_transverter_1_from.set(on ? 462500000 : 432000000);
    cfg_sm.p_transverter_1_to.set(on ? 467800000 : 438000000);
    cfg_sm.p_transverter_1_shift.set(on ? 434000000 : 0);    /* 462.5-467.8 MHz on a 28.5-33.8 MHz IF */
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: gmrs_tx_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;
    sqlite3          *db  = nullptr;
    char             *err = nullptr;

    sql << f.rdbuf();
    if (!f.good() || sqlite3_open(":memory:", &db) != SQLITE_OK ||
        sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK || !cfg_db_init(db)) {
        printf("params.sql into a database failed: %s\n", err ? err : "?");
        return 2;
    }
    cfg_api_init(nullptr);

    std::vector<int32_t> fs = gmrs_freqs();

    check(fs.size() == (GMRS_CHANNELS + 8) * 3, "every channel, input and edge", (long)fs.size());
    check(cfg_sm.p_tx_outside_bands.get() == 0 && cfg_sm.p_tx_ranges.get() == TX_RANGES_BANDS, "as the radio comes",
          0);

    /* As the radio comes: its own receiver reaches none of GMRS, and nothing is sent there */
    reach(fs, false, false, "the radio alone: GMRS out of its reach");
    refused(fs, HAM_TX_RX_ONLY, "the radio alone: nothing sent");

#ifdef BOARD_X6200
    /* The WR12 on HOST: received, never sent */
    cfg_sm.p_wr12.set(1);
    reach(fs, true, false, "the WR12: GMRS received, the hardware sends nothing there");
    refused(fs, HAM_TX_RX_ONLY, "the WR12: nothing sent");
    cfg_sm.p_tx_outside_bands.set(1);
    refused(fs, HAM_TX_RX_ONLY, "the WR12, TX outside ham bands on: nothing sent");
    cfg_sm.p_tx_outside_bands.set(0);
    cfg_sm.p_wr12.set(0);
    reach(fs, false, false, "the WR12 off again: out of reach");
#endif

    /* A transverter taking GMRS in: received, and the hardware would send, so GMRS's own refusal decides */
    transverter(true);
    check(cfg_transverter_shift_for(462675000) == 434000000, "the transverter's shift there", 0);
    reach(fs, true, true, "a GMRS transverter: received, the hardware would send");
    refused(fs, HAM_TX_GMRS, "a GMRS transverter: refused as GMRS");
    check(!strcmp(cfg_tx_refusal(462675000), tx_ranges_why(HAM_TX_GMRS)), "the refusal's words", 0);
    cfg_sm.p_tx_outside_bands.set(1);
    refused(fs, HAM_TX_GMRS, "a GMRS transverter, TX outside ham bands on: still refused as GMRS");
    cfg_sm.p_tx_outside_bands.set(0);
    tx_ranges_own_t st = tx_ranges_own_set("28.000-34.000\n");    /* the transverter's whole IF in the own list */

    check(st.count == 1 && st.bad == 0, "own list read", st.count);
    cfg_sm.p_tx_ranges.set(TX_RANGES_OWN);
    for (int32_t g : fs) check(cfg_tx_check(g) != HAM_TX_OK, "a GMRS transverter, the own list: refused", g);
    cfg_sm.p_tx_ranges.set(TX_RANGES_KR);
    for (int32_t g : fs) check(cfg_tx_check(g) != HAM_TX_OK, "a GMRS transverter, South Korea: refused", g);
    cfg_sm.p_tx_ranges.set(TX_RANGES_BANDS);
    tx_ranges_own_set("");

    /* Just outside GMRS's spans, on the same transverter: not GMRS (the transverter's own rule, as before) */
    for (int32_t g : {462537500 - 1, 467737500 + 1, 465000000})
        check(cfg_tx_check(g) == HAM_TX_OK, "the transverter outside GMRS's spans: as before", g);
    transverter(false);
    reach(fs, false, false, "the transverter off: out of reach again");

    /* A 70 cm transverter (432-438 MHz, the default range) still sends: GMRS's rule takes nothing else */
    cfg_sm.p_transverter_1_shift.set(404000000);
    check(cfg_tx_refusal(436000000) == nullptr, "a 70 cm transverter's 436 MHz sends", 436000000);
    cfg_sm.p_transverter_1_shift.set(0);

    printf("gmrs_tx (%s): %d checks, %d failed\n",
#ifdef BOARD_X6200
           "X6200",
#else
           "X6100",
#endif
           checks, fails);
    return fails != 0;
}
