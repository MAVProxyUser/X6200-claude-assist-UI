/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the link after T.30 (link.h).
 */
#include "link.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LEVEL        0.35       /* of full scale: V.21 and V.29's outer points with room over them */
#define VERSION      1
#define SENDER_WAIT  6.0        /* s after the sender's transmission for the answer */
#define RX_WAIT      20.0       /* s the receiver waits for a burst it was told of */
#define TRIES        4
#define PPR_ROUNDS   3
#define TCF_SKIP     0.10       /* of the check's bits, not counted: the receiver settling */
#define TCF_OK       1e-3
#define MUTE_AFTER   0.05       /* s after unkeying the receiver is not fed (our own radio coming back) */
#define QUIET_LEVEL  2.0        /* the line busy above this much of its quiet level (6 dB: a radio's AGC holds its
                                 * noise within 8 dB of a burst, and 12 dB took a far end's data for a quiet line) ... */
#define QUIET_ABS    3e-4       /* ... and above this (-70 dBFS) */
#define FLOOR_MIN    1e-5
#define REPLY_MAX    2.5        /* s: an answer not keyed by then is dropped (the sender may be about to try again) */
#define RETRY_QUIET  1.0        /* s of quiet line before the sender tries again */

void link_defaults(link_cfg_t *c, double fs, link_role_t role) {
    memset(c, 0, sizeof(*c));
    c->fs          = fs;
    c->role        = role;
    c->max_rate    = RATE_9600;
    c->data_bytes  = 16384;
    c->frame_bytes = 256;
    c->burst_s     = 8.0;
    c->lead_s      = 0.25;
    c->tail_s      = 0.05;
    c->reply_s     = 0.20;
    c->preamble_s  = 0.4;
    c->tcf_s       = 1.5;
    c->name        = "radio";
    c->v21_channel = 2;
}

const char *link_state_name(link_state_t s) {
    static const char *n[] = {"idle", "capabilities", "training", "data", "end", "done", "failed",
                              "listening", "training check", "receiving data"};
    return s <= L_RXDATA ? n[s] : "?";
}

double link_seconds(const link_t *l) {
    return l->now / l->cfg.fs;
}

static void say(link_t *l, const char *fmt, ...) {
    link_event_t *e = &l->log[l->nlog % LINK_LOG];
    va_list       ap;
    e->t = link_seconds(l);
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof(e->text), fmt, ap);
    va_end(ap);
    l->nlog++;
}

/* the sender's data, byte i of it: the same at both ends */
static uint8_t data_byte(uint32_t i) {
    uint32_t x = i * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return (uint8_t)x;
}

static void on_fast_bit(void *ctx, int bit);

void link_init(link_t *l, const link_cfg_t *cfg) {
    memset(l, 0, sizeof(*l));
    l->cfg  = *cfg;
    l->role = cfg->role;
    v21_tx_init(&l->v21tx, cfg->fs);
    v21_tx_channel(&l->v21tx, cfg->v21_channel);
    v21_rx_init_channel(&l->v21rx, cfg->fs, cfg->v21_channel);
    pump_init(&l->tx, cfg->fs, NULL, NULL);
    pump_init(&l->rx, cfg->fs, on_fast_bit, l);
    hdlc_rx_reset(&l->h21);
    hdlc_rx_reset(&l->hfast);
    bandpass(&l->busy_band, 4, 300.0, 3000.0, cfg->fs);
    onepole_init(&l->busy_lev, 0.020, cfg->fs);
    l->points_rate = -1;
}

void link_free(link_t *l) {
    pump_free(&l->tx);
    pump_free(&l->rx);
}

/* ---- planning a transmission ---- */

static void plan_begin(link_t *l) {
    l->nplan = 0;
    l->at    = 0;
}

static void plan_add(link_t *l, seg_t s) {
    if (l->nplan < (int)(sizeof(l->plan) / sizeof(l->plan[0])))
        l->plan[l->nplan++] = s;
}

static void plan_silence(link_t *l, double s) {
    plan_add(l, (seg_t){.kind = SEG_SILENCE, .samples = (int)(s * l->cfg.fs)});
}

static void plan_v21(link_t *l, const uint8_t *f, int n) {
    seg_t s = {.kind = SEG_V21, .nframe = n};
    memcpy(s.frame, f, (size_t)n);
    plan_add(l, s);
}

/* the plan goes once the line has been quiet for reply_s: never over the far end */
static void plan_go(link_t *l) {
    l->at          = 0;
    l->seg_started = false;
    l->planned     = true;
    l->plan_made   = l->now;
    l->plan_reply  = l->role == LINK_RECEIVER;
    l->deadline    = 0;
}

static void send_frame(link_t *l, const uint8_t *f, int n) {
    plan_begin(l);
    plan_silence(l, l->cfg.lead_s);
    plan_v21(l, f, n);
    plan_silence(l, l->cfg.tail_s);
    plan_go(l);
}

static int rate_frames(const link_t *l, rate_t r) {
    double bits_each = 8.0 * (l->cfg.frame_bytes + 9) * 1.03;
    int    n         = (int)(l->cfg.burst_s * rate_bps(r) / bits_each);
    return n < 1 ? 1 : n > 255 ? 255 : n;
}

/* ---- the sender ---- */

static void send_cap(link_t *l) {
    uint8_t f[16] = {'C', VERSION, (uint8_t)l->cfg.max_rate};
    int     n     = (int)strlen(l->cfg.name);
    n             = n > 12 ? 12 : n;
    memcpy(f + 3, l->cfg.name, (size_t)n);
    say(l, "TX CAP: up to %s", rate_name(l->cfg.max_rate));
    send_frame(l, f, 3 + n);
}

static void send_train(link_t *l) {
    uint8_t f[5] = {'D', (uint8_t)l->rate, (uint8_t)l->block_frames, (uint8_t)l->cfg.frame_bytes,
                    (uint8_t)(l->cfg.frame_bytes >> 8)};
    say(l, "TX DCS %s, then the training check (%.1f s of zeros)", rate_name(l->rate), l->cfg.tcf_s);
    plan_begin(l);
    plan_silence(l, l->cfg.lead_s);
    plan_v21(l, f, sizeof(f));
    plan_silence(l, 0.075);
    plan_add(l, (seg_t){.kind = SEG_FAST_TCF, .rate = l->rate});
    plan_silence(l, l->cfg.tail_s);
    plan_go(l);
}

static int block_size(const link_t *l) {
    int left = l->frames_total - l->frames_done;
    return left < l->block_frames ? left : l->block_frames;
}

static void send_block(link_t *l) {
    int want = 0;
    for (int i = 0; i < block_size(l); i++)
        want += (l->need[i / 8] >> (i % 8)) & 1;
    say(l, "TX block %d at %s: %d frame%s of %d bytes", l->block, rate_name(l->rate), want, want == 1 ? "" : "s",
        l->cfg.frame_bytes);
    plan_begin(l);
    plan_silence(l, l->cfg.lead_s);
    plan_add(l, (seg_t){.kind = SEG_FAST_DATA, .rate = l->rate});
    plan_silence(l, l->cfg.tail_s);
    plan_go(l);
}

static void new_block(link_t *l) {
    memset(l->need, 0, sizeof(l->need));
    for (int i = 0; i < block_size(l); i++)
        l->need[i / 8] |= (uint8_t)(1 << (i % 8));
    l->ppr_rounds = 0;
}

static void send_eop(link_t *l) {
    uint8_t f[1] = {'E'};
    say(l, "TX EOP");
    send_frame(l, f, 1);
}

static void become(link_t *l, link_role_t role);

static void sender_frame(link_t *l, const uint8_t *f, int n) {
    switch (f[0]) {
    case 'C':
        if (n < 3)
            return;
        l->far_max = f[2] < RATE_COUNT ? (rate_t)f[2] : RATE_2400;
        snprintf(l->far_name, sizeof(l->far_name), "%.*s", n - 3 > 12 ? 12 : n - 3, (const char *)f + 3);
        if (l->state != L_CAP)
            return;
        l->rate         = l->cfg.max_rate < l->far_max ? l->cfg.max_rate : l->far_max;
        l->block_frames = rate_frames(l, l->rate);
        say(l, "RX CAP from %s: up to %s", l->far_name, rate_name(l->far_max));
        l->state = L_TRAIN;
        l->tries = 0;
        send_train(l);
        break;
    case 'K':
        if (l->state != L_TRAIN)
            return;
        say(l, "RX CFR: the check came through at %s (their receiver %.1f dB)", rate_name(l->rate),
            n > 1 ? f[1] / 4.0 : 0.0);
        if (l->state == L_TRAIN && !l->resuming)
            new_block(l);
        l->resuming = false;
        l->state    = L_DATA;
        l->tries    = 0;
        send_block(l);
        break;
    case 'F':
        if (l->state != L_TRAIN)
            return;
        l->ftt_count++;
        say(l, "RX FTT: the check failed at %s%s", rate_name(l->rate), l->rate > RATE_2400 ? ", stepping down" : "");
        if (l->rate == RATE_2400) {
            l->state = L_FAILED;
            say(l, "no rate trains: giving up");
            return;
        }
        l->rate--;
        l->block_frames = rate_frames(l, l->rate);
        l->tries        = 0;
        send_train(l);
        break;
    case 'M':
        if (l->state == L_DATA && n >= 2 && f[1] == (uint8_t)l->block) {
            int got = block_size(l);
            l->frames_done += got;
            uint64_t bytes = (uint64_t)l->frames_done * l->cfg.frame_bytes;
            l->bytes_delivered = bytes > (uint64_t)l->cfg.data_bytes ? (uint64_t)l->cfg.data_bytes : bytes;
            say(l, "RX MCF: block %d confirmed (%llu of %d bytes)", l->block, (unsigned long long)l->bytes_delivered,
                l->cfg.data_bytes);
            l->block++;
            l->tries = 0;
            if (l->frames_done >= l->frames_total) {
                l->state = L_EOP;
                send_eop(l);
            } else {
                new_block(l);
                send_block(l);
            }
        } else if (l->state == L_EOP) {
            l->t_end = link_seconds(l);
            say(l, "RX MCF: done, %d bytes in %.1f s: %.0f bit/s", l->cfg.data_bytes, l->t_end - l->t_start,
                8.0 * l->cfg.data_bytes / (l->t_end - l->t_start));
            l->state = L_DONE;
            if (l->round + 1 < l->rounds)
                become(l, LINK_RECEIVER);
        }
        break;
    case 'P': {
        if (l->state != L_DATA || n < 34 || f[1] != (uint8_t)l->block)
            return;
        int missing = 0;
        for (int i = 0; i < 32; i++)
            l->need[i] &= f[2 + i];
        for (int i = 0; i < block_size(l); i++)
            missing += (l->need[i / 8] >> (i % 8)) & 1;
        l->ppr_count++;
        l->frames_resent += missing;
        l->ppr_rounds++;
        say(l, "RX PPR: block %d, %d frame%s missing", l->block, missing, missing == 1 ? "" : "s");
        l->tries = 0;
        if (missing == 0) {
            /* nothing missing after all: as an MCF */
            uint8_t m[2] = {'M', (uint8_t)l->block};
            sender_frame(l, m, 2);
        } else if (l->ppr_rounds >= PPR_ROUNDS && l->rate > RATE_2400) {
            l->rate--;
            say(l, "%d rounds on one block: stepping down to %s", PPR_ROUNDS, rate_name(l->rate));
            l->ppr_rounds = 0;
            l->state      = L_TRAIN;
            l->resuming   = true;
            send_train(l);
        } else
            send_block(l);
        break;
    }
    }
}

/* ---- the receiver ---- */

static void reply(link_t *l, char type, const uint8_t *extra, int n) {
    uint8_t f[40] = {(uint8_t)type};
    if (n > 0)
        memcpy(f + 1, extra, (size_t)n);
    send_frame(l, f, 1 + n);
}

static uint8_t snr_q(double db) {
    double q = db * 4.0;
    return (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
}

static void arm_fast(link_t *l, bool tcf) {
    pump_rx_arm(&l->rx, l->rate);
    l->fast_armed = true;
    l->fast_tcf   = tcf;
    l->fast_was   = QAM_IDLE;
    l->fast_fails = l->rx.rx.fails;
    l->tcf_bits = l->tcf_ones = l->tcf_seen = 0;
    hdlc_rx_reset(&l->hfast);
    l->points_rate = l->rate;
}

static void receiver_frame(link_t *l, const uint8_t *f, int n) {
    switch (f[0]) {
    case 'C': {
        if (n < 3)
            return;
        l->far_max = f[2] < RATE_COUNT ? (rate_t)f[2] : RATE_2400;
        snprintf(l->far_name, sizeof(l->far_name), "%.*s", n - 3 > 12 ? 12 : n - 3, (const char *)f + 3);
        say(l, "RX CAP from %s: up to %s", l->far_name, rate_name(l->far_max));
        uint8_t me[14] = {VERSION, (uint8_t)l->cfg.max_rate};
        int     k      = (int)strlen(l->cfg.name);
        k              = k > 12 ? 12 : k;
        memcpy(me + 2, l->cfg.name, (size_t)k);
        say(l, "TX CAP: up to %s", rate_name(l->cfg.max_rate));
        reply(l, 'C', me, 2 + k);
        l->state = L_LISTEN;
        break;
    }
    case 'D':
        if (n < 5)
            return;
        l->rate             = f[1] < RATE_COUNT ? (rate_t)f[1] : RATE_2400;
        l->block_frames     = f[2];
        l->cfg.frame_bytes  = f[3] | f[4] << 8;
        say(l, "RX DCS: %s, blocks of %d frames", rate_name(l->rate), l->block_frames);
        arm_fast(l, true);
        /* the check follows in the same transmission: its closing flags (two, 53 ms), 75 ms, then the turn-on.
         * Looked for there, from 0.1 s before to 0.35 s after, rather than by its level rising */
        qamrx_expect(&l->rx.rx, l->rx.rx.n + (uint64_t)((2 * 8 / V21_BAUD + 0.075) * l->cfg.fs), 0.1, 0.35);
        l->state = L_TCF;
        /* the check's end, and 1.5 s more: then FTT whatever came. Its answer keys by REPLY_MAX after that, which is
         * 2 s before the sender's own next try (its SENDER_WAIT, from its end of the check) */
        l->deadline = l->now + (uint64_t)((0.1 + pump_train_seconds(l->rate) + l->cfg.tcf_s + 1.5) * l->cfg.fs);
        break;
    case 'E':
        say(l, "RX EOP: TX MCF");
        reply(l, 'M', NULL, 0);
        l->state      = L_DONE;
        l->fast_armed = false;
        l->t_end      = link_seconds(l);
        if (l->round + 1 < l->rounds)
            l->swap_after_tx = true;
        break;
    }
}

static void tcf_verdict(link_t *l, bool trained) {
    double expect = l->cfg.tcf_s * rate_bps(l->rate);
    bool   ok     = trained && l->tcf_bits > 0.6 * expect * (1.0 - 2 * TCF_SKIP) && l->tcf_ones <= TCF_OK * l->tcf_bits;
    l->tcf_ber    = l->tcf_bits ? (double)l->tcf_ones / l->tcf_bits : 1.0;
    l->fast_armed = false;
    if (ok) {
        uint8_t s[1] = {snr_q(l->rx.rx.snr_db)};
        say(l, "TCF %s: %u bits, %u wrong, receiver %.1f dB: TX CFR", rate_name(l->rate), l->tcf_bits, l->tcf_ones,
            l->rx.rx.snr_db);
        reply(l, 'K', s, 1);
        memset(l->got, 0, sizeof(l->got));
        l->rx_block = -1;
        l->state    = L_RXDATA;     /* armed for the block once the CFR is out (tx_done) */
        l->deadline = 0;
    } else {
        uint8_t s[1] = {snr_q(l->rx.rx.snr_db)};
        if (!trained)
            say(l, "TCF %s: not trained (equaliser %.1f dB): TX FTT", rate_name(l->rate), l->rx.rx.trained_snr_db);
        else
            say(l, "TCF %s: %u bits, %u wrong: TX FTT", rate_name(l->rate), l->tcf_bits, l->tcf_ones);
        reply(l, 'F', s, 1);
        l->state    = L_LISTEN;
        l->deadline = 0;
    }
}

static void block_verdict(link_t *l) {
    int     nf = l->rx_block_frames > 0 ? l->rx_block_frames : l->block_frames;
    uint8_t miss[33];
    int     missing = 0;

    if (getenv("LINK_DEBUG"))
        fprintf(stderr, "%.2f verdict: qam state %d fails %d k %d keyed %d planned %d armed %d\n", link_seconds(l),
                l->rx.rx.state, l->rx.rx.fails, l->rx.rx.k, l->keyed, l->planned, l->fast_armed);
    if (l->rx_block < 0)
        l->rx_block = l->expect_block;
    memset(miss, 0, sizeof(miss));
    miss[0] = (uint8_t)l->rx_block;
    for (int i = 0; i < nf; i++)
        if (!((l->got[i / 8] >> (i % 8)) & 1)) {
            miss[1 + i / 8] |= (uint8_t)(1 << (i % 8));
            missing++;
        }
    if (missing == 0) {
        say(l, "block %d: all %d frames: TX MCF", l->rx_block, nf);
        uint8_t b[1] = {(uint8_t)l->rx_block};
        reply(l, 'M', b, 1);
        l->expect_block = l->rx_block + 1;
        l->rx_block     = -1;
        memset(l->got, 0, sizeof(l->got));
    } else {
        say(l, "block %d: %d of %d frames missing: TX PPR", l->rx_block, missing, nf);
        l->frames_bad += missing;
        reply(l, 'P', miss, 33);
    }
    /* not listening for the next block till the answer is out: the end of this one is no burst */
    l->fast_armed = false;
}

static void on_fast_frame(link_t *l, const uint8_t *f, int n) {
    if (n < 7 || f[0] != 'B')
        return;
    int      block = f[1], idx = f[2], nf = f[3];
    uint32_t g     = (uint32_t)(f[4] | f[5] << 8 | f[6] << 16);
    if (block != l->rx_block) {
        l->rx_block = block;
        memset(l->got, 0, sizeof(l->got));
    }
    l->rx_block_frames = nf;
    for (int i = 7; i < n; i++)
        if (f[i] != data_byte(g * (uint32_t)l->cfg.frame_bytes + (uint32_t)(i - 7))) {
            l->frames_bad++;
            return;
        }
    if (!((l->got[idx / 8] >> (idx % 8)) & 1)) {
        l->got[idx / 8] |= (uint8_t)(1 << (idx % 8));
        l->bytes_delivered += (uint64_t)(n - 7);
    }
}

static void on_fast_bit(void *ctx, int bit) {
    link_t *l = ctx;
    uint8_t f[LINK_FRAME_MAX + 8];
    if (!l->fast_armed)
        return;
    if (l->fast_tcf) {
        /* the check's middle: not the receiver settling at its start, nor the turn-off's scrambled ones
         * after its end */
        double   expect = l->cfg.tcf_s * rate_bps(l->rate);
        uint32_t i      = l->tcf_seen++;
        if (i >= (uint32_t)(TCF_SKIP * expect) && i < (uint32_t)((1.0 - TCF_SKIP) * expect)) {
            l->tcf_bits++;
            l->tcf_ones += bit;
        }
        return;
    }
    int n = hdlc_rx_bit(&l->hfast, bit, f, sizeof(f));
    if (n > 0)
        on_fast_frame(l, f, n);
}

/* ---- roles ---- */

static void become(link_t *l, link_role_t role) {
    l->role = role;
    l->round++;
    l->tries = 0;
    if (role == LINK_SENDER) {
        l->state        = L_CAP;
        l->frames_total = (l->cfg.data_bytes + l->cfg.frame_bytes - 1) / l->cfg.frame_bytes;
        l->frames_done  = 0;
        l->block        = 0;
        l->bytes_delivered = 0;
        l->t_start      = link_seconds(l);
        say(l, "-- sending %d bytes --", l->cfg.data_bytes);
        send_cap(l);
    } else {
        l->state        = L_LISTEN;
        l->expect_block = 0;
        l->rx_block     = -1;
        l->bytes_delivered = 0;
        l->t_start      = link_seconds(l);
        say(l, "-- receiving --");
    }
}

void link_tone(link_t *l, double hz, double seconds) {
    l->role  = LINK_RECEIVER;
    l->state = L_DONE;
    say(l, "TX a tone: %.0f Hz for %.1f s", hz, seconds);
    plan_begin(l);
    plan_silence(l, l->cfg.lead_s);
    plan_add(l, (seg_t){.kind = SEG_TONE, .samples = (int)(seconds * l->cfg.fs), .hz = hz});
    plan_silence(l, l->cfg.tail_s);
    plan_go(l);
    l->plan_reply = false;
}

void link_start(link_t *l) {
    l->rounds = l->cfg.rounds > 0 ? l->cfg.rounds : l->cfg.both_ways ? 2 : 1;
    l->round  = -1;
    become(l, l->cfg.role);
}

static void timeout(link_t *l) {
    l->deadline = 0;
    if (l->role == LINK_RECEIVER) {
        if (l->state == L_TCF) {
            say(l, "no training check heard");
            tcf_verdict(l, false);
            return;
        }
        l->state = l->state == L_DONE ? L_DONE : L_LISTEN;
        return;
    }
    if (++l->tries > TRIES) {
        say(l, "no answer after %d tries: giving up", TRIES);
        l->state = L_FAILED;
        return;
    }
    say(l, "no answer: again (%d)", l->tries);
    switch (l->state) {
    case L_CAP: send_cap(l); break;
    case L_TRAIN: send_train(l); break;
    case L_DATA: send_block(l); break;
    case L_EOP: send_eop(l); break;
    default: break;
    }
}

static void tx_done(link_t *l) {
    if (getenv("LINK_DEBUG"))
        fprintf(stderr, "%.2f %s tx done, state %s\n", link_seconds(l), l->cfg.name, link_state_name(l->state));
    if (l->role == LINK_SENDER && l->state != L_DONE && l->state != L_FAILED)
        l->deadline = l->now + (uint64_t)(SENDER_WAIT * l->cfg.fs);
    if (l->role == LINK_RECEIVER && l->state == L_RXDATA) {
        arm_fast(l, false);
        l->deadline = l->now + (uint64_t)(RX_WAIT * l->cfg.fs);
    }
    if (l->swap_after_tx) {
        l->swap_after_tx = false;
        become(l, LINK_SENDER);
    }
}

/* ---- transmitting, a sample at a time ---- */

static void put_v21(void *ctx, int bit) {
    bitq_push(&((link_t *)ctx)->v21tx.pending, bit);
}

static void put_fast(void *ctx, int bit) {
    pump_tx_push(&((link_t *)ctx)->tx, bit);
}

/* data frames into the fast transmitter as its queue empties */
static void top_up_data(link_t *l) {
    while (pump_tx_pending(&l->tx) < 4096 && l->fast_next < block_size(l)) {
        int i = l->fast_next++;
        if (!((l->need[i / 8] >> (i % 8)) & 1))
            continue;
        uint8_t  f[LINK_FRAME_MAX + 8];
        uint32_t g    = (uint32_t)(l->frames_done + i);
        int      left = l->cfg.data_bytes - (int)(g * (uint32_t)l->cfg.frame_bytes);
        int      len  = left < l->cfg.frame_bytes ? left : l->cfg.frame_bytes;
        f[0]          = 'B';
        f[1]          = (uint8_t)l->block;
        f[2]          = (uint8_t)i;
        f[3]          = (uint8_t)block_size(l);
        f[4]          = (uint8_t)g;
        f[5]          = (uint8_t)(g >> 8);
        f[6]          = (uint8_t)(g >> 16);
        for (int k = 0; k < len; k++)
            f[7 + k] = data_byte(g * (uint32_t)l->cfg.frame_bytes + (uint32_t)k);
        hdlc_frame(put_fast, l, f, 7 + len);
    }
    if (l->fast_next >= block_size(l) && !l->fast_flags_done) {
        hdlc_flags(put_fast, l, 4);
        l->fast_flags_done = true;
        pump_tx_stop(&l->tx);
    }
}

static bool line_quiet(link_t *l) {
    return l->quiet_for >= (uint64_t)(l->cfg.reply_s * l->cfg.fs);
}

double link_tx_sample(link_t *l) {
    double x = 0.0;

    l->now++;
    /* the sender tries again only on a line quiet for a while: an answer on its way restarts the wait */
    if (l->deadline && l->now >= l->deadline && !l->keyed && !l->planned) {
        if (l->role == LINK_RECEIVER || l->quiet_for >= (uint64_t)(RETRY_QUIET * l->cfg.fs))
            timeout(l);
    }
    if (!l->planned)
        return 0.0;
    if (!l->keyed) {
        /* the receiver answers within REPLY_MAX of deciding to, or not at all: the sender's next try comes
         * later than that, so the two never start together */
        /* counted from the line's last sound: an answer decided while the far end is still sending (a check that did
         * not train, its 1.5 s still to come) waits for it, then has REPLY_MAX */
        if (!line_quiet(l) && l->plan_reply)
            l->plan_made = l->now;
        if (l->plan_reply && l->now - l->plan_made > (uint64_t)(REPLY_MAX * l->cfg.fs)) {
            l->planned = false;
            say(l, "answer not sent: the line was not quiet in time");
            return 0.0;
        }
        if (!line_quiet(l) || l->hold)
            return 0.0;
        l->keyed    = true;
        l->keyed_at = l->now;
    }
    if (l->at >= l->nplan) {
        l->keyed      = false;
        l->planned    = false;
        l->unkeyed_at = l->now;
        tx_done(l);
        return 0.0;
    }
    seg_t *s = &l->plan[l->at];
    if (!l->seg_started) {
        l->seg_started = true;
        switch (s->kind) {
        case SEG_SILENCE:
        case SEG_TONE:
            l->seg_left   = s->samples;
            l->tone_phase = 0.0;
            break;
        case SEG_V21:
            bitq_clear(&l->v21tx.pending);
            hdlc_flags(put_v21, l, (int)(l->cfg.preamble_s * V21_BAUD / 8) + 1);
            hdlc_frame(put_v21, l, s->frame, s->nframe);
            hdlc_flags(put_v21, l, 2);
            v21_tx_set(&l->v21tx, true);
            l->seg_left    = -1;
            l->points_rate = -1;
            break;
        case SEG_FAST_TCF:
            pump_tx_start(&l->tx, s->rate);
            l->tcf_bits_left = (int)(l->cfg.tcf_s * rate_bps(s->rate));
            l->points_rate   = s->rate;
            break;
        case SEG_FAST_DATA:
            pump_tx_start(&l->tx, s->rate);
            hdlc_flags(put_fast, l, 8);
            l->fast_next       = 0;
            l->fast_flags_done = false;
            l->points_rate     = s->rate;
            break;
        }
    }
    bool done = false;
    switch (s->kind) {
    case SEG_SILENCE:
        l->air_s[3] += 1.0 / l->cfg.fs;
        done = --l->seg_left <= 0;
        break;
    case SEG_TONE:
        x = cos(TAU * l->tone_phase);
        l->tone_phase += s->hz / l->cfg.fs;
        l->tone_phase -= floor(l->tone_phase);
        done = --l->seg_left <= 0;
        break;
    case SEG_V21:
        x = v21_tx_sample(&l->v21tx);
        l->air_s[0] += 1.0 / l->cfg.fs;
        if (bitq_len(&l->v21tx.pending) == 0) {
            if (l->seg_left < 0)
                l->seg_left = (int)(l->cfg.fs / V21_BAUD) + 1;
            done = --l->seg_left <= 0;
            if (done)
                v21_tx_set(&l->v21tx, false);
        }
        break;
    case SEG_FAST_TCF:
        while (l->tcf_bits_left > 0 && pump_tx_pending(&l->tx) < 1024) {
            pump_tx_push(&l->tx, 0);
            if (--l->tcf_bits_left == 0)
                pump_tx_stop(&l->tx);
        }
        x = pump_tx_sample(&l->tx);
        l->air_s[1] += 1.0 / l->cfg.fs;
        done = !pump_tx_busy(&l->tx);
        break;
    case SEG_FAST_DATA:
        top_up_data(l);
        x = pump_tx_sample(&l->tx);
        if (l->now - l->keyed_at < (uint64_t)((l->cfg.lead_s + pump_train_seconds(s->rate)) * l->cfg.fs))
            l->air_s[1] += 1.0 / l->cfg.fs;
        else
            l->air_s[2] += 1.0 / l->cfg.fs;
        done = !pump_tx_busy(&l->tx);
        break;
    }
    if (s->kind == SEG_FAST_TCF || s->kind == SEG_FAST_DATA) {
        cplx p = l->tx.rate >= RATE_7200 ? l->tx.v29.sent / v29_rms(l->tx.rate == RATE_9600 ? V29_9600 : V29_7200)
                                         : l->tx.v27.sent;
        if (p != l->tx_last_point) {
            l->tx_last_point                       = p;
            l->points[l->npoints++ % LINK_POINTS] = p;
        }
    }
    if (done) {
        l->at++;
        l->seg_started = false;
    }
    return LEVEL * x;
}

/* ---- receiving ---- */

void link_rx_sample(link_t *l, double x) {
    uint8_t f[LINK_FRAME_MAX + 8];

    if (l->keyed || l->now - l->unkeyed_at < (uint64_t)(MUTE_AFTER * l->cfg.fs)) {
        l->quiet_for = 0;
        return;
    }
    /* the line's own level, for when it is quiet enough to answer on */
    double b   = cascade_run(&l->busy_band, x);
    double lev = onepole_run(&l->busy_lev, fabs(b));
    double on  = QUIET_LEVEL * l->busy_floor > QUIET_ABS ? QUIET_LEVEL * l->busy_floor : QUIET_ABS;
    /* not V.21's carrier flag: an absolute level (BinModem's, for a telephone line), which a radio's receiver
     * noise is over all the time */
    bool   busy = (l->fast_armed && l->rx.rx.state == QAM_TRACK) || lev > on;
    /* the quiet line's level: down quickly, up slowly, busy or not (a steady noise is learned) */
    l->busy_floor += (lev - l->busy_floor) / ((lev < l->busy_floor ? 0.2 : 5.0) * l->cfg.fs);
    if (l->busy_floor < FLOOR_MIN)
        l->busy_floor = FLOOR_MIN;
    l->quiet_for = busy ? 0 : l->quiet_for + 1;

    int bit = v21_rx_feed(&l->v21rx, x);
    if (bit >= 0) {
        int n = hdlc_rx_bit(&l->h21, bit, f, sizeof(f));
        if (l->h21.run >= 3)
            l->v21_heard_at = l->now;   /* a V.21 preamble: three flags back to back, which noise seldom makes */
        if (n > 0) {
            /* the far end has said something new: an answer not yet keyed is to what it said before */
            if (l->planned && !l->keyed)
                l->planned = false;
            if (l->role == LINK_SENDER)
                sender_frame(l, f, n);
            else
                receiver_frame(l, f, n);
        }
    }
    if (l->fast_armed) {
        int k = l->rx.rx.k;
        pump_rx_feed(&l->rx, x);
        qam_state_t st = l->rx.rx.state;
        if (st == QAM_TRACK && l->rx.rx.k != k)
            l->points[l->npoints++ % LINK_POINTS] = l->rx.rx.last;
        if (l->rx.rx.fails != l->fast_fails) {
            /* a burst that did not train; V.21 on the line is not one (its tones are in every fast modem's band) */
            l->fast_fails = l->rx.rx.fails;
            if (l->now - l->v21_heard_at < (uint64_t)(0.15 * l->cfg.fs))
                ;
            else if (l->state == L_TCF)
                tcf_verdict(l, false);
            else if (l->state == L_RXDATA)
                block_verdict(l);
        } else if (l->fast_was == QAM_TRACK && st != QAM_TRACK) {
            if (l->state == L_TCF)
                tcf_verdict(l, true);
            else if (l->state == L_RXDATA)
                block_verdict(l);
        }
        l->fast_was = st;
    }
}
