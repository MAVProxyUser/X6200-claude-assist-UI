/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - ACARS (ARINC 618): the datalink between airliners and ground stations on VHF AM (131.550 MHz and the
 *  others, acars.c), decoded from one channel's AM-demodulated audio. 2400 bit/s MSK on an audio subcarrier: 1200 and
 *  2400 Hz, phase continuous. A message: the pre-key, "+*", SYN SYN, SOH, the mode, the aircraft's registration (7),
 *  an acknowledgement, a label (2), a block id, STX and the text (or none), ETX or ETB, a 16-bit check (BCS) and DEL.
 *  The characters are 7-bit ASCII with odd parity, least significant bit first.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct acars_s acars_t;

typedef struct {
    char mode;                  /* '2', 'X', ... */
    char reg[8];                /* the registration as sent, its leading dots off (".N123UA" -> "N123UA") */
    char ack;                   /* the acknowledged block, NAK (0x15) as '!' */
    char label[3];              /* "H1", "_d", "Q0" ... */
    char block;                 /* '0'-'9' from the aircraft (downlink), a letter from the ground (uplink) */
    char msn[5];                /* a downlink's message number ("M01A"), "" otherwise */
    char flight[7];             /* a downlink's flight ("UA1234"), "" otherwise */
    char text[221];             /* the text, unprintable characters as spaces, "" none */
    bool more;                  /* ETB: more blocks follow */
    int  parity_errs;           /* characters whose parity failed (0 for a message whose check passed) */
    float level;                /* the audio's level while it came, dB, for the list */
} acars_msg_t;

/* rate: the audio's sample rate (44100 here) */
acars_t *acars_new(float rate);
void     acars_free(acars_t *a);

/* The channel's audio in, AM-demodulated with its DC off (about +-1). Each whole message whose check passed is put
 * into out (at most max) and counted in the return */
int acars_feed(acars_t *a, const float *x, size_t n, acars_msg_t *out, int max);

/* Counts since the start: frames whose sync came, and of them those whose check failed */
void acars_counts(const acars_t *a, uint32_t *synced, uint32_t *bad);

/* The 16-bit check of ARINC 618 (CRC-CCITT, reflected, from 0) over n bytes, as acars.c checks it */
uint16_t acars_crc(const uint8_t *b, size_t n);
