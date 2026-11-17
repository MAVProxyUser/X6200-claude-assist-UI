/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - Meteor-M LRPT (Low Rate Picture Transmission): the Russian weather satellites' pictures on 137.100 and
 *  137.900 MHz, decoded from the complex baseband of one channel (lrpt.c). From the bottom up:
 *    - QPSK (Meteor-M2) or offset QPSK (M2-3, M2-4) at 72 k symbols/s, a root raised cosine of roll-off 0.6 (about
 *      120 kHz wide); the "80k" mode 80 k symbols/s, the same data with a convolutional interleaver in it (36
 *      branches, 2048 deep) and a marker byte 0x27 after every 72 bits;
 *    - the newer satellites' bits differentially coded, each rail against its own symbol before;
 *    - the convolutional code of CCSDS, K=7 rate 1/2 (171, 133 octal), decoded by a soft Viterbi;
 *    - 1024-byte frames (CADUs): the sync word 0x1ACFFC1D and 1020 bytes scrambled by the CCSDS sequence: four
 *      Reed-Solomon (255,223) codewords interleaved (CCSDS's code, its bytes in the conventional basis), 892 bytes of
 *      a VCDU;
 *    - in the VCDUs, CCSDS packets (M_PDU), and in those the MSU-MR imager's: APID 64-69 the six channels (64-66
 *      visible and near infrared, 67-69 infrared), each strip of 8 lines in 14 packets of 14 MCUs, every MCU an 8x8
 *      block coded as a JPEG is (the standard Huffman tables, quantization scaled by the packet's quality factor).
 *  A picture of each channel heard: 1568 pixels wide, a row of 8 pixels' strips for each 8 lines.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lrpt_s lrpt_t;

/* The signal: what the satellite sends. AUTO tries each in turn until frames come, then keeps that one */
enum {
    LRPT_AUTO = 0,
    LRPT_72K_OQPSK,             /* Meteor-M2-3, M2-4: offset QPSK, differential */
    LRPT_80K_OQPSK,             /* the same, interleaved: "80k" */
    LRPT_72K_QPSK,              /* Meteor-M2 (2014-2022) */
    LRPT_80K_QPSK,              /* Meteor-M2 interleaved */
    LRPT_MODES
};

#define LRPT_W     1568         /* a picture's width: 196 MCUs of 8 pixels */
#define LRPT_CHANS 6            /* APID 64-69 */

typedef struct {
    int      mode;              /* the signal being tried or decoded (LRPT_72K_OQPSK...) */
    bool     locked;            /* frames coming (a sync word within the last two seconds) */
    float    freq;              /* the carrier's offset from the channel's centre, Hz */
    float    quality;           /* the carrier's lock, 0 (turning, or noise) to 1 (locked, clean) */
    uint32_t frames;            /* CADUs found (their sync) */
    uint32_t frames_ok;         /* of them those whose four RS codewords decoded */
    uint32_t rs_fixed;          /* bytes RS corrected */
    uint32_t packets;           /* imager packets whose MCUs were decoded */
    int      scid;              /* the spacecraft id of the last good frame, -1 none */
    int      rows[LRPT_CHANS];  /* each channel's picture's rows so far (a multiple of 8) */
} lrpt_stat_t;

/* rate: the complex samples' rate (at least 2 a symbol: 160 kS/s; 264600 in the daemon). mode: LRPT_AUTO or one */
lrpt_t *lrpt_new(float rate, int mode);
void    lrpt_free(lrpt_t *l);

/* n complex samples, interleaved (re, im), of the channel mixed to 0 Hz (the carrier within about 15 kHz of it) */
void lrpt_feed(lrpt_t *l, const float *iq, size_t n);

/* Soft bits as the demodulator makes them (one a byte, +127 a sure 0 and -127 a sure 1; I then Q of each symbol), in
 * place of lrpt_feed: everything after the demodulator, for the test. The mode should not be AUTO */
void lrpt_feed_soft(lrpt_t *l, const int8_t *s, size_t n);

void lrpt_stat(const lrpt_t *l, lrpt_stat_t *s);

/* Channel ch (0-5: APID 64-69): its picture so far, LRPT_W wide and *h rows (0 none yet), a row after another; 0 a
 * pixel not received. Valid till the next lrpt_feed */
const uint8_t *lrpt_image(const lrpt_t *l, int ch, int *h);

/* The strips (8 rows each) of channel ch changed since the last call: from and to (inclusive); false none */
bool lrpt_changed(lrpt_t *l, int ch, int *from, int *to);

/* The pictures dropped, and the counts (frames, packets) with them: a new pass */
void lrpt_clear(lrpt_t *l);

/* ---- The layers, for lrpt_test.c's transmitter ---- */

/* The convolutional code: n bits (one a byte, 0 or 1) into 2n, from the state given (the last 6 bits before, the
 * latest the lowest); the state after returned */
int lrpt_conv_encode(const uint8_t *bits, size_t n, uint8_t *out, int state);

/* RS(255,223) of CCSDS, its bytes in the conventional basis (Meteor's) or the dual one (CCSDS's): the 32 check bytes
 * of 223 */
void lrpt_rs_encode(const uint8_t data[223], uint8_t parity[32], bool dual);
/* Corrects a codeword in place: the bytes corrected, -1 too many errors (the codeword then as it was) */
int lrpt_rs_decode(uint8_t cw[255], bool dual);

/* The CCSDS scrambling sequence (x^8+x^7+x^5+x^3+1 from all ones) xored over n bytes */
void lrpt_pn(uint8_t *b, size_t n);

/* The standard JPEG luminance tables MSU-MR codes with: the DC and AC code lengths and values (Annex K.3, K.5), the
 * quantization table (K.1) for quality q, natural order */
void lrpt_jpeg_tables(const uint8_t **dc_bits, const uint8_t **dc_vals, const uint8_t **ac_bits, const uint8_t **ac_vals);
void lrpt_quant(int q, int table[64]);
extern const uint8_t lrpt_zigzag[64];

/* The packet's MCUs decoded (for the test without the rest): 14 blocks of 8x8 pixels from data, n bytes, with
 * quality q, into px (112 wide, 8 rows); the blocks decoded returned */
int lrpt_mcus(const uint8_t *data, size_t n, int q, uint8_t px[8][112]);
