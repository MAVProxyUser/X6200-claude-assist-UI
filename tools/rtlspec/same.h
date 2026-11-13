/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - SAME (Specific Area Message Encoding, 47 CFR 11.31): the headers NOAA Weather Radio sends before
 *  each alert and test, decoded from the channel's demodulated audio (same.c). 520.83 baud AFSK, mark 2083.3 Hz,
 *  space 1562.5 Hz, bytes least significant bit first after a preamble of 0xAB; each header three times, a
 *  second apart, then "NNNN" three times at the end of the message.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SAME_MAX 280            /* a header's characters at most, "ZCZC-...-" */

typedef struct same_s same_t;

/* rate: the audio's sample rate (44100 here) */
same_t *same_new(float rate);
void    same_free(same_t *s);

/* Audio in. True when a header is decided (two of its three copies agree, or one alone after 6 s without the
 * others): its text in out ("ZCZC-WXR-TOR-039049+0030-3171500-KILN/NWS-"), or "NNNN" for the end of a message */
bool same_feed(same_t *s, const float *x, size_t n, char *out, size_t out_n);

/* An event code's name ("TOR": "TORNADO WARNING"), NULL when not known */
const char *same_event_name(const char *code);
