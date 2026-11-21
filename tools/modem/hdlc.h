/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: HDLC framing (ISO 13239), as T.30 carries its frames over V.21 and its error-correcting pages
 *  over the fast modems: 0x7E flags, a zero stuffed after five ones, octets least significant bit first, the
 *  16-bit frame check sequence of X.25 (0x8408 reflected, 0xFFFF in, inverted out).
 */
#ifndef X6X00_MODEM_HDLC_H
#define X6X00_MODEM_HDLC_H

#include <stdbool.h>
#include <stdint.h>

#define HDLC_MAX 1100                   /* the longest frame taken, its check sequence included */

typedef void (*hdlc_sink)(void *ctx, int bit);

uint16_t hdlc_fcs(const uint8_t *d, int n);
void     hdlc_flags(hdlc_sink put, void *ctx, int n);
/* the frame, its check sequence and a closing flag */
void     hdlc_frame(hdlc_sink put, void *ctx, const uint8_t *d, int n);
/* bits a frame takes on the line, without its flags, as hdlc_frame stuffs it */
int      hdlc_frame_bits(const uint8_t *d, int n);

typedef struct {
    uint8_t  bits[(HDLC_MAX + 2) * 8 + 8];
    int      nbits, ones;
    bool     open;                      /* a flag seen: what follows is a frame */
    int      run;                       /* flags back to back, as a preamble sends them */
    uint32_t good, bad;                 /* frames with their check right, and wrong */
} hdlc_rx_t;

void hdlc_rx_reset(hdlc_rx_t *h);
/* one bit in; the frame's length (its check removed) into out when a good one closes, else 0 */
int  hdlc_rx_bit(hdlc_rx_t *h, int bit, uint8_t *out, int max);

#endif
