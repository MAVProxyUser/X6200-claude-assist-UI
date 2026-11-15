/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - src/wwv.c on a recording: 11025 Hz mono float32 (make_wwv.py makes them). Prints the lock and each
 *  frame decoded; with an expected "YYYY DDD HH:MM" and a count, checks the first frame's time and the frames.
 *      make && ./wwv_test FILE.f32 [YYYY DDD HH:MM N]
 */

#include "../../src/wwv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    FILE      *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    wwv_t     *w = wwv_new(11025);
    float      buf[1024];
    size_t     n, total = 0;
    wwv_info_t in, first = {0};
    uint32_t   seen = 0;
    int        want_n = argc > 5 ? atoi(argv[5]) : 0, best_ok = 0;

    if (!f || !w) return fprintf(stderr, "usage: wwv_test FILE.f32 [YYYY DDD HH:MM N]\n"), 2;
    while ((n = fread(buf, sizeof(float), 1024, f)) > 0) {
        if (wwv_feed(w, buf, n, total / 11025.0)) {
            wwv_info(w, &in);
            if (in.frames != seen) {
                seen = in.frames;
                if (in.have_time && in.frames_ok > 0) {
                    printf("%7.2f s  frame %u: %04d day %03d %02d:%02d UTC, DUT1 %+.1f, DST %d%d, :00 at %.4f s, %d in a row\n",
                           total / 11025.0, in.frames, in.year, in.yday, in.hour, in.minute, in.dut1, in.dst1, in.dst2,
                           in.minute_t, in.frames_ok);
                    if (!first.have_time) first = in;
                    if (in.frames_ok > best_ok) best_ok = in.frames_ok;
                } else {
                    printf("%7.2f s  frame %u: refused  %.60s\n", total / 11025.0, in.frames, in.symbols);
                }
            }
        }
        total += n;
    }
    wwv_info(w, &in);
    printf("locked %d station %d ticks %.1f dB code %.1f dB second %d symbols \"%s\" frames %u bad %u\n", in.locked,
           in.station, in.tick_db, in.code_db, in.second, in.symbols, in.frames, in.frames_bad);
    if (argc > 5) {
        int y, d, h, m;
        if (sscanf(argv[2], "%d", &y) != 1 || sscanf(argv[3], "%d", &d) != 1 || sscanf(argv[4], "%d:%d", &h, &m) != 2)
            return 2;
        bool ok = first.have_time && first.year == y && first.yday == d && first.hour == h && first.minute == m &&
                  (int)(in.frames - in.frames_bad) >= want_n;
        printf("%s: first frame %04d %03d %02d:%02d, %u good of %u\n", ok ? "PASS" : "FAIL", first.year, first.yday,
               first.hour, first.minute, in.frames - in.frames_bad, in.frames);
        return ok ? 0 : 1;
    }
    return 0;
}
