/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  OpenX6X00 - host test of the contest messages (src/ft8/contest_unpack.c); see the Makefile.
 *
 *  Each message is packed here as WSJT-X packs it (lib/77bit/packjt77.f90: pack28, ihashcall, pack77_03,
 *  pack77_06, pack77_3, pack77_5, written again from the Fortran, not from contest_unpack.c), unpacked by
 *  contest_unpack() and compared with the text WSJT-X shows, put together here the way unpack77 puts it
 *  together. The tables are WSJT-X's data statements, verbatim, read here. Hashed calls are looked up in a
 *  table here (ft8_lib's interface; its own hashtable.c at 73b0db86 clears a quarter of itself at init, see
 *  security review R-05), filled the way the decoder fills it: ft8_lib's encoder hashes the calls.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft8lib/message.h>

#include "contest_unpack.h"

#define NTOKENS 2063592u
#define MAX22   4194304u

static int  cases, bad;
static bool verbose;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    if (ok && !verbose) return;
    bad += !ok;
    printf("%s ", ok ? "  ok " : "  BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ---- WSJT-X's tables, as packjt77.f90 (2.7.0) has them in unpack77 ---------------------------------------- */

static const char *csec_data =
    "       \"AB \",\"AK \",\"AL \",\"AR \",\"AZ \",\"BC \",\"CO \",\"CT \",\"DE \",\"EB \",  &       \n"
    "       \"EMA\",\"ENY\",\"EPA\",\"EWA\",\"GA \",\"GH \",\"IA \",\"ID \",\"IL \",\"IN \",  &\n"
    "       \"KS \",\"KY \",\"LA \",\"LAX\",\"NS \",\"MB \",\"MDC\",\"ME \",\"MI \",\"MN \",  &\n"
    "       \"MO \",\"MS \",\"MT \",\"NC \",\"ND \",\"NE \",\"NFL\",\"NH \",\"NL \",\"NLI\",  &       \n"
    "       \"NM \",\"NNJ\",\"NNY\",\"TER\",\"NTX\",\"NV \",\"OH \",\"OK \",\"ONE\",\"ONN\",  &\n"
    "       \"ONS\",\"OR \",\"ORG\",\"PAC\",\"PR \",\"QC \",\"RI \",\"SB \",\"SC \",\"SCV\",  &       \n"
    "       \"SD \",\"SDG\",\"SF \",\"SFL\",\"SJV\",\"SK \",\"SNJ\",\"STX\",\"SV \",\"TN \",  &       \n"
    "       \"UT \",\"VA \",\"VI \",\"VT \",\"WCF\",\"WI \",\"WMA\",\"WNY\",\"WPA\",\"WTX\",  &       \n"
    "       \"WV \",\"WWA\",\"WY \",\"DX \",\"PE \",\"NB \"/\n";

static const char *cmult_data =
    "       \"AL \",\"AK \",\"AZ \",\"AR \",\"CA \",\"CO \",\"CT \",\"DE \",\"FL \",\"GA \",  &\n"
    "       \"HI \",\"ID \",\"IL \",\"IN \",\"IA \",\"KS \",\"KY \",\"LA \",\"ME \",\"MD \",  &\n"
    "       \"MA \",\"MI \",\"MN \",\"MS \",\"MO \",\"MT \",\"NE \",\"NV \",\"NH \",\"NJ \",  &\n"
    "       \"NM \",\"NY \",\"NC \",\"ND \",\"OH \",\"OK \",\"OR \",\"PA \",\"RI \",\"SC \",  &\n"
    "       \"SD \",\"TN \",\"TX \",\"UT \",\"VT \",\"VA \",\"WA \",\"WV \",\"WI \",\"WY \",  &\n"
    "       \"NB \",\"NS \",\"QC \",\"ON \",\"MB \",\"SK \",\"AB \",\"BC \",\"NWT\",\"NF \",  &\n"
    "       \"LB \",\"NU \",\"YT \",\"PEI\",\"DC \",\"DR \",\"FR \",\"GD \",\"GR \",\"OV \",  &\n"
    "       \"ZH \",\"ZL \",\"X01\",\"X02\",\"X03\",\"X04\",\"X05\",\"X06\",\"X07\",\"X08\",  &\n"
    "       \"X09\",\"X10\",\"X11\",\"X12\",\"X13\",\"X14\",\"X15\",\"X16\",\"X17\",\"X18\",  &\n"
    "       \"X19\",\"X20\",\"X21\",\"X22\",\"X23\",\"X24\",\"X25\",\"X26\",\"X27\",\"X28\",  &\n"
    "       \"X29\",\"X30\",\"X31\",\"X32\",\"X33\",\"X34\",\"X35\",\"X36\",\"X37\",\"X38\",  &\n"
    "       \"X39\",\"X40\",\"X41\",\"X42\",\"X43\",\"X44\",\"X45\",\"X46\",\"X47\",\"X48\",  &\n"
    "       \"X49\",\"X50\",\"X51\",\"X52\",\"X53\",\"X54\",\"X55\",\"X56\",\"X57\",\"X58\",  &\n"
    "       \"X59\",\"X60\",\"X61\",\"X62\",\"X63\",\"X64\",\"X65\",\"X66\",\"X67\",\"X68\",  &\n"
    "       \"X69\",\"X70\",\"X71\",\"X72\",\"X73\",\"X74\",\"X75\",\"X76\",\"X77\",\"X78\",  &\n"
    "       \"X79\",\"X80\",\"X81\",\"X82\",\"X83\",\"X84\",\"X85\",\"X86\",\"X87\",\"X88\",  &\n"
    "       \"X89\",\"X90\",\"X91\",\"X92\",\"X93\",\"X94\",\"X95\",\"X96\",\"X97\",\"X98\",  &\n"
    "       \"X99\"/\n";

#define NSEC   86
#define NUSCAN 171

static char csec[NSEC + 1][4], cmult[NUSCAN + 1][4];  /* from 1, as the Fortran; trailing blanks off (trim) */

static int read_data(const char *data, char (*table)[4], int max) {
    int n = 0;

    for (const char *q = strchr(data, '"'); q && n < max; q = strchr(q + 5, '"')) {
        n++;
        memcpy(table[n], q + 1, 3);
        table[n][3] = '\0';
        for (int i = 2; i >= 0 && table[n][i] == ' '; i--) table[n][i] = '\0';
    }
    return n;
}

/* ---- The 77 bits, written in order ------------------------------------------------------------------------ */

typedef struct {
    ftx_message_t msg;
    int           pos;
} bits_t;

static void put(bits_t *b, int len, uint64_t v) {
    for (int i = len - 1; i >= 0; i--, b->pos++)
        if ((v >> i) & 1u) b->msg.payload[b->pos / 8] |= (uint8_t)(0x80u >> (b->pos % 8));
}

static bits_t begin(void) {
    bits_t b;

    memset(&b, 0, sizeof(b));
    return b;
}

static ftx_message_t done(bits_t *b) {
    if (b->pos != 77) {
        printf("  BAD the packer wrote %d bits\n", b->pos);
        bad++;
    }
    return b->msg;
}

/* ---- packjt77.f90, again ---------------------------------------------------------------------------------- */

/* ihashcall(c0, m) with ihashcall_from_n8: the top m bits of the low 64 of 47055833459*n8, by 16-bit limbs */
static uint32_t ihashcall(const char *c0, int m) {
    static const char c[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/";
    static const int64_t hash_factor[4] = {2419, 62655, 10, 0};
    int64_t              n8 = 0, x, b[4], p[4], carry = 0;
    size_t               len = strlen(c0);

    for (size_t i = 0; i < 11; i++) n8 = 38 * n8 + (strchr(c, i < len ? c0[i] : ' ') - c);
    x = n8;
    for (int i = 0; i < 4; i++) {
        b[i] = x % 65536;
        x /= 65536;
    }
    for (int i = 0; i < 4; i++) {
        int64_t term = carry;
        for (int j = 0; j <= i; j++) term += hash_factor[j] * b[i - j];
        p[i]  = term % 65536;
        carry = term / 65536;
    }
    if (m <= 16) return (uint32_t)(p[3] / (1 << (16 - m)));
    return (uint32_t)(p[3] * (1 << (m - 16)) + p[2] / (1 << (32 - m)));
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_letter(char c) {
    return c >= 'A' && c <= 'Z';
}

static int idx(const char *set, char c) {
    const char *p = strchr(set, c);
    return p && c ? (int)(p - set) : -1;
}

/* pack28: a token, a hash ("<CALL>", or a call not standard), a standard call */
static uint32_t pack28(const char *c13) {
    static const char a1[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ", a2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char a3[] = "0123456789", a4[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int               n = (int)strlen(c13), i;

    if (!strcmp(c13, "DE")) return 0;
    if (!strcmp(c13, "QRZ")) return 1;
    if (!strcmp(c13, "CQ")) return 2;
    if (!strncmp(c13, "CQ_", 3) && n >= 4 && n <= 7) {
        int nlet = 0, nnum = 0, m = 0;
        for (i = 3; i < n; i++) nlet += is_letter(c13[i]), nnum += is_digit(c13[i]);
        if (nnum == 3 && nlet == 0) return 3 + (uint32_t)atoi(c13 + 3);
        if (nlet >= 1 && nlet <= 4 && nnum == 0) {
            for (i = 0; i < 4 - (n - 3); i++) m = 27 * m;
            for (i = 3; i < n; i++) m = 27 * m + (c13[i] - 'A' + 1);
            return 3 + 1000 + (uint32_t)m;
        }
    }
    if (c13[0] == '<') {
        char call[14];
        snprintf(call, sizeof(call), "%.*s", (int)strcspn(c13 + 1, ">"), c13 + 1);
        return NTOKENS + ihashcall(call, 22);
    }
    for (i = n; i >= 2; i--)
        if (is_digit(c13[i - 1])) break;
    int iarea = i, npdig = 0, nplet = 0, nslet = 0;
    for (i = 1; i <= iarea - 1; i++) npdig += is_digit(c13[i - 1]), nplet += is_letter(c13[i - 1]);
    for (i = iarea + 1; i <= n; i++) nslet += is_letter(c13[i - 1]);
    if (iarea < 2 || iarea > 3 || nplet == 0 || npdig >= iarea - 1 || nslet > 3) return NTOKENS + ihashcall(c13, 22);
    char callsign[7] = "      ";
    if (iarea == 2) memcpy(callsign + 1, c13, n < 5 ? n : 5);
    else memcpy(callsign, c13, n < 6 ? n : 6);
    uint32_t i1 = idx(a1, callsign[0]), i2 = idx(a2, callsign[1]), i3 = idx(a3, callsign[2]);
    uint32_t i4 = idx(a4, callsign[3]), i5 = idx(a4, callsign[4]), i6 = idx(a4, callsign[5]);
    return 36 * 10 * 27 * 27 * 27 * i1 + 10 * 27 * 27 * 27 * i2 + 27 * 27 * 27 * i3 + 27 * 27 * i4 + 27 * i5 + i6 +
           NTOKENS + MAX22;
}

static int split(const char *msg, char w[][16]) {
    char buf[64];
    int  n = 0;

    snprintf(buf, sizeof(buf), "%s", msg);
    for (char *t = strtok(buf, " "); t && n < 19; t = strtok(NULL, " ")) snprintf(w[n++], 16, "%s", t);
    return n;
}

/* pack77_03: "WA9XYZ KA1ABC R 16A EMA" */
static ftx_message_t pack_fd(const char *msg) {
    char   w[19][16];
    int    nwords = split(msg, w), isec = -1, ntx, nclass, intx, n3 = 3, ir;
    bits_t b = begin();

    for (int i = 1; i <= NSEC; i++)
        if (!strcmp(csec[i], w[nwords - 1])) isec = i;
    int j = (int)strlen(w[nwords - 2]) - 1;
    ntx    = atoi(w[nwords - 2]);
    nclass = w[nwords - 2][j] - 'A';
    intx   = ntx - 1;
    if (intx >= 16) {
        n3   = 4;
        intx = ntx - 17;
    }
    ir = nwords == 5 && !strcmp(w[2], "R");
    if (isec < 0 || (nwords == 5 && !ir)) printf("  BAD pack_fd(%s)\n", msg), bad++;
    put(&b, 28, pack28(w[0]));
    put(&b, 28, pack28(w[1]));
    put(&b, 1, ir);
    put(&b, 4, intx);
    put(&b, 3, nclass);
    put(&b, 7, isec);
    put(&b, 3, n3);
    put(&b, 3, 0);
    return done(&b);
}

/* pack77_3: "TU; W9XYZ K1ABC R 579 MA", "W9XYZ G8ABC 559 0013" */
static ftx_message_t pack_rtty(const char *msg) {
    char   w[19][16];
    int    nwords = split(msg, w), itu = !strcmp(w[0], "TU;"), imult = -1, nserial = 0, nexch = 0, ir, irpt;
    bits_t b = begin();

    if (w[nwords - 2][0] == '5' && w[nwords - 2][1] >= '2' && w[nwords - 2][1] <= '9' && w[nwords - 2][2] == '9')
        nserial = atoi(w[nwords - 1]);  /* list-directed read: "MA" is no number (err=1), nserial stays 0 */
    for (int i = 1; i <= NUSCAN; i++)
        if (!strcmp(cmult[i], w[nwords - 1])) {
            imult = i;
            break;
        }
    if (nserial > 0) nexch = nserial;
    if (imult > 0) nexch = 8000 + imult;
    ir   = !strcmp(w[2 + itu], "R");
    irpt = (atoi(w[2 + itu + ir]) - 509) / 10 - 2;
    if (irpt < 0) irpt = 0;
    if (irpt > 7) irpt = 7;
    put(&b, 1, itu);
    put(&b, 28, pack28(w[itu]));
    put(&b, 28, pack28(w[1 + itu]));
    put(&b, 1, ir);
    put(&b, 3, irpt);
    put(&b, 13, nexch);
    put(&b, 3, 3);
    return done(&b);
}

/* pack77_5: "<PA3XYZ> <G4ABC/P> R 590003 IO91NP" */
static ftx_message_t pack_vhf(const char *msg) {
    char   w[19][16], c1[14], c2[14];
    int    nwords = split(msg, w), nx = atoi(w[nwords - 2]), ir = !strcmp(w[2], "R");
    bits_t b = begin();

    snprintf(c1, sizeof(c1), "%.*s", (int)strcspn(w[0] + 1, ">"), w[0] + 1);
    snprintf(c2, sizeof(c2), "%.*s", (int)strcspn(w[1] + 1, ">"), w[1] + 1);
    const char *g = w[nwords - 1];
    int iserial = nx % 10000 > 2047 ? 2047 : nx % 10000;
    uint32_t igrid6 = (g[0] - 'A') * 18 * 10 * 10 * 24 * 24 + (g[1] - 'A') * 10 * 10 * 24 * 24 +
                      (g[2] - '0') * 10 * 24 * 24 + (g[3] - '0') * 24 * 24 + (g[4] - 'A') * 24 + (g[5] - 'A');
    put(&b, 12, ihashcall(c1, 12));
    put(&b, 22, ihashcall(c2, 22));
    put(&b, 1, ir);
    put(&b, 3, nx / 10000 - 52);
    put(&b, 11, iserial);
    put(&b, 25, igrid6);
    put(&b, 3, 5);
    return done(&b);
}

static int nint_03(int dbm) {   /* nint(0.3*idbm) */
    return (int)(0.3 * dbm + 0.5);
}

/* pack77_06, WSPR type 1: "K1ABC FN42 37" */
static ftx_message_t pack_wspr1(const char *msg) {
    char   w[19][16];
    int    dbm;
    bits_t b = begin();

    split(msg, w);
    dbm = atoi(w[2]);
    put(&b, 28, pack28(w[0]));
    put(&b, 15, (w[1][0] - 'A') * 18 * 10 * 10 + (w[1][1] - 'A') * 10 * 10 + (w[1][2] - '0') * 10 + (w[1][3] - '0'));
    put(&b, 5, nint_03(dbm < 0 ? 0 : dbm > 60 ? 60 : dbm));
    put(&b, 2, 0);
    put(&b, 21, 0);
    put(&b, 3, 6);
    put(&b, 3, 0);
    return done(&b);
}

/* pack77_06, WSPR type 2: "PJ4/K1ABC 37", "K1ABC/P 37" */
static ftx_message_t pack_wspr2(const char *msg) {
    static const char a2[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char              w[19][16], bcall[16];
    int               m1, i1, npfx = 0, dbm;
    bits_t            b = begin();

    split(msg, w);
    m1 = (int)strlen(w[0]);
    i1 = (int)(strchr(w[0], '/') - w[0]) + 1;
    if (i1 <= 4) {
        for (int i = 0; i < i1 - 1; i++) npfx = 36 * npfx + idx(a2, w[0][i]);
        snprintf(bcall, sizeof(bcall), "%s", w[0] + i1);
    } else {
        const char *s = w[0] + i1;
        if (m1 - i1 == 1) npfx = idx(a2, s[0]);
        if (m1 - i1 == 2) npfx = 36 * idx(a2, s[0]) + idx(a2, s[1]);
        if (m1 - i1 == 3) npfx = 36 * 10 * idx(a2, s[0]) + 10 * idx(a2, s[1]) + idx(a2, s[2]);
        npfx += 46656;
        snprintf(bcall, sizeof(bcall), "%.*s", i1 - 1, w[0]);
    }
    dbm = atoi(w[1]);
    put(&b, 28, pack28(bcall));
    put(&b, 16, npfx);
    put(&b, 5, nint_03(dbm < 0 ? 0 : dbm > 60 ? 60 : dbm));
    put(&b, 1, 1);
    put(&b, 21, 0);
    put(&b, 3, 6);
    put(&b, 3, 0);
    return done(&b);
}

/* pack77_06, WSPR type 3 (FST4W's hint only): "<PJ4/K1ABC> FN42AX", "<PJ4/K1ABC> FN42" */
static ftx_message_t pack_wspr3(const char *msg) {
    char     w[19][16];
    bits_t   b = begin();
    uint32_t igrid6;

    split(msg, w);
    const char *g = w[1];
    igrid6 = (g[0] - 'A') * 18 * 10 * 10 * 25 * 25 + (g[1] - 'A') * 10 * 10 * 25 * 25 + (g[2] - '0') * 10 * 25 * 25 +
             (g[3] - '0') * 25 * 25;
    igrid6 += strlen(g) == 4 ? 24 * 25 + 24 : (uint32_t)((g[4] - 'A') * 25 + (g[5] - 'A'));
    put(&b, 22, pack28(w[0]) - NTOKENS);
    put(&b, 25, igrid6);
    put(&b, 3, 2);
    put(&b, 21, 0);
    put(&b, 3, 6);
    put(&b, 3, 0);
    return done(&b);
}

/* ---- What unpack77 shows, put together as it does (Fortran concatenation, trim) --------------------------- */

static void fd_text(char *msg, const char *c1, const char *c2, int ir, int ntx, int nclass, int isec) {
    char cntx[4];

    snprintf(cntx, sizeof(cntx), "%2d%c", ntx, 'A' + nclass);   /* write(cntx(1:2),'(i2)'), cntx(3:3) */
    if (ir == 0 && ntx < 10) snprintf(msg, 64, "%s %s%s %s", c1, c2, cntx, csec[isec]);
    if (ir == 1 && ntx < 10) snprintf(msg, 64, "%s %s R%s %s", c1, c2, cntx, csec[isec]);
    if (ir == 0 && ntx >= 10) snprintf(msg, 64, "%s %s %s %s", c1, c2, cntx, csec[isec]);
    if (ir == 1 && ntx >= 10) snprintf(msg, 64, "%s %s R %s %s", c1, c2, cntx, csec[isec]);
}

static void rtty_text(char *msg, int itu, const char *c1, const char *c2, int ir, int irpt, const char *exch) {
    char crpt[4];

    snprintf(crpt, sizeof(crpt), "5%d9", irpt + 2);
    snprintf(msg, 64, "%s%s %s%s%s %s", itu ? "TU; " : "", c1, c2, ir ? " R " : " ", crpt, exch);
}

/* ---- The calls heard, by their hashes, as ft8_lib's table keeps them -------------------------------------- */

static struct {
    char     call[12];
    uint32_t n22;
} table[1024];
static int table_n;

static bool table_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char *callsign) {
    int shift = type == FTX_CALLSIGN_HASH_10_BITS ? 12 : type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0;

    for (int i = 0; i < table_n; i++)
        if (table[i].n22 >> shift == hash) {
            memcpy(callsign, table[i].call, sizeof(table[i].call));   /* 12 bytes, as ft8_lib's */
            return true;
        }
    callsign[0] = '\0';
    return false;
}

static void table_save(const char *callsign, uint32_t n22) {
    for (int i = 0; i < table_n; i++)
        if (!strcmp(table[i].call, callsign)) return;
    if (table_n < (int)(sizeof(table) / sizeof(table[0]))) {
        snprintf(table[table_n].call, sizeof(table[0].call), "%s", callsign);
        table[table_n++].n22 = n22 & 0x3FFFFFu;
    }
}

static ftx_callsign_hash_interface_t hash_if = {.lookup_hash = table_lookup, .save_hash = table_save};

static void heard(const char *call) {
    ftx_message_t m;
    char          text[40];

    snprintf(text, sizeof(text), "CQ %s", call);    /* ft8_lib's encoder keeps the call as its decoder does */
    ftx_message_init(&m);
    ftx_message_encode(&m, &hash_if, text);
}

static void reset_table(void) {
    table_n = 0;
}

/* ---- Cases ------------------------------------------------------------------------------------------------- */

static bool unpack(const ftx_message_t *m, char *out) {
    out[0] = '\0';
    return contest_unpack(m, &hash_if, out);
}

static void same(const char *what, ftx_message_t m, const char *want) {
    char got[CONTEST_TEXT_LENGTH + 8];
    bool ok = unpack(&m, got);

    check(ok && !strcmp(got, want), "%-6s %-40s -> %s", what, want, ok ? got : "(none)");
}

static void none(const char *what, ftx_message_t m) {
    char got[CONTEST_TEXT_LENGTH + 8];
    bool ok = unpack(&m, got);

    check(!ok, "%-6s none %s", what, ok ? got : "");
}

static void named_cases(void) {
    printf("WSJT-X's examples and the FT4 sample's exchanges, round trip\n");
    reset_table();
    same("0.3", pack_fd("WA9XYZ KA1ABC R 16A EMA"), "WA9XYZ KA1ABC R 16A EMA");
    same("0.3", pack_fd("W9XYZ K1ABC 3A EMA"), "W9XYZ K1ABC 3A EMA");
    same("0.3", pack_fd("W9XYZ K1ABC R 1D DX"), "W9XYZ K1ABC R 1D DX");
    same("0.4", pack_fd("WA9XYZ KA1ABC R 32A EMA"), "WA9XYZ KA1ABC R 32A EMA");
    same("0.4", pack_fd("K1ABC W9XYZ 17F NB"), "K1ABC W9XYZ 17F NB");
    same("3", pack_rtty("TU; W9XYZ K1ABC R 579 MA"), "TU; W9XYZ K1ABC R 579 MA");
    same("3", pack_rtty("W9XYZ G8ABC R 559 0013"), "W9XYZ G8ABC R 559 0013");
    same("3", pack_rtty("TU; W9XYZ G8ABC 529 7999"), "TU; W9XYZ G8ABC 529 7999");
    same("3", pack_rtty("K1ABC VE3ABC 599 PEI"), "K1ABC VE3ABC 599 PEI");
    const char *ft4[] = {"AC6BW KR9A R 559 WI", "K1JT WB4HXE 559 GA", "KB0VHA KA1YQC R 539 MA", "N1TRK N4FKH 569 VA",
                         "NI6G W7DRW 569 AZ", "NZ7P WA7JAY 589 CA", "VE3LON K7RL R 549 WA"};
    for (size_t i = 0; i < sizeof(ft4) / sizeof(ft4[0]); i++) same("3", pack_rtty(ft4[i]), ft4[i]);

    printf("EU VHF contest (i3=5): both calls hashed, heard before or not\n");
    same("5", pack_vhf("<PA3XYZ> <G4ABC/P> R 590003 IO91NP"), "<...> <...> R 590003 IO91NP");
    heard("PA3XYZ");
    heard("G4ABC/P");
    same("5", pack_vhf("<PA3XYZ> <G4ABC/P> R 590003 IO91NP"), "<PA3XYZ> <G4ABC/P> R 590003 IO91NP");
    same("5", pack_vhf("<PA3XYZ> <PA9ZZZ> 520001 AA00AA"), "<PA3XYZ> <...> 520001 AA00AA");
    /* G4ABC/P and PA3XYZ share a 12-bit hash (3211): the first one is what the table holds for it, ft8_lib's too
     * (WSJT-X's calls12 the last one heard); their 22-bit hashes differ */
    same("5", pack_vhf("<G4ABC/P> <PA3XYZ> 520001 AA00AA"), "<PA3XYZ> <PA3XYZ> 520001 AA00AA");
    same("5", pack_vhf("<PA3XYZ> <G4ABC/P> 520001 AA00AA"), "<PA3XYZ> <G4ABC/P> 520001 AA00AA");
    heard("DL1ABC/P");
    heard("PA3XYZ/P");
    same("5", pack_vhf("<DL1ABC/P> <PA3XYZ/P> R 590003 JO31AB"), "<DL1ABC/P> <PA3XYZ/P> R 590003 JO31AB");
    same("5", pack_vhf("<PA3XYZ> <G4ABC/P> 592047 RR99XX"), "<PA3XYZ> <G4ABC/P> 592047 RR99XX");
    same("5", pack_vhf("<PA3XYZ> <G4ABC/P> 599999 RR99XX"), "<PA3XYZ> <G4ABC/P> 592047 RR99XX"); /* 2047 at most */

    printf("WSPR-style (0.6), heard as WSJT-X keeps them\n");
    reset_table();
    same("0.6/1", pack_wspr1("K1ABC FN42 37"), "K1ABC FN42 37");
    same("0.6/1", pack_wspr1("K1ABC FN42 0"), "K1ABC FN42 0");
    same("0.6/1", pack_wspr1("K1ABC FN42 60"), "K1ABC FN42 60");
    same("0.6/2", pack_wspr2("PJ4/K1ABC 37"), "PJ4/K1ABC 37");
    same("0.6/2", pack_wspr2("F/G4ABC 30"), "F/G4ABC 30");
    same("0.6/2", pack_wspr2("K1ABC/P 37"), "K1ABC/P 37");
    same("0.6/2", pack_wspr2("K1ABC/MM 10"), "K1ABC/MM 10");
    same("0.6/2", pack_wspr2("K1ABC/AB1 7"), "K1ABC/AB1 7");
    same("0.6/3", pack_wspr3("<PJ4/K1ABC> FN42AX"), "<PJ4/K1ABC> FN42AX");  /* type 2 above kept it */
    same("0.6/3", pack_wspr3("<PJ4/K1ABC> FN42"), "<PJ4/K1ABC> FN42");
    same("0.6/3", pack_wspr3("<K1ABC/MM> RR99XX"), "<K1ABC/MM> RR99XX");
    same("0.6/3", pack_wspr3("<W9XYZ/7> FN42"), "<...> FN42");
}

static void sweeps(void) {
    char want[64];
    int  n;

    printf("Field Day: every section, transmitters 1-32, class A-H, with and without R\n");
    reset_table();
    n = cases;
    for (int isec = 1; isec <= NSEC; isec++)
        for (int ntx = 1; ntx <= 32; ntx++)
            for (int nclass = 0; nclass < 8; nclass++)
                for (int ir = 0; ir < 2; ir++) {
                    fd_text(want, "WA9XYZ", "KA1ABC", ir, ntx, nclass, isec);
                    same(ntx > 16 ? "0.4" : "0.3", pack_fd(want), want);
                }
    printf("  %d cases\n", cases - n);

    printf("RTTY Roundup: every multiplier and report, TU and R; serials 1-7999\n");
    n = cases;
    for (int imult = 1; imult <= NUSCAN; imult++)
        for (int irpt = 0; irpt < 8; irpt++)
            for (int itu = 0; itu < 2; itu++)
                for (int ir = 0; ir < 2; ir++) {
                    rtty_text(want, itu, "W9XYZ", "K1ABC", ir, irpt, cmult[imult]);
                    same("3", pack_rtty(want), want);
                }
    for (int serial = 1; serial <= 7999; serial++) {
        char s[8];
        snprintf(s, sizeof(s), "%04d", serial);
        rtty_text(want, serial & 1, "W9XYZ", "G8ABC", serial >> 1 & 1, serial % 8, s);
        same("3", pack_rtty(want), want);
    }
    printf("  %d cases\n", cases - n);

    printf("EU VHF contest: reports 52-59, serials 0-2047, a grid each\n");
    heard("PA3XYZ");
    heard("G4ABC/P");
    n = cases;
    for (int irpt = 0; irpt < 8; irpt++)
        for (int serial = 0; serial <= 2047; serial++) {
            char g[7];
            int  k = irpt * 2048 + serial;
            snprintf(g, sizeof(g), "%c%c%d%d%c%c", 'A' + k % 18, 'A' + k / 18 % 18, k % 10, k / 10 % 10, 'A' + k % 24,
                     'A' + k / 24 % 24);
            snprintf(want, sizeof(want), "<PA3XYZ> <G4ABC/P>%s%d%04d %s", k & 1 ? " R " : " ", 52 + irpt, serial, g);
            same("5", pack_vhf(want), want);
        }
    printf("  %d cases\n", cases - n);

    printf("WSPR-style: every power WSJT-X sends, every 1-3 character prefix and suffix form\n");
    n = cases;
    for (int dbm = 0; dbm <= 60; dbm++) {
        int shown = (nint_03(dbm) * 10 + 1) / 3;    /* nint(idbm*10/3.0) */
        snprintf(want, sizeof(want), "K1ABC FN42 %d", shown);
        same("0.6/1", pack_wspr1(want), want);
    }
    const char *fixes[] = {"0/K1ABC 7", "Z/K1ABC 7", "A0/K1ABC 7", "ZZ/K1ABC 7", "A0Z/K1ABC 7", "ZZZ/K1ABC 7",
                           "K1ABC/0 7", "K1ABC/Z 7", "K1ABC/10 7", "K1ABC/ZZ 7", "K1ABC/A00 7", "K1ABC/ZZ9 7"};
    for (size_t i = 0; i < sizeof(fixes) / sizeof(fixes[0]); i++) same("0.6/2", pack_wspr2(fixes[i]), fixes[i]);
    printf("  %d cases\n", cases - n);
}

/* Fields WSJT-X does not show, or shows the other way: raw bits */
static ftx_message_t raw_fd(uint32_t n28a, uint32_t n28b, int ir, int intx, int nclass, int isec, int n3) {
    bits_t b = begin();
    put(&b, 28, n28a), put(&b, 28, n28b), put(&b, 1, ir), put(&b, 4, intx), put(&b, 3, nclass), put(&b, 7, isec);
    put(&b, 3, n3), put(&b, 3, 0);
    return done(&b);
}

static ftx_message_t raw_rtty(int itu, uint32_t n28a, uint32_t n28b, int ir, int irpt, int nexch) {
    bits_t b = begin();
    put(&b, 1, itu), put(&b, 28, n28a), put(&b, 28, n28b), put(&b, 1, ir), put(&b, 3, irpt), put(&b, 13, nexch);
    put(&b, 3, 3);
    return done(&b);
}

static ftx_message_t raw_vhf(uint32_t n12, uint32_t n22, uint32_t igrid6) {
    bits_t b = begin();
    put(&b, 12, n12), put(&b, 22, n22), put(&b, 1, 1), put(&b, 3, 7), put(&b, 11, 3), put(&b, 25, igrid6);
    put(&b, 3, 5);
    return done(&b);
}

static ftx_message_t raw_type(int i3, int n3) {
    bits_t b = begin();
    put(&b, 28, pack28("K1ABC")), put(&b, 28, pack28("W9XYZ")), put(&b, 15, 1234);
    put(&b, 3, n3), put(&b, 3, i3);
    return done(&b);
}

static ftx_message_t raw_wspr(int j48, int j49, int j50) {
    bits_t b = begin();
    put(&b, 28, pack28("K1ABC")), put(&b, 19, 1234), put(&b, 1, j48), put(&b, 1, j49), put(&b, 1, j50);
    put(&b, 21, 0), put(&b, 3, 6), put(&b, 3, 0);
    return done(&b);
}

static ftx_message_t raw_wspr1(uint32_t n28, uint32_t igrid4, uint32_t idbm) {
    bits_t b = begin();
    put(&b, 28, n28), put(&b, 15, igrid4), put(&b, 5, idbm), put(&b, 2, 0), put(&b, 21, 0), put(&b, 3, 6);
    put(&b, 3, 0);
    return done(&b);
}

static ftx_message_t raw_wspr2(uint32_t n28, uint32_t npfx, uint32_t idbm) {
    bits_t b = begin();
    put(&b, 28, n28), put(&b, 16, npfx), put(&b, 5, idbm), put(&b, 1, 1), put(&b, 21, 0), put(&b, 3, 6), put(&b, 3, 0);
    return done(&b);
}

static ftx_message_t raw_wspr3(uint32_t n22, uint32_t igrid6) {
    bits_t b = begin();
    put(&b, 22, n22), put(&b, 25, igrid6), put(&b, 3, 2), put(&b, 21, 0), put(&b, 3, 6), put(&b, 3, 0);
    return done(&b);
}

/* A standard call with a space inside: "AB1 CD" */
static uint32_t n28_spaced(void) {
    return 36 * 10 * 27 * 27 * 27 * 11 + 10 * 27 * 27 * 27 * 11 + 27 * 27 * 27 * 1 + 27 * 27 * 0 + 27 * 3 + 4 +
           NTOKENS + MAX22;
}

static void edges(void) {
    printf("What WSJT-X shows of the rest, or does not\n");
    reset_table();
    uint32_t k1abc = pack28("K1ABC"), w9xyz = pack28("W9XYZ");

    none("0.3", raw_fd(k1abc, w9xyz, 0, 0, 0, 0, 3));                       /* isec 0 */
    none("0.3", raw_fd(k1abc, w9xyz, 0, 0, 0, 87, 3));                      /* isec past NSEC */
    none("0.4", raw_fd(k1abc, w9xyz, 0, 0, 0, 127, 4));
    none("0.3", raw_fd(2, w9xyz, 0, 0, 0, 11, 3));                          /* CQ */
    none("0.3", raw_fd(k1abc, 0, 0, 0, 0, 11, 3));                          /* DE */
    same("0.3", raw_fd(pack28("CQ_FD"), w9xyz, 0, 0, 0, 11, 3), "CQ_FD W9XYZ 1A EMA");
    same("0.3", raw_fd(NTOKENS + ihashcall("PJ4/K1ABC", 22), w9xyz, 1, 1, 7, 84, 3), "<...> W9XYZ R 2H DX");
    none("0.3", raw_fd(n28_spaced(), w9xyz, 0, 0, 0, 11, 3));               /* "AB1 CD": unpack28's QU1RK */
    none("0.3", raw_fd(600000, w9xyz, 0, 0, 0, 11, 3));                     /* a token past CQ_ZZZZ */

    none("3", raw_rtty(0, k1abc, w9xyz, 0, 0, 0));                          /* nexch 0: an empty line */
    none("3", raw_rtty(0, k1abc, w9xyz, 0, 0, 8000));
    none("3", raw_rtty(0, k1abc, w9xyz, 0, 0, 8172));
    none("3", raw_rtty(0, k1abc, w9xyz, 0, 0, 8191));
    same("3", raw_rtty(0, k1abc, w9xyz, 0, 0, 8171), "K1ABC W9XYZ 529 X99");
    same("3", raw_rtty(0, 2, w9xyz, 0, 7, 8021), "CQ W9XYZ 599 MA");
    none("3", raw_rtty(0, 2, NTOKENS + 12345, 0, 7, 8021));                /* "CQ <...> ...": no */
    same("3", raw_rtty(1, 2, NTOKENS + 12345, 0, 7, 8021), "TU; CQ <...> 599 MA");
    same("3", raw_rtty(0, pack28("CQ_RU"), w9xyz, 1, 3, 8001), "CQ_RU W9XYZ R 559 AL");
    same("3", raw_rtty(0, pack28("CQ_001"), w9xyz, 1, 3, 1), "CQ_001 W9XYZ R 559 0001");
    none("3", raw_rtty(0, k1abc, n28_spaced(), 0, 0, 1));

    none("5", raw_vhf(1, 2, 18662400));                                     /* past RR99XX */
    none("5", raw_vhf(1, 2, 33554431));
    same("5", raw_vhf(1, 2, 18662399), "<...> <...> R 590003 RR99XX");

    none("0.6", raw_wspr(1, 1, 0));                                         /* 110: no type */
    none("0.6/1", raw_wspr1(k1abc, 1234, 19));                              /* 63 dBm */
    same("0.6/1", raw_wspr1(k1abc, 1234, 18), "K1ABC AM34 60");
    none("0.6/1", raw_wspr1(k1abc, 32400, 0));                              /* past RR99 */
    same("0.6/1", raw_wspr1(2, 0, 0), "CQ AA00 0");
    same("0.6/1", raw_wspr1(NTOKENS + 1, 0, 3), "<...> AA00 10");
    none("0.6/2", raw_wspr2(k1abc, 46656 + 12960, 1));                      /* past a 3-character suffix */
    same("0.6/2", raw_wspr2(k1abc, 46656 + 12959, 1), "K1ABC/ZZ9 3");
    same("0.6/2", raw_wspr2(k1abc, 0, 1), "0/K1ABC 3");
    none("0.6/2", raw_wspr2(k1abc, 1, 31));
    none("0.6/3", raw_wspr3(5, 18 * 1125000));                              /* past RR99 */
    same("0.6/3", raw_wspr3(5, 17 * 1125000 + 17 * 62500 + 99 * 625 + 24 * 25 + 0), "<...> RR99YA");
    same("0.6/3", raw_wspr3(5, 24 * 25 + 24), "<...> AA00");

    none("0.0", raw_type(0, 0));                                            /* ft8_lib's */
    none("0.1", raw_type(0, 1));                                            /* hound.c's */
    none("0.2", raw_type(0, 2));                                            /* none to WSJT-X 2.1 on */
    none("0.5", raw_type(0, 5));
    none("0.7", raw_type(0, 7));
    none("1", raw_type(1, 0));
    none("2", raw_type(2, 0));
    none("4", raw_type(4, 0));
    none("6", raw_type(6, 0));
    none("7", raw_type(7, 0));
}

/* Each kind's 77 bits at random: no crash (ASan, UBSan), at most 37 characters, nothing qso.cpp reads as a report
 * (a sign), and a message not shown leaves the table as it was */
static int saves;

static bool mock_lookup(ftx_callsign_hash_type_t type, uint32_t hash, char *callsign) {
    if (hash % 3 == 0) {
        callsign[0] = '\0';
        return false;
    }
    strcpy(callsign, type == FTX_CALLSIGN_HASH_22_BITS ? "PJ4/K1ABCDE" : "DL1ABC/P"); /* 11 characters, the most */
    return true;
}

static void mock_save(const char *callsign, uint32_t n22) {
    (void)callsign, (void)n22;
    saves++;
}

static void fuzz(void) {
    ftx_callsign_hash_interface_t mock = {.lookup_hash = mock_lookup, .save_hash = mock_save};
    const int                     kinds[][2] = {{0, 3}, {0, 4}, {0, 6}, {3, 0}, {5, 0}};
    int                           shown = 0, longest = 0, n = 0, bad_text = 0;
    uint64_t                      x = 88172645463325252ull;

    printf("Random bits of each kind\n");
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < 400000; i++) {
            ftx_message_t m;
            char          text[64];

            for (int j = 0; j < 10; j++) {
                x ^= x << 13, x ^= x >> 7, x ^= x << 17;
                m.payload[j] = (uint8_t)x;
            }
            /* i3 in bits 74-76, n3 in 71-73 (i3 0 only), nothing after 76 */
            if (kinds[k][0] == 0) {
                m.payload[8] = (uint8_t)((m.payload[8] & 0xFE) | kinds[k][1] >> 2);
                m.payload[9] = (uint8_t)((kinds[k][1] & 3) << 6);
            } else {
                m.payload[9] = (uint8_t)((m.payload[9] & 0xC0) | kinds[k][0] << 3);
            }
            memset(text, 'Z', sizeof(text));
            int  before = saves;
            bool ok     = contest_unpack(&m, &mock, text);
            n++;
            if (!ok) {
                if (saves != before) bad_text++;
                continue;
            }
            shown++;
            int len = (int)strlen(text);
            if (len > longest) longest = len;
            if (len > CONTEST_TEXT_LENGTH - 1 || strpbrk(text, "+-") || !strncmp(text, "CQ <", 4)) {
                if (bad_text++ < 5) printf("  BAD %s\n", text);
            }
        }
    check(bad_text == 0, "%d random messages, %d shown, the longest %d characters, %d wrong", n, shown, longest,
          bad_text);
}

int main(int argc, char **argv) {
    verbose = argc > 1 && !strcmp(argv[1], "-v");

    check(read_data(csec_data, csec, NSEC) == NSEC && !strcmp(csec[11], "EMA") && !strcmp(csec[86], "NB"),
          "csec: %d sections", NSEC);
    check(read_data(cmult_data, cmult, NUSCAN) == NUSCAN && !strcmp(cmult[64], "PEI") && !strcmp(cmult[171], "X99"),
          "cmult: %d multipliers", NUSCAN);
    check(ihashcall("K1ABC", 22) >> 10 == ihashcall("K1ABC", 12) && ihashcall("K1ABC", 22) >> 12 ==
          ihashcall("K1ABC", 10), "ihashcall: 10 and 12 bits the top of 22");

    named_cases();
    sweeps();
    edges();
    fuzz();

    printf("%d cases, %d bad\n", cases, bad);
    return bad != 0;
}
