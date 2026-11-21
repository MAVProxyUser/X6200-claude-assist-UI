/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: two link ends through two simulated SSB paths, a sample at a time: the procedure, the rates it
 *  settles on, the throughput, and that the two never transmit at once.
 *      test_link [snr_db [offset_hz [ppm [bytes [both]]]]]
 */
#include "channel.h"
#include "link.h"

#include <stdio.h>
#include <stdlib.h>

#define FS 24000.0
#define Q  8192     /* each path's samples in flight: the far clock's rate is taken up here, never a hole */

typedef struct {
    double   q[Q];
    unsigned head, tail;
} fifo_t;

static void fifo_put(fifo_t *f, double x) {
    if (f->head - f->tail < Q)
        f->q[f->head++ % Q] = x;
}

static bool fifo_get(fifo_t *f, double *x) {
    if (f->head == f->tail)
        return false;
    *x = f->q[f->tail++ % Q];
    return true;
}

static void dump(const char *who, link_t *l, uint32_t *shown) {
    for (; *shown < l->nlog; (*shown)++) {
        const link_event_t *e = &l->log[*shown % LINK_LOG];
        printf("%7.2f %-6s %s\n", e->t, who, e->text);
    }
}

int main(int argc, char **argv) {
    double     snr = argc > 1 ? atof(argv[1]) : 300, off = argc > 2 ? atof(argv[2]) : 7, ppm = argc > 3 ? atof(argv[3]) : 40;
    int        bytes = argc > 4 ? atoi(argv[4]) : 8192;
    bool       both  = argc > 5 ? atoi(argv[5]) : false;
    static link_t a, b;
    link_cfg_t ca, cb;
    channel_t  ab, ba;
    fifo_t     fab = {0}, fba = {0};
    uint64_t   overlap = 0;
    uint32_t   sa = 0, sb = 0;
    double     out[4];

    link_defaults(&ca, FS, LINK_SENDER);
    link_defaults(&cb, FS, LINK_RECEIVER);
    ca.name = "X6100";
    cb.name = "X6200";
    ca.data_bytes = cb.data_bytes = bytes;
    ca.both_ways = cb.both_ways = both;
    link_init(&a, &ca);
    link_init(&b, &cb);
    channel_init(&ab, FS, true, off, ppm, snr);
    channel_init(&ba, FS, true, -off, -ppm, snr);
    for (int i = 0; i < Q / 2; i++) {
        fifo_put(&fab, 0.0);
        fifo_put(&fba, 0.0);
    }
    link_start(&a);
    link_start(&b);
    for (uint64_t n = 0; n < (uint64_t)(600 * FS); n++) {
        double xa = link_tx_sample(&a), xb = link_tx_sample(&b), y;
        if (link_keyed(&a) && link_keyed(&b)) {
            if (!overlap)
                printf("%7.2f ****** BOTH KEYED: X6100 %s (keyed at %.2f), X6200 %s (keyed at %.2f)\n", n / FS,
                       link_state_name(a.state), a.keyed_at / FS, link_state_name(b.state), b.keyed_at / FS);
            overlap++;
        }
        int k = channel_run(&ab, xa, out);
        for (int i = 0; i < k; i++)
            fifo_put(&fab, out[i]);
        k = channel_run(&ba, xb, out);
        for (int i = 0; i < k; i++)
            fifo_put(&fba, out[i]);
        link_rx_sample(&b, fifo_get(&fab, &y) ? y : 0.0);
        link_rx_sample(&a, fifo_get(&fba, &y) ? y : 0.0);
        if (getenv("WATCH") && n % 12000 == 0 && n / FS > atof(getenv("WATCH")) && n / FS < atof(getenv("WATCH")) + 7)
            printf("   [X6100 %.2f] quiet %.3f s floor %.2e lev %.2e v21 %d armed %d qam %d planned %d keyed %d\n", n / FS,
                   a.quiet_for / FS, a.busy_floor, a.busy_lev.y, v21_rx_carrier(&a.v21rx), a.fast_armed, a.rx.rx.state,
                   a.planned, a.keyed);
        dump("X6100", &a, &sa);
        dump("X6200", &b, &sb);
        bool a_end = a.state == L_FAILED || (a.state == L_DONE && !a.planned && !a.swap_after_tx);
        bool b_end = b.state == L_FAILED || (b.state == L_DONE && !b.planned && !b.swap_after_tx);
        if ((a_end && b_end) || a.state == L_FAILED || b.state == L_FAILED)
            break;
    }
    printf("-- X6100 %s, X6200 %s; both keyed for %llu samples%s\n", link_state_name(a.state),
           link_state_name(b.state), (unsigned long long)overlap, overlap ? "  <-- BOTH ON THE AIR" : "");
    for (int i = 0; i < 2; i++) {
        link_t *l = i ? &b : &a;
        printf("   %s V.21 frames heard %u, bad %u; fast frames %u, bad %u\n", i ? "X6200" : "X6100", l->h21.good, l->h21.bad,
               l->hfast.good, l->hfast.bad);
        printf("   %s keyed: V.21 %.1f s, training %.1f s, data %.1f s, silence %.1f s; FTT %llu, PPR %llu, resent %llu\n",
               i ? "X6200" : "X6100", l->air_s[0], l->air_s[1], l->air_s[2], l->air_s[3],
               (unsigned long long)l->ftt_count, (unsigned long long)l->ppr_count, (unsigned long long)l->frames_resent);
    }
    return overlap || a.state == L_FAILED || b.state == L_FAILED;
}
