/* Host test of src/adsb.c, the AIRSPACE screen's decoder: the examples of "The 1090 MHz Riddle" (Junzi Sun), the
 * frames the radio's GNS 5890 stick sends, and the history (track points, ghosts, decay).
 *
 *     cc -std=gnu11 -Wall -Wextra -O1 -g -fsanitize=address,undefined -I ../../src -o adsb_test adsb_test.c \
 *        ../../src/adsb.c -lm -lpthread && ./adsb_test
 */
#include "adsb.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int cases, good;

static void check(bool ok, const char *fmt, ...) {
    va_list ap;

    cases++;
    good += ok;
    printf("  %s  ", ok ? "ok " : "BAD");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static const adsb_plane_t *find(uint32_t icao) {
    for (int i = 0; i < adsb_count(); i++)
        if (adsb_plane(i)->icao == icao) return adsb_plane(i);
    return NULL;
}

static bool feed(const char *hex, double t) { return adsb_frame_hex(hex, strlen(hex), t); }

int main(void) {
    const adsb_plane_t *p;

    printf("CRC\n");
    {
        uint8_t m[14] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
        check(adsb_crc24(m, 14) == 0, "8D4840D6202CC371C32CE0576098 is good");
        m[13] ^= 1;
        check(adsb_crc24(m, 14) != 0, "one bit wrong is not");
        check(feed("8D4840D6202CC371C32CE0576099", 0.5) && adsb_stats()->fixed == 1, "but put right, one bit");
        check(!feed("8D4840D6202CC371C32CE0576066", 0.6) && adsb_stats()->crc_bad == 1, "several wrong: not decoded");
    }

    printf("Identification, position, velocity (The 1090 MHz Riddle)\n");
    adsb_clear();
    check(feed("8D4840D6202CC371C32CE0576098", 1.0), "taken");
    p = find(0x4840D6);
    check(p && !strcmp(p->call, "KLM1023"), "call sign KLM1023 (%s)", p ? p->call : "-");
    feed("8D40621D58C386435CC412692AD6", 2.0);   /* odd */
    feed("8D40621D58C382D690C8AC2863A7", 3.0);   /* even, the newest */
    p = find(0x40621D);
    check(p && p->pos_ok && fabs(p->lat - 52.25720) < 1e-4 && fabs(p->lon - 3.91937) < 1e-4,
          "position from the pair 52.25720 3.91937 (%.5f %.5f)", p ? p->lat : 0, p ? p->lon : 0);
    check(p && p->alt_ok && p->alt == 38000, "altitude 38000 ft (%d)", p ? p->alt : 0);
    feed("8D485020994409940838175B284F", 4.0);
    p = find(0x485020);
    check(p && p->vel_ok && fabs(p->gs - 159.2) < 0.1 && fabs(p->trk - 182.88) < 0.05 && p->vr_ok && p->vr == -832,
          "159.2 kt, 182.88 deg, -832 ft/min (%.1f %.2f %d)", p ? p->gs : 0, p ? p->trk : 0, p ? p->vr : 0);

    printf("One frame near the receiver\n");
    adsb_clear();
    adsb_set_ref(true, 52.258, 3.918);
    feed("8D40621D58C382D690C8AC2863A7", 10.0);
    p = find(0x40621D);
    check(p && p->pos_ok && fabs(p->lat - 52.25720) < 1e-4 && fabs(p->lon - 3.91937) < 1e-4,
          "the even frame alone: 52.25720 3.91937 (%.5f %.5f)", p ? p->lat : 0, p ? p->lon : 0);
    adsb_set_ref(false, 0, 0);
    adsb_clear();
    feed("8D40621D58C382D690C8AC2863A7", 10.0);
    p = find(0x40621D);
    check(p && !p->pos_ok, "no reference and no pair: no position yet");

    printf("The stick's own frames (X6200, 2026-11-13)\n");
    adsb_clear();
    check(feed("8DAA1219990DB9807804A7A3D488", 20.0), "AA1219 velocity");
    p = find(0xAA1219);
    check(p && p->vel_ok && fabs(p->trk - 270) < 2 && p->gs > 400 && p->gs < 460, "westbound, %.0f kt", p ? p->gs : 0);
    check(feed("8DA1E3029910F293786492A0A3F7", 21.0), "A1E302 velocity");
    check(!feed("5DA1E302F8230A", 22.0) && adsb_stats()->shorts == 1, "a short frame counted, not decoded");
    check(!feed("8DA1E302F8230006004AB8C7D4", 23.0) && adsb_stats()->junk == 1, "a cut frame is junk");

    printf("History: track points, ghosts, decay\n");
    adsb_clear();
    adsb_set_ref(true, 52.258, 3.918);
    for (int i = 0; i < 20; i++) feed("8D40621D58C382D690C8AC2863A7", 100.0 + i * 20.0);   /* same place, 20 s apart */
    p = find(0x40621D);
    check(p && p->trail_n == 20, "a point every 15 s or more when standing still (%d)", p ? p->trail_n : 0);
    adsb_expire(500.0, 600.0, 120.0);
    p = find(0x40621D);
    check(p && p->trail_n == 6, "decay 2 min: the last 6 points left (%d)", p ? p->trail_n : 0);
    adsb_expire(500.0, -1.0, -1.0);
    check(find(0x40621D) != NULL, "HOLD keeps it");
    adsb_expire(1000.0, 300.0, 300.0);
    check(find(0x40621D) == NULL, "gone 300 s after it was last heard");
    {
        uint16_t r[ADSB_RATE_S];
        adsb_clear();
        for (int i = 0; i < 7; i++) feed("8D4840D6202CC371C32CE0576098", 1000.2 + i * 0.1);
        feed("8D4840D6202CC371C32CE0576098", 1003.5);
        adsb_rate(1003.9, r, 5);
        check(r[0] == 0 && r[1] == 7 && r[2] == 0 && r[3] == 0 && r[4] == 1, "rate 0 7 0 0 1 (%u %u %u %u %u)", r[0],
              r[1], r[2], r[3], r[4]);
    }
    {
        char hex[29];
        adsb_clear();
        for (int i = 0; i < ADSB_MAX_PLANES + 5; i++) {
            uint8_t m[14] = {0x8D, 0, 0, 0, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0, 0, 0};
            m[1] = 0x10;
            m[2] = (uint8_t)(i >> 8);
            m[3] = (uint8_t)i;
            uint32_t c = adsb_crc24(m, 11);    /* the parity: the CRC of the first 88 bits */
            m[11] = (uint8_t)(c >> 16), m[12] = (uint8_t)(c >> 8), m[13] = (uint8_t)c;
            for (int k = 0; k < 14; k++) snprintf(hex + 2 * k, 3, "%02X", m[k]);
            feed(hex, 2000.0 + i);
        }
        check(adsb_count() == ADSB_MAX_PLANES && find(0x100000) == NULL && find(0x100000 + ADSB_MAX_PLANES + 4),
              "full: the one heard longest ago makes room (%d)", adsb_count());
    }

    printf("%d of %d ADS-B cases as they should be\n", good, cases);
    return good == cases ? 0 : 1;
}
