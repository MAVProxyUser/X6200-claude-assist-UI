/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - DMR (ETSI TS 102 361-1, Tier II): the digital two-way radio of ham repeaters (70 cm, 2 m) and of
 *  business and public-service radio (450-470 MHz, 150-174 MHz), decoded from a narrow FM channel's discriminator
 *  (dmr.c). 4800 symbols/s 4FSK (+3 +1 -1 -3: +-1944 and +-648 Hz), two TDMA timeslots of 30 ms. Each burst: 108
 *  bits, 48 of sync or embedded signalling, 108 bits; a repeater's (BS) bursts each after a 24-bit CACH that says the
 *  slot. Voice: a superframe of six bursts (A with its sync, B-F with the embedded signalling: the link control in
 *  four pieces), three AMBE+2 frames of 72 bits each (2450 bit/s speech, 1150 FEC), for a vocoder (a DVSI AMBE-3000
 *  in a USB-3000 here: ambe.c). Data bursts: the voice call's header and terminator (the full link control: who
 *  talks to whom, BPTC(196,96) and RS(12,9)), idle and control bursts.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct dmr_s dmr_t;

/* What a timeslot carries, as it changes */
typedef struct {
    int      slot;              /* 1 or 2, 0 not known (an MS's bursts with no CACH) */
    int      cc;                /* the colour code 0-15, -1 not known */
    bool     voice;             /* a voice call going on */
    bool     lc;                /* src, dst and flco known (a header, an embedded LC, a terminator) */
    int      flco;              /* 0 a group call (dst a talkgroup), 3 a private one (dst a radio) */
    uint32_t src, dst;          /* the radio talking; the talkgroup or the radio talked to */
    bool     bs;                /* a repeater's (base station's) bursts, not a radio's */
} dmr_call_t;

/* frame: the 72 bits of one AMBE+2 frame as sent (the burst's order, MSB first), as a DVSI AMBE-3000 at rate 33 takes
 * them; call: a timeslot's state when it changes (a call starts, its link control comes, it ends) */
typedef void (*dmr_ambe_cb)(void *ctx, int slot, const uint8_t frame[9]);
typedef void (*dmr_call_cb)(void *ctx, const dmr_call_t *call);

/* rate: the discriminator's sample rate (44100 here) */
dmr_t *dmr_new(float rate, dmr_ambe_cb ambe, dmr_call_cb call, void *ctx);
void   dmr_free(dmr_t *d);

/* The channel's FM discriminator, +-1 for +-5 kHz, as it comes */
void dmr_feed(dmr_t *d, const float *x, size_t n);

/* DMR heard: a sync within the last half second */
bool dmr_active(const dmr_t *d);

/* Counts since the start: syncs found, bursts read, full link controls whose check passed, embedded ones */
void dmr_counts(const dmr_t *d, uint32_t *syncs, uint32_t *bursts, uint32_t *lcs, uint32_t *elcs);

/* The coding, for dmr_test.c's transmitter: BPTC(196,96) of 96 bits (one a byte), RS(12,9)'s 3 parity bytes of 9
 * (as sent: the last one first), the embedded LC's 128 bits (one a byte, as the four bursts carry them) of 72 */
void dmr_bptc_encode(const uint8_t in[96], uint8_t out[196]);
void dmr_rs129(const uint8_t lc[9], uint8_t parity[3]);
void dmr_elc_encode(const uint8_t lc[9], uint8_t out[128]);
