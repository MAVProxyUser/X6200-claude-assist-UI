/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the CI-V CAT's modes (src/cat.cpp), the real file and the real settings (src/cfg) on a database made
 *  from sql/params.sql, CI-V requests given to its process_req as the UART's frames would be. The cases:
 *  - 06 (set mode) to USB, CW, AM, FM, LSB: the mode taken, OK;
 *  - 06 with a mode this radio has none of (RTTY 04, RTTY-R 08, PSK 12, DV 17): NG, the mode as it was. Up to
 *    v0.3.12.48 the radio went to whatever its unset r_mode held, clamped into its modes (a -Wall build, 2026-11-25);
 *  - 26 (selected VFO's mode, the data byte too) to USB-D: taken, OK; to RTTY: NG, the mode as it was;
 *  - 04 (read mode) answers the mode set.
 *    cat_modes_test <params.sql>
 */

#include <pthread.h>
#define pthread_setname_np(...) 0     /* glibc's two-argument form, cat_thread's alone, not run here */
#include "cat.cpp"

#include <sqlite3.h>

#include <fstream>
#include <sstream>

static sqlite3 *db;
static int      fails, checks;

static void eq(const char *what, long got, long want) {
    checks++;
    if (got != want) {
        fails++;
        printf("FAIL %s: %ld, want %ld\n", what, got, want);
    }
}

/* A request from the computer (E0) to the radio (A4), its answer: the code (FB OK, FA NG) or the command */
static Frame *ask(uint8_t cmd, std::vector<uint8_t> data) {
    Frame req(LOCAL_ADDRESS, 0xE0, cmd);

    req.data = data;
    return process_req(&req);
}

static long code(Frame *r) {
    long c = r->command;

    delete r;
    return c;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: cat_modes_test <params.sql>\n");
        return 2;
    }
    std::ifstream     f(argv[1]);
    std::stringstream sql;

    sql << f.rdbuf();
    if (!f.good() || sqlite3_open(":memory:", &db) != SQLITE_OK) {
        printf("params.sql or the database: not opened\n");
        return 2;
    }
    char *err = nullptr;
    if (sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, &err) != SQLITE_OK || !cfg_db_init(db)) {
        printf("the database: %s\n", err ? err : "cfg_db_init failed");
        return 2;
    }
    cfg_sm.init_load(nullptr);

    struct { uint8_t ci; radio_mode_t x; const char *name; } known[] = {
        {M_USB, radio_mode_usb, "USB"}, {M_CW, radio_mode_cw, "CW"}, {M_AM, radio_mode_am, "AM"},
        {M_NFM, radio_mode_nfm, "FM"}, {M_CWR, radio_mode_cwr, "CW-R"}, {M_LSB, radio_mode_lsb, "LSB"},
    };
    for (auto &k : known) {
        char what[64];
        snprintf(what, sizeof(what), "06 %02X (%s): the answer", k.ci, k.name);
        eq(what, code(ask(C_SET_MODE, {k.ci})), CODE_OK);
        snprintf(what, sizeof(what), "06 %02X (%s): the mode", k.ci, k.name);
        eq(what, cfg_sm.cp_cur_mode.get(), k.x);
    }

    /* Modes of other Icoms this radio has none of: refused, the mode left as it was (LSB, the last above) */
    for (uint8_t m : {0x04, 0x08, 0x12, 0x17, 0x22, 0xFF}) {
        char what[64];
        snprintf(what, sizeof(what), "06 %02X (none here): the answer", m);
        eq(what, code(ask(C_SET_MODE, {m})), CODE_NG);
        snprintf(what, sizeof(what), "06 %02X (none here): the mode kept", m);
        eq(what, cfg_sm.cp_cur_mode.get(), radio_mode_lsb);
        snprintf(what, sizeof(what), "06 %02X 01 (none here, a filter byte): the answer", m);
        eq(what, code(ask(C_SET_MODE, {m, 0x01})), CODE_NG);
        eq("... the mode kept", cfg_sm.cp_cur_mode.get(), radio_mode_lsb);
    }

    /* 26 00: the selected VFO's mode, with the data byte (Hamlib's for USB-D) */
    eq("26 00 01 01 (USB, data): the answer", code(ask(C_SEND_SEL_MODE, {0x00, M_USB, 0x01})), CODE_OK);
    eq("26 00 01 01 (USB, data): the mode", cfg_sm.cp_cur_mode.get(), radio_mode_usb_dig);
    eq("26 00 04 00 (RTTY): the answer", code(ask(C_SEND_SEL_MODE, {0x00, 0x04, 0x00})), CODE_NG);
    eq("26 00 04 00 (RTTY): the mode kept", cfg_sm.cp_cur_mode.get(), radio_mode_usb_dig);
    eq("26 00 04 (RTTY, no data byte): the answer", code(ask(C_SEND_SEL_MODE, {0x00, 0x04})), CODE_NG);
    eq("... the mode kept", cfg_sm.cp_cur_mode.get(), radio_mode_usb_dig);

    /* 04: the mode read back as set */
    eq("06 03 (CW)", code(ask(C_SET_MODE, {M_CW})), CODE_OK);
    Frame *r = ask(C_RD_MODE, {});
    eq("04: CW read back", r->data.size() >= 1 ? r->data[0] : -1, M_CW);
    delete r;

    printf("cat_modes: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
