/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: the link between the two radios, after T.30's procedure.
 *
 *  Half duplex, as a fax call is, and for the same reason: one end transmits at a time. The sender says what it
 *  can do and hears what the receiver can (CAP, over V.21). It names a rate (DCS, over V.21) and in the same
 *  transmission sends the training check, 1.5 s of zeros at that rate (TCF). The receiver answers CFR if the
 *  zeros came through, or FTT if they did not, and the sender then steps down a rate. Data goes in blocks of
 *  numbered HDLC frames at the agreed rate, each block one transmission. The receiver answers MCF, or PPR with
 *  the frames it lacks, which are sent again, as T.30 Annex A's error correction does; three PPRs on one block
 *  and the rate steps down. EOP ends it.
 *
 *  Every transmission is planned: key, a lead of silence (the far radio's receiver coming back after its own
 *  transmission, and our own transmitter's ramp), the signals, a tail, unkey. The plan is the only thing that
 *  keys. The receiver is not fed while keyed.
 *
 *  The far end's words are taken from the line only: the link neither knows nor asks what the other radio is
 *  doing. The two never transmit at once because each transmits only in answer to the other, after it has
 *  heard the other's carrier end, or on a timeout long enough to cover the other's longest transmission; the
 *  daemon's arbiter (x6x00-modem.c) holds a second, separate lock on that.
 */
#ifndef X6X00_MODEM_LINK_H
#define X6X00_MODEM_LINK_H

#include "hdlc.h"
#include "pump.h"
#include "v21.h"

#define LINK_POINTS 512
#define LINK_LOG    64
#define LINK_FRAME_MAX 1024

typedef enum { LINK_SENDER, LINK_RECEIVER } link_role_t;

typedef struct {
    double      fs;
    link_role_t role;
    rate_t      max_rate;               /* the fastest this end will offer */
    int         data_bytes;             /* the sender's: how much to send (pseudo-random, seeded) */
    int         frame_bytes;            /* payload of each data frame */
    double      burst_s;                /* how long a data block may be on the air */
    double      lead_s, tail_s;         /* silence after keying and before unkeying */
    double      reply_s;                /* quiet line before an answer is keyed: longer than any gap inside a transmission */
    double      preamble_s;             /* V.21 flags before a frame */
    double      tcf_s;
    bool        both_ways;              /* after the sender's EOP, the roles swap and it runs again */
    int         rounds;                 /* transfers, the roles swapping after each (0: two if both_ways, else one) */
    const char *name;                   /* this radio, for the CAP */
} link_cfg_t;

void link_defaults(link_cfg_t *c, double fs, link_role_t role);

typedef enum {
    L_IDLE, L_CAP, L_TRAIN, L_DATA, L_EOP, L_DONE, L_FAILED,                 /* sender */
    L_LISTEN, L_TCF, L_RXDATA                                                 /* receiver */
} link_state_t;

typedef enum { SEG_SILENCE, SEG_V21, SEG_FAST_TCF, SEG_FAST_DATA } seg_kind_t;

typedef struct {
    seg_kind_t kind;
    int        samples;                 /* SEG_SILENCE */
    int        nframe;                  /* SEG_V21: one frame */
    uint8_t    frame[64];
    rate_t     rate;                    /* the fast ones */
} seg_t;

typedef struct {
    double      t;                      /* seconds since the link began */
    char        text[96];
} link_event_t;

typedef struct link {
    link_cfg_t   cfg;
    link_role_t  role;
    link_state_t state;
    uint64_t     now;                   /* samples since the start */
    uint64_t     deadline;              /* a timeout, 0 for none */

    /* transmitting */
    seg_t     plan[8];
    int       nplan, at;
    bool      planned, keyed, seg_started, plan_reply;
    uint64_t  plan_made;
    uint64_t  keyed_at;
    int       seg_left;
    v21_tx_t  v21tx;
    pump_t    tx;
    int       fast_frames_left;         /* frames still to queue for SEG_FAST_DATA */
    int       fast_next;                /* the next index into the resend list */
    int       tcf_bits_left;
    bool      fast_flags_done;

    /* receiving */
    v21_rx_t    v21rx;
    hdlc_rx_t   h21, hfast;
    pump_t      rx;
    bool        fast_armed, fast_tcf;
    qam_state_t fast_was;
    int         fast_fails;
    uint64_t    unkeyed_at;
    uint32_t    tcf_bits, tcf_ones, tcf_seen;
    cascade_t   busy_band;              /* the line's level in 300-3000 Hz, and when it last was quiet */
    onepole_t   busy_lev;
    double      busy_floor;
    uint64_t    quiet_for;
    uint64_t    v21_heard_at;           /* when V.21 flags were last on the line */

    /* the procedure */
    rate_t   rate, far_max;
    char     far_name[16];
    int      block, block_frames, frames_total, frames_done;
    uint8_t  need[32];                  /* the frames of this block still wanted, a bit each */
    uint8_t  got[32];                   /* the receiver's: the frames of this block it has */
    int      ppr_rounds, tries;
    int      round, rounds;
    bool     resuming;                  /* retrained mid-block: the block's wanted frames kept */
    bool     swap_after_tx;
    int      rx_block, rx_block_frames, expect_block;

    /* what the view shows */
    uint64_t bytes_delivered, frames_resent, frames_bad, ftt_count, ppr_count;
    double   t_start, t_end;
    double   air_s[4];                  /* seconds keyed in: V.21, training, data, silence */
    double   tcf_ber;
    cplx     points[LINK_POINTS];
    uint32_t npoints;                   /* written so far, the ring holds the last LINK_POINTS */
    int      points_rate;               /* the constellation they are on (-1 V.21) */
    cplx     tx_last_point;
    link_event_t log[LINK_LOG];
    uint32_t nlog;
} link_t;

void   link_init(link_t *l, const link_cfg_t *cfg);
void   link_free(link_t *l);
void   link_start(link_t *l);
/* each sample: the transmitter's (0 while not keyed), then the receiver's line */
double link_tx_sample(link_t *l);
void   link_rx_sample(link_t *l, double x);
static inline bool link_keyed(const link_t *l) {
    return l->keyed;
}
const char *link_state_name(link_state_t s);
double      link_seconds(const link_t *l);

#endif
