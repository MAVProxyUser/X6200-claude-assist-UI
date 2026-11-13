/*
 * SSTV modes known to the firmware: one table with what the receiver, the
 * transmitter and the app need to know about each of them.
 */
#pragma once

#include <stdbool.h>

#define SSTV_RATE   11025       /* every SSTV function works at this rate */
#define SSTV_MAX_W  640         /* PD120, PD180 */
#define SSTV_MAX_H  496

/* The order is the one of the RX/TX Mode button and of the remote "sstv_tx"
 * value (sstv_mode_cycle[]) */
typedef enum {
    SSTV_UNKNOWN = 0,
    SSTV_ROBOT8,
    SSTV_ROBOT36,
    SSTV_ROBOT72,
    SSTV_MARTIN1,
    SSTV_MARTIN2,
    SSTV_SCOTTIE1,
    SSTV_SCOTTIE2,
    SSTV_SCOTTIE_DX,
    SSTV_PD90,                /* PD: receive only (the ISS sends PD120)          */
    SSTV_PD120,
    SSTV_PD180,
} sstv_mode_t;

typedef struct {
    sstv_mode_t mode;
    int         vis;          /* 7-bit VIS code                                 */
    const char *name;         /* "Robot 8 BW": decoder, saved-picture event      */
    const char *label;        /* "Robot 8": buttons and messages                 */
    int         width, height;
    int         line_rows;    /* picture rows per line sync: 2 for PD, else 1    */
    double      line_ms;      /* nominal line (sync to sync) period used by the sync tracker */
    float       capture_s;    /* audio kept after the VIS (Auto RX), Manual limit */
    unsigned    tx_ms;        /* on-air length after the TX pre-roll, 0: no TX   */
} sstv_mode_info_t;

/* NULL for SSTV_UNKNOWN or anything not in the table */
const sstv_mode_info_t *sstv_mode_info(sstv_mode_t mode);
const sstv_mode_info_t *sstv_mode_by_vis(int vis);

/* Modes we transmit and decode by hand, in button order */
extern const sstv_mode_t sstv_mode_cycle[7];
