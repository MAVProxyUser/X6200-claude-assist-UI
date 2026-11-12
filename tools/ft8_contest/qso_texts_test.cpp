/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the contest messages (src/ft8/contest_unpack.c); see the Makefile.
 *
 *  What the QSO sequence (src/ft8/qso.cpp add_rx_text, as ft8_window.c calls it) makes of the texts
 *  contest_unpack() gives: none ends in std::stoi (no word is a signed report), none is taken for a call, a grid
 *  or a report, and nothing goes out for them in Auto. The CQ forms are CQs, as before. The Fox's multi-stream
 *  message still is one.
 */

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include "qso.h"

static int  cases, bad;
static bool verbose;

struct result_t {
    ftx_msg_meta_t meta;
    std::string    tx;
    bool           threw;
};

static result_t rx(const char *me, const char *text) {
    FTxQsoProcessor *p = ftx_qso_processor_init(me, "FN42", nullptr, 6);
    ftx_tx_msg_t     tx = {};
    result_t         r = {};

    ftx_qso_processor_set_auto(p, true);
    ftx_qso_processor_set_local_standard(p, true);
    try {
        ftx_qso_processor_add_rx_text(p, text, -10, &r.meta, &tx);
    } catch (const std::exception &e) {
        r.threw = true;
    }
    r.tx = tx.msg;
    ftx_qso_processor_delete(p);
    return r;
}

static const char *type_name(ftx_msg_type_t t) {
    switch (t) {
        case FTX_MSG_TYPE_CQ: return "CQ";
        case FTX_MSG_TYPE_GRID: return "GRID";
        case FTX_MSG_TYPE_REPORT: return "REPORT";
        case FTX_MSG_TYPE_R_REPORT: return "R_REPORT";
        case FTX_MSG_TYPE_RR73: return "RR73";
        case FTX_MSG_TYPE_73: return "73";
        default: return "OTHER";
    }
}

/* want: the type; call_de: what it says sent it, "" for none; tx: what goes out in Auto, "" for nothing */
static void expect(const char *me, const char *text, ftx_msg_type_t want, const char *call_de, const char *tx) {
    result_t r  = rx(me, text);
    bool     ok = !r.threw && r.meta.type == want && !strcmp(r.meta.call_de, call_de) && r.tx == tx;

    cases++;
    bad += !ok;
    if (!ok || verbose)
        printf("%s %-8s %-40s %-8s call_de \"%s\" grid \"%s\"%s%s%s\n", ok ? "  ok " : "  BAD", me, text,
               r.threw ? "THREW" : type_name(r.meta.type), r.meta.call_de, r.meta.grid, r.tx.empty() ? "" : " next: ",
               r.tx.c_str(), r.meta.to_me ? " (to me)" : "");
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    printf("ARRL RTTY Roundup (i3=3)\n");
    const char *ft4[][2] = {{"KR9A", "AC6BW KR9A R 559 WI"},   {"K1JT", "K1JT WB4HXE 559 GA"},
                            {"KA1YQC", "KB0VHA KA1YQC R 539 MA"}, {"N1TRK", "N1TRK N4FKH 569 VA"},
                            {"W7DRW", "NI6G W7DRW 569 AZ"},     {"NZ7P", "NZ7P WA7JAY 589 CA"},
                            {"VE3LON", "VE3LON K7RL R 549 WA"}};
    for (auto &c : ft4) {
        expect(c[0], c[1], FXT_MSG_TYPE_OTHER, "", "");
        expect("Q1QQQ", c[1], FXT_MSG_TYPE_OTHER, "", "");
    }
    for (const char *me : {"Q1QQQ", "W9XYZ", "K1ABC"}) {
        expect(me, "TU; W9XYZ K1ABC R 579 MA", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "TU; W9XYZ K1ABC 579 MA", FXT_MSG_TYPE_OTHER, "", "");   /* qso.cpp's ';': K1ABC, 579, MA */
        expect(me, "TU; W9XYZ K1ABC 599 NB", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "TU; W9XYZ G8ABC 559 0013", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "W9XYZ G8ABC R 559 0013", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "K1ABC W9XYZ 529 X99", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "CQ_RU W9XYZ R 559 AL", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "TU; CQ <...> 599 MA", FXT_MSG_TYPE_OTHER, "", "");
    }
    expect("Q1QQQ", "CQ W9XYZ 599 MA", FTX_MSG_TYPE_CQ, "W9XYZ", "");   /* a CQ, its "grid" 599 */

    printf("ARRL Field Day (0.3, 0.4)\n");
    for (const char *me : {"Q1QQQ", "WA9XYZ", "KA1ABC"}) {
        expect(me, "WA9XYZ KA1ABC R 16A EMA", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "WA9XYZ KA1ABC 3A EMA", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "WA9XYZ KA1ABC R 3A EMA", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "WA9XYZ KA1ABC 32H DX", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "CQ_FD KA1ABC 1A EMA", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "<...> KA1ABC R 2H DX", FXT_MSG_TYPE_OTHER, "", "");
    }

    printf("EU VHF contest (i3=5)\n");
    for (const char *me : {"Q1QQQ", "PA3XYZ", "G4ABC/P"}) {
        expect(me, "<PA3XYZ> <G4ABC/P> R 590003 IO91NP", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "<PA3XYZ> <G4ABC/P> 590003 IO91NP", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "<...> <...> R 590003 RR99XX", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "<DL1ABC/P> <PA3XYZ/P> R 590003 JO31AB", FXT_MSG_TYPE_OTHER, "", "");
    }

    printf("WSPR-style (0.6)\n");
    for (const char *me : {"Q1QQQ", "K1ABC"}) {
        expect(me, "K1ABC FN42 37", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "PJ4/K1ABC 37", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "K1ABC/P 37", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "<PJ4/K1ABC> FN42AX", FXT_MSG_TYPE_OTHER, "", "");
        expect(me, "<...> FN42", FXT_MSG_TYPE_OTHER, "", "");
    }
    expect("Q1QQQ", "CQ AA00 0", FTX_MSG_TYPE_CQ, "AA00", "");
    /* Our call hashed then a word: the form of a type 4 answer to our CQ ("<ON6URE> PA/W1ABC"), so the grid is
     * taken for the caller, as a type 4 "<K1ABC> FN42AX" is today. WSJT-X sends 0.6 type 3 only in FST4W */
    expect("K1ABC", "<K1ABC> FN42AX", FTX_MSG_TYPE_GRID, "FN42AX", "FN42AX K1ABC -10");

    printf("The Fox's multi-stream message (0.1), as before\n");
    expect("K1ABC", "K1ABC RR73; W9XYZ <KH1/KH7Z> -12", FTX_MSG_TYPE_RR73, "KH1/KH7Z", "");
    expect("W9XYZ", "K1ABC RR73; W9XYZ <KH1/KH7Z> -12", FTX_MSG_TYPE_REPORT, "KH1/KH7Z", "KH1/KH7Z W9XYZ R-10");
    expect("Q1QQQ", "K1ABC RR73; W9XYZ <KH1/KH7Z> -12", FTX_MSG_TYPE_REPORT, "KH1/KH7Z", "");

    printf("%d cases, %d bad\n", cases, bad);
    return bad != 0;
}
