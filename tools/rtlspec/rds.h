/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - RDS (IEC 62106; RBDS in North America, NRSC-4): the data an FM broadcast station sends on a 57 kHz
 *  subcarrier of its multiplex, decoded from a WFM channel's demodulated multiplex (rds.c): the station's code (PI),
 *  its name (PS, 8 characters), its programme type (PTY) and RadioText (RT, up to 64 characters). 1187.5 bit/s,
 *  differentially coded, biphase, BPSK on the suppressed 57 kHz carrier; 26-bit blocks of 16 data bits and a 10-bit
 *  checkword with an offset word that names the block (A, B, C, C', D), four blocks a group.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rds_s rds_t;

typedef struct {
    uint16_t pi;                /* the station's code, 0 until one came */
    int      pty;               /* the programme type 0-31, -1 until one came */
    bool     tp, ta;            /* traffic programme, traffic announcement now */
    char     ps[9];             /* the name, its 8 characters once all four parts came, "" before */
    char     rt[65];            /* RadioText once a whole one came, the spaces after it off, "" before */
    uint32_t groups;            /* groups decoded since the start or rds_reset */
    uint32_t blocks, bad;       /* blocks read while in step, and those of them that failed their check */
} rds_info_t;

/* rate: the multiplex's sample rate, a multiple of 24 x 2375 / 2 (264600 here) */
rds_t *rds_new(float rate);
void   rds_free(rds_t *r);

/* Another station: what came from the last one forgotten */
void rds_reset(rds_t *r);

/* The multiplex in, the FM discriminator's output (+-1 at +-75 kHz). True when what rds_info gives changed */
bool rds_feed(rds_t *r, const float *x, size_t n);
void rds_info(const rds_t *r, rds_info_t *out);

/* RBDS: the call letters a PI code stands for in the USA ("WOSU"), "" when it stands for none */
void rds_call_letters(uint16_t pi, char out[5]);

/* The programme type's name, RBDS's table (North America) or RDS's (elsewhere) */
const char *rds_pty_name(int pty, bool rbds);
