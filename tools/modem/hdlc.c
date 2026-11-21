/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  x6x00-modem: HDLC framing (hdlc.h).
 */
#include "hdlc.h"

#include <string.h>

uint16_t hdlc_fcs(const uint8_t *d, int n) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return (uint16_t)~crc;
}

void hdlc_flags(hdlc_sink put, void *ctx, int n) {
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 8; b++)
            put(ctx, (0x7E >> b) & 1);
}

typedef struct {
    hdlc_sink put;
    void     *ctx;
    int       ones, count;
} stuff_t;

static void stuffed(stuff_t *s, int bit) {
    if (s->put)
        s->put(s->ctx, bit);
    s->count++;
    if (bit) {
        if (++s->ones == 5) {
            if (s->put)
                s->put(s->ctx, 0);
            s->count++;
            s->ones = 0;
        }
    } else
        s->ones = 0;
}

static int frame_out(hdlc_sink put, void *ctx, const uint8_t *d, int n) {
    stuff_t  s   = {put, ctx, 0, 0};
    uint16_t fcs = hdlc_fcs(d, n);
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 8; b++)
            stuffed(&s, (d[i] >> b) & 1);
    for (int b = 0; b < 16; b++)
        stuffed(&s, (fcs >> b) & 1);
    return s.count;
}

void hdlc_frame(hdlc_sink put, void *ctx, const uint8_t *d, int n) {
    frame_out(put, ctx, d, n);
    hdlc_flags(put, ctx, 1);
}

int hdlc_frame_bits(const uint8_t *d, int n) {
    return frame_out(NULL, NULL, d, n);
}

void hdlc_rx_reset(hdlc_rx_t *h) {
    h->nbits = h->ones = 0;
    h->open            = false;
}

int hdlc_rx_bit(hdlc_rx_t *h, int bit, uint8_t *out, int max) {
    int got = 0;

    if (bit) {
        if (++h->ones > 6) {            /* seven ones: an abort, or the line idle */
            h->run   = 0;
            h->open  = false;
            h->nbits = 0;
            return 0;
        }
    } else {
        if (h->ones == 6) {             /* a flag: what came before it, less its own seven bits, a frame */
            int n = h->nbits - 7;
            if (h->open && n >= 24 && n % 8 == 0 && n / 8 <= max + 2) {
                uint8_t buf[HDLC_MAX + 2];
                int     bytes = n / 8;
                memset(buf, 0, (size_t)bytes);
                for (int i = 0; i < n; i++)
                    buf[i / 8] |= (uint8_t)(h->bits[i] << (i % 8));
                uint16_t fcs = (uint16_t)(buf[bytes - 2] | buf[bytes - 1] << 8);
                if (hdlc_fcs(buf, bytes - 2) == fcs) {
                    memcpy(out, buf, (size_t)(bytes - 2));
                    got = bytes - 2;
                    h->good++;
                } else
                    h->bad++;
            } else if (h->open && n >= 24)
                h->bad++;
            h->run   = h->open && h->nbits == 7 ? h->run + 1 : 1;
            h->open  = true;
            h->nbits = 0;
            h->ones  = 0;
            return got;
        }
        if (h->ones == 5) {             /* a stuffed zero */
            h->ones = 0;
            return 0;
        }
        h->ones = 0;
    }
    if (h->open) {
        if (h->nbits < (int)sizeof(h->bits))
            h->bits[h->nbits++] = (uint8_t)bit;
        else {
            h->open  = false;
            h->nbits = 0;
        }
    }
    return 0;
}
