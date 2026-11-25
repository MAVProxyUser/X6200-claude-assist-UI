/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - GMRS, the US General Mobile Radio Service (47 CFR Part 95 Subpart E): its 22 channels, the repeater
 *  inputs of 15-22, their bandwidths and power limits, and the CTCSS and DCS codes its radios use. For listening only:
 *  GMRS may be sent on only by radios certified for it (95.1761), and neither the X6100/X6200 (through a transverter)
 *  nor a HackRF or bladeRF is, so the TX checks refuse it (ham_tx_check, tx_ranges_sdr_check: HAM_TX_GMRS). FRS
 *  shares the same 22 channels. tools/gmrs checks the table.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GMRS_CHANNELS   22
#define GMRS_RPT_SHIFT  5000000         /* a repeater's input: its output (15-22) plus 5 MHz */
#define GMRS_CTCSS_N    38              /* privacy codes 1-38: the EIA's 38 CTCSS tones */
#define GMRS_DCS_N      104             /* the 104 standard DCS codes */

typedef struct {
    uint8_t     ch;             /* 1-22 */
    int32_t     hz;             /* the channel: on 15-22 a repeater's output too */
    int32_t     in_hz;          /* its repeater's input (15-22: hz + GMRS_RPT_SHIFT), 0 none */
    int32_t     bw_hz;          /* the authorized bandwidth (95.1773): 20 kHz, 12.5 kHz on 8-14 (narrow FM) */
    int16_t     w10;            /* GMRS's power limit (95.1767), tenths of a watt: 500, 50 on 1-7, 5 on 8-14 */
    const char *name;           /* "GMRS 20" */
    const char *in_name;        /* "GMRS 20 IN", NULL none */
    const char *note;           /* what it is, a few words: "RPT OUT, IN 467.675" */
} gmrs_ch_t;

/* Channel ch (1-22), NULL none */
const gmrs_ch_t *gmrs_ch(int ch);

/* The channel at hz (its own frequency, or with input its repeater input) within tol Hz: its number, 0 none.
 * *input (may be NULL): hz is the repeater input */
int gmrs_find(int64_t hz, int32_t tol, bool *input);

/* hz's name within tol Hz: "GMRS 20", "GMRS 20 IN"; NULL for none */
const char *gmrs_name(int64_t hz, int32_t tol);

/* The channels in the order a scan goes, by frequency (the 462 MHz ones, then the 467 MHz ones, so a receiver moves
 * least): its i-th (0..GMRS_CHANNELS-1), the channel number */
int gmrs_scan_ch(int i);

/* Privacy code n (1-38): its CTCSS tone, tenths of a hertz; 0 none */
int gmrs_ctcss(int n);

/* DCS code i (0..GMRS_DCS_N-1), its three octal digits as written ("023" is 23); 0 none */
int gmrs_dcs(int i);

#ifdef __cplusplus
}
#endif
