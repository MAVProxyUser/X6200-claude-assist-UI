/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  FT8/FT4 messages ft8_lib leaves out (its ftx_message_decode() unpacks 0.0, 0.5, 1, 2 and 4, hound.c 0.1): the
 *  contest exchanges and the WSPR-style messages, read field by field as WSJT-X's unpack77 reads them, with its
 *  tables of ARRL/RAC sections and of states and provinces (lib/77bit/packjt77.f90, WSJT-X 2.7.0, GPL v3, K1JT
 *  and the WSJT-X team). The 28-bit calls go through ft8_lib as the calls of a standard message, as in hound.c.
 *
 *  i3.n3  example                              bits
 *  0.3    WA9XYZ KA1ABC R 16A EMA              n28 n28 r1 n4 k3 s7   ARRL Field Day, 1-16 transmitters
 *  0.4    WA9XYZ KA1ABC R 32A EMA              n28 n28 r1 n4 k3 s7   ARRL Field Day, 17-32
 *  0.6    K1ABC FN42 37                        n28 g15 p5            WSPR type 1
 *         PJ4/K1ABC 37                         n28 x16 p5 1          WSPR type 2, prefix or suffix
 *         <PJ4/K1ABC> FN42AX                   h22 g25 010           WSPR type 3
 *  3      TU; W9XYZ K1ABC R 579 MA             t1 n28 n28 r1 r3 e13  ARRL RTTY Roundup, state/province or serial
 *  5      <PA3XYZ> <G4ABC/P> R 590003 IO91NP   h12 h22 r1 r3 s11 g25 EU VHF contest
 */

#include "contest_unpack.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(CONTEST_TEXT_LENGTH >= FTX_MAX_MESSAGE_LENGTH, "ftx_message_decode() writes FTX_MAX_MESSAGE_LENGTH");

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define NTOKENS 2063592u    /* n28 below it: DE, QRZ, CQ, CQ_nnn, CQ_abcd; then a 22-bit hash; then a standard call */
#define NZZZ    46656u      /* WSPR type 2: a prefix below it (36^3), a suffix from it */

/* csec (NSEC 86): 0.3 and 0.4's isec, from 1 */
static const char *const sections[] = {
    "AB",  "AK",  "AL",  "AR",  "AZ",  "BC",  "CO",  "CT",  "DE",  "EB",  "EMA", "ENY", "EPA", "EWA", "GA",
    "GH",  "IA",  "ID",  "IL",  "IN",  "KS",  "KY",  "LA",  "LAX", "NS",  "MB",  "MDC", "ME",  "MI",  "MN",
    "MO",  "MS",  "MT",  "NC",  "ND",  "NE",  "NFL", "NH",  "NL",  "NLI", "NM",  "NNJ", "NNY", "TER", "NTX",
    "NV",  "OH",  "OK",  "ONE", "ONN", "ONS", "OR",  "ORG", "PAC", "PR",  "QC",  "RI",  "SB",  "SC",  "SCV",
    "SD",  "SDG", "SF",  "SFL", "SJV", "SK",  "SNJ", "STX", "SV",  "TN",  "UT",  "VA",  "VI",  "VT",  "WCF",
    "WI",  "WMA", "WNY", "WPA", "WTX", "WV",  "WWA", "WY",  "DX",  "PE",  "NB",
};

/* cmult (NUSCAN 171): i3=3's nexch 8001-8171, these then X01-X99 */
static const char *const mults[] = {
    "AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "FL", "GA", "HI",  "ID", "IL", "IN", "IA", "KS", "KY", "LA",
    "ME", "MD", "MA", "MI", "MN", "MS", "MO", "MT", "NE", "NV", "NH",  "NJ", "NM", "NY", "NC", "ND", "OH", "OK",
    "OR", "PA", "RI", "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA",  "WV", "WI", "WY", "NB", "NS", "QC", "ON",
    "MB", "SK", "AB", "BC", "NWT", "NF", "LB", "NU", "YT", "PEI", "DC", "DR", "FR", "GD", "GR", "OV", "ZH", "ZL",
};
#define NUSCAN (ARRAY_LEN(mults) + 99)

static const char alnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static uint32_t get_bits(const uint8_t *p, int start, int len) {
    uint32_t v = 0;
    for (int i = start; i < start + len; i++) v = (v << 1) | ((p[i / 8] >> (7 - i % 8)) & 1u);
    return v;
}

static void put_bits(uint8_t *p, int start, int len, uint32_t v) {
    for (int i = start + len - 1; i >= start; i--, v >>= 1) {
        if (v & 1u) p[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    }
}

/* ft8_lib's "CQ nnn" and "CQ abcd" are WSJT-X's CQ_nnn and CQ_abcd outside a standard message; a call with a space
 * inside is none (unpack28's "QU1RK") */
static bool wsjtx_call(char *call) {
    if (!strncmp(call, "CQ ", 3)) call[2] = '_';
    return call[0] && !strchr(call, ' ');
}

/* Two 28-bit calls (14 bytes each) as ft8_lib unpacks those of a standard message: a standard call, DE, QRZ, CQ
 * with its modifier, a hash as "<CALL>" or "<...>". Checked first without the table, which ft8_lib then gives the
 * standard calls (as in a standard message) */
static bool calls28(ftx_callsign_hash_interface_t *hash_if, uint32_t n28a, uint32_t n28b, char *c1, char *c2) {
    ftx_message_t std = {0};
    char          extra[8];

    put_bits(std.payload, 0, 29, n28a << 1);    /* ipa 0 */
    put_bits(std.payload, 29, 29, n28b << 1);   /* ipb 0 */
    put_bits(std.payload, 58, 16, 32400 + 1);   /* ir 0, igrid4 MAXGRID4+1: no grid */
    put_bits(std.payload, 74, 3, 1);            /* i3 1 */
    if (ftx_message_decode_std(&std, NULL, c1, c2, extra) != FTX_MESSAGE_RC_OK || !wsjtx_call(c1) || !wsjtx_call(c2))
        return false;
    ftx_message_decode_std(&std, hash_if, c1, c2, extra);
    return wsjtx_call(c1) && wsjtx_call(c2);
}

/* A 12- or 22-bit hash (14 bytes): "<CALL>" for a call heard, "<...>" */
static void hashed(ftx_callsign_hash_interface_t *hash_if, ftx_callsign_hash_type_t type, uint32_t hash, char *call) {
    char heard[16] = "";

    if (hash_if && hash_if->lookup_hash(type, hash, heard) && heard[0])
        snprintf(call, 14, "<%.11s>", heard);
    else
        strcpy(call, "<...>");
}

/* save_hash_call: a call heard ("<CALL>" too), for "<CALL>" later */
static void save_call(ftx_callsign_hash_interface_t *hash_if, const char *call) {
    static const char c38[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
    char              c11[12];
    uint64_t          n58 = 0;
    size_t            n;

    if (call[0] == '<') call++;
    n = strcspn(call, ">");
    if (!hash_if || n < 3 || n > 11 || !strncmp(call, "...", 3)) return;
    memcpy(c11, call, n);
    c11[n] = '\0';
    for (size_t i = 0; i < 11; i++) {
        const char *c = i < n ? strchr(c38, c11[i]) : c38;   /* the call, then spaces */
        if (!c) return;
        n58 = 38 * n58 + (uint64_t)(c - c38);
    }
    hash_if->save_hash(c11, (uint32_t)((47055833459ull * n58) >> 42) & 0x3FFFFFu);   /* ihashcall(c, 22) */
}

/* The message, as unpack77 writes its character*37 msg: longer (two calls of 11 in brackets) is cut */
__attribute__((format(printf, 2, 3))) static void show(char *text, const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, CONTEST_TEXT_LENGTH, fmt, ap);
    va_end(ap);
}

/* nint(n*10/3.0): the 5-bit power, 0..103 dBm */
static unsigned dbm(uint32_t n) {
    return (n * 10 + 1) / 3;
}

/* ---- 0.3 and 0.4: "WA9XYZ KA1ABC R 16A EMA" ---- */

static bool field_day(ftx_callsign_hash_interface_t *hash_if, const uint8_t *p, unsigned n3, char *text) {
    uint32_t n28a = get_bits(p, 0, 28), n28b = get_bits(p, 28, 28), ir = get_bits(p, 56, 1);
    uint32_t intx = get_bits(p, 57, 4), nclass = get_bits(p, 61, 3), isec = get_bits(p, 64, 7);
    char     c1[14], c2[14];

    if (isec < 1 || isec > ARRAY_LEN(sections) || n28a <= 2 || n28b <= 2) return false; /* DE, QRZ, CQ: none */
    if (!calls28(hash_if, n28a, n28b, c1, c2)) return false;
    show(text, "%s %s %s%u%c %s", c1, c2, ir ? "R " : "", intx + 1 + (n3 == 4 ? 16 : 0), (char)('A' + nclass),
         sections[isec - 1]);
    return true;
}

/* ---- 0.6: "K1ABC FN42 37", "PJ4/K1ABC 37", "K1ABC/P 37", "<PJ4/K1ABC> FN42AX" ---- */

static bool wspr(ftx_callsign_hash_interface_t *hash_if, const uint8_t *p, char *text) {
    char call[24], c1[14], c2[14];

    if (get_bits(p, 49, 1)) { /* xx1, type 2: n28 x16 p5 */
        uint32_t n28 = get_bits(p, 0, 28), npfx = get_bits(p, 28, 16);
        unsigned pwr = dbm(get_bits(p, 44, 5));
        bool     prefix = npfx < NZZZ;
        char     fix[4] = "", *pfx = fix;

        if (prefix) {                   /* base 36, no leading 0 */
            pfx = fix + 3;
            do {
                *--pfx = alnum[npfx % 36];
                npfx /= 36;
            } while (npfx && pfx > fix);
        } else if ((npfx -= NZZZ) <= 35) {
            fix[0] = alnum[npfx];
            fix[1] = '\0';
        } else if (npfx <= 1295) {
            fix[0] = alnum[npfx / 36];
            fix[1] = alnum[npfx % 36];
            fix[2] = '\0';
        } else if (npfx <= 12959) {     /* a third character only a digit */
            fix[0] = alnum[npfx / 360];
            fix[1] = alnum[npfx / 10 % 36];
            fix[2] = alnum[npfx % 10];
        } else {
            return false;
        }
        if (pwr > 60 || !calls28(hash_if, n28, n28, c1, c2)) return false;
        snprintf(call, sizeof(call), "%s/%s", prefix ? pfx : c1, prefix ? c1 : fix);
        show(text, "%s %u", call, pwr);
        save_call(hash_if, call);
        return true;
    }
    if (!get_bits(p, 48, 1)) { /* x00, type 1: n28 g15 p5 */
        uint32_t n28 = get_bits(p, 0, 28), g = get_bits(p, 28, 15);
        unsigned pwr = dbm(get_bits(p, 43, 5));

        if (pwr > 60 || g >= 18 * 18 * 10 * 10 || !calls28(hash_if, n28, n28, c1, c2)) return false;
        show(text, "%s %c%c%c%c %u", c1, 'A' + g / 1800, 'A' + g / 100 % 18, '0' + g / 10 % 10, '0' + g % 10, pwr);
        save_call(hash_if, c1);
        return true;
    }
    if (!get_bits(p, 47, 1)) { /* 010, type 3: h22 g25, a grid's subsquare 24/24 (Y/Y) for none */
        uint32_t g = get_bits(p, 22, 25), s1 = g / 25 % 25, s2 = g % 25;
        char     sub[3] = "";

        if (g >= 18 * 18 * 10 * 10 * 25 * 25) return false;
        hashed(hash_if, FTX_CALLSIGN_HASH_22_BITS, get_bits(p, 0, 22), c1);
        if (s1 != 24 || s2 != 24) {
            sub[0] = (char)('A' + s1);
            sub[1] = (char)('A' + s2);
        }
        show(text, "%s %c%c%c%c%s", c1, 'A' + g / 1125000, 'A' + g / 62500 % 18, '0' + g / 6250 % 10,
             '0' + g / 625 % 10, sub);
        return true;
    }
    return false; /* 110 */
}

/* ---- i3=3: "TU; W9XYZ K1ABC R 579 MA", "W9XYZ G8ABC 559 0013" ---- */

static bool rtty_roundup(ftx_callsign_hash_interface_t *hash_if, const uint8_t *p, char *text) {
    uint32_t itu = get_bits(p, 0, 1), n28a = get_bits(p, 1, 28), n28b = get_bits(p, 29, 28);
    uint32_t ir = get_bits(p, 57, 1), irpt = get_bits(p, 58, 3), nexch = get_bits(p, 61, 13);
    char     c1[14], c2[14], exch[8];

    if (nexch > 8000 && nexch - 8000 <= NUSCAN) {
        if (nexch - 8000 <= ARRAY_LEN(mults))
            snprintf(exch, sizeof(exch), "%s", mults[nexch - 8001]);
        else
            snprintf(exch, sizeof(exch), "X%02u", (unsigned)(nexch - 8000 - ARRAY_LEN(mults)));
    } else if (nexch >= 1 && nexch <= 7999) {
        snprintf(exch, sizeof(exch), "%04u", (unsigned)nexch);
    } else {
        return false; /* 0, 8000, 8172-8191: WSJT-X shows an empty line */
    }
    if (!calls28(hash_if, n28a, n28b, c1, c2)) return false;
    show(text, "%s%s %s %s5%u9 %s", itu ? "TU; " : "", c1, c2, ir ? "R " : "", (unsigned)irpt + 2, exch);
    return true;
}

/* ---- i3=5: "<PA3XYZ> <G4ABC/P> R 590003 IO91NP" ---- */

static bool eu_vhf(ftx_callsign_hash_interface_t *hash_if, const uint8_t *p, char *text) {
    uint32_t n12 = get_bits(p, 0, 12), n22 = get_bits(p, 12, 22), ir = get_bits(p, 34, 1);
    uint32_t irpt = get_bits(p, 35, 3), iserial = get_bits(p, 38, 11), g = get_bits(p, 49, 25);
    char     c1[14], c2[14];

    if (g >= 18 * 18 * 10 * 10 * 24 * 24) return false;
    hashed(hash_if, FTX_CALLSIGN_HASH_12_BITS, n12, c1);
    hashed(hash_if, FTX_CALLSIGN_HASH_22_BITS, n22, c2);
    show(text, "%s %s %s%u%04u %c%c%c%c%c%c", c1, c2, ir ? "R " : "", (unsigned)(52 + irpt), (unsigned)iserial,
         'A' + g / 1036800, 'A' + g / 57600 % 18, '0' + g / 5760 % 10, '0' + g / 576 % 10, 'A' + g / 24 % 24,
         'A' + g % 24);
    return true;
}

bool contest_unpack(const ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, char *text) {
    unsigned i3 = ftx_message_get_i3(msg), n3 = ftx_message_get_n3(msg);
    char     buf[CONTEST_TEXT_LENGTH] = "";
    bool     ok;

    if (i3 == 0 && (n3 == 3 || n3 == 4))
        ok = field_day(hash_if, msg->payload, n3, buf);
    else if (i3 == 0 && n3 == 6)
        ok = wspr(hash_if, msg->payload, buf);
    else if (i3 == 3)
        ok = rtty_roundup(hash_if, msg->payload, buf);
    else if (i3 == 5)
        ok = eu_vhf(hash_if, msg->payload, buf);
    else
        return false; /* 0.2 and 0.7 are no message to WSJT-X, nor i3 6 and 7; the rest is ft8_lib's or hound.c's */
    if (!ok || !strncmp(buf, "CQ <", 4)) return false;
    strcpy(text, buf);
    return true;
}
