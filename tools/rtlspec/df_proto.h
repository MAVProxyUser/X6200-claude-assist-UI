/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - what x6x00-df (the direction finder, tools/rtlspec/df.c) and the GUI's page I (src/df_rx.c,
 *  src/app_df.c) say to each other, on the abstract SOCK_SEQPACKET socket "@x6x00-df" (one client at a time).
 *
 *  Daemon to GUI: binary packets, each one of the structs below, its first 4 bytes the tag. Both ends are the
 *  radio's ARM build, so the structs go as they are (packed, little-endian, every field fixed-size).
 *    DFST  the state, the settings, every channel's calibration      on any change and twice a second
 *    DFDO  one estimate: bearing, its quality, every channel live    every block (10 a second)
 *    DFSP  CH0's spectrum across the whole passband                  every block
 *
 *  GUI to daemon: one text command per packet, ASCII, no newline needed:
 *    f HZ                  the tuners' centre
 *    t OFFSET_HZ BW_HZ     the target: offset from the centre (signed) and its bandwidth
 *    s RATE                sample rate (250000, 1024000, 2048000, 2400000)
 *    g TENTHS              tuner gain in tenths of a dB (the R820T's steps), the same on every channel
 *    a KIND N SIZE_MM      the array: KIND uca|ula, N 2-8, the circle's radius or the line's spacing in mm
 *    a custom N X0 Y0 ...  every element's x (forward) and y (left) in mm, ANT 0 first
 *    m METHOD              0 MUSIC, 1 Bartlett, 2 Capon, 3 MEM
 *    q TENTHS              the squelch: the target this many tenths of a dB over the floor
 *    i SECONDS             calibrate this often (in a gap in the signal), 0 only when asked
 *    x MASK                channels the user leaves out of the bearing (bit m: channel m)
 *    c                     calibrate now
 *    Q                     end (the sticks closed, the noise source off)
 */

#pragma once

#include <stdint.h>

#define DF_SOCK          "x6x00-df"     /* abstract: "\0x6x00-df" */
#define DF_PROTO_VER     1
#define DF_PROTO_MAXCH   8
#define DF_PROTO_SPEC    512            /* DFSP bins */

/* what the daemon is doing (DFST.state) */
enum {
    DF_ST_NODEV = 0,    /* no Kraken (no five sticks with serials 1000-1004): waits, looks again each 2 s */
    DF_ST_OPENING,      /* opening the sticks one at a time */
    DF_ST_CAL,          /* noise source on, measuring */
    DF_ST_LISTEN,       /* calibrated, the squelch shut: nothing to point at */
    DF_ST_TRACK,        /* a bearing (the gate open) */
    DF_ST_HOLD,         /* the signal went: the last bearing kept (DFDO.age_ds says how old) */
    DF_ST_SLIP,         /* a channel lost samples: bearings stopped till the next calibration */
    DF_ST_NOCAL,        /* streaming but no good calibration yet (DFST.why says why) */
    DF_ST_ERROR,        /* a stick failed (DFST.why); retried */
};

/* a channel's condition (df_ch_t.cond), the GUI's colour for each antenna */
enum {
    DF_CH_GONE = 0,     /* its stick is not there / not streaming */
    DF_CH_OK,           /* streaming, calibrated, in the bearing */
    DF_CH_QUIET,        /* in the bearing, but the target is under the squelch on it */
    DF_CH_LOW,          /* the target 20 dB or more under the other channels: a dead antenna or cable; left out */
    DF_CH_SLIP,         /* the channel the slip check points at */
    DF_CH_CLIP,         /* clipping (over 1 % of its bytes at 0 or 255): turn the gain down */
    DF_CH_NOCAL,        /* streaming, not calibrated */
    DF_CH_OFF,          /* left out by the user (x MASK) */
};

#pragma pack(push, 1)

typedef struct {
    char     serial[8];         /* "1000" */
    uint8_t  cond;              /* DF_CH_* (as of the last estimate) */
    uint8_t  used;              /* in the last bearing */
    int16_t  cal_k;             /* whole-sample delay against CH0 */
    int16_t  cal_phase_ddeg;    /* phase correction, tenths of a degree (-1800..1800) */
    int16_t  cal_gain_mdb;      /* amplitude correction, hundredths of a dB */
    int16_t  cal_margin_ddb;    /* correlation peak margin, tenths of a dB */
    int16_t  cal_resid_cdeg;    /* phase fit residual, hundredths of a degree */
    int16_t  noise_ddbfs;       /* the noise source's level, tenths of dB full scale */
} df_ch_t;

typedef struct {
    char     tag[4];            /* "DFST" */
    uint8_t  ver;               /* DF_PROTO_VER */
    uint8_t  state;             /* DF_ST_* */
    uint8_t  nch;               /* channels streaming (5 for a Kraken) */
    uint8_t  synth;             /* 1: a synthetic test source, no sticks */
    uint8_t  method;            /* 0-3 */
    uint8_t  arr_kind;          /* 0 UCA, 1 ULA, 2 custom */
    uint8_t  arr_n;             /* elements */
    uint8_t  user_off;          /* the x MASK */
    uint32_t fc_hz;
    int32_t  f_off_hz;
    uint32_t bw_hz;
    uint32_t rate;
    int16_t  gain_tenths;
    int16_t  sql_tenths;
    uint16_t cal_every_s;
    uint16_t cal_age_s;         /* 0xffff: none yet */
    uint32_t cals;              /* calibrations done (good ones) */
    uint32_t cal_fails;
    uint32_t slips;             /* slip events */
    uint32_t overruns;          /* blocks the worker could not keep up with */
    uint16_t arr_size_mm;       /* UCA radius / ULA spacing (0 custom) */
    int16_t  arr_x_mm[DF_PROTO_MAXCH];
    int16_t  arr_y_mm[DF_PROTO_MAXCH];
    uint8_t  cpu_pct;           /* the daemon's share of one core */
    uint8_t  pad[3];
    char     why[64];           /* the last failure, "" none */
    df_ch_t  ch[DF_PROTO_MAXCH];
} df_st_msg_t;

typedef struct {
    char     tag[4];            /* "DFDO" */
    uint32_t seq;
    uint8_t  gate;              /* 1: the bearing counts */
    int8_t   bad_ch;            /* the slip check's channel, -1 none */
    uint8_t  used_mask;         /* channels in this estimate */
    uint8_t  fit_pct;           /* 0-100 */
    int16_t  bearing_ddeg;      /* tenths of a degree 0-3599; the last good one while HOLD; -1 none ever */
    int16_t  mirror_ddeg;       /* a line's other side, -1 none */
    int16_t  uncert_ddeg;       /* +- */
    int16_t  level_ddb;         /* the target over the floor (CH0) */
    int16_t  snr_ddb;
    int16_t  eig_ddb;
    int16_t  papr_ddb;
    uint16_t age_ds;            /* tenths of a second since the bearing was last good (0 now) */
    uint8_t  npeaks;
    uint8_t  pad;
    int16_t  peaks_ddeg[3];     /* other peaks within 6 dB */
    int16_t  ch_ddb[DF_PROTO_MAXCH];    /* the target's power on each channel, tenths of dB over that channel's floor */
    uint8_t  ch_clip_pm[DF_PROTO_MAXCH];/* bytes at 0 or 255, per thousand (255 = 25.5 % or more) */
    uint8_t  p[360];            /* the method's spectrum per degree: 255 at its peak, 0 at 40 dB under or less */
} df_do_msg_t;

typedef struct {
    char     tag[4];            /* "DFSP" */
    uint32_t seq;
    uint32_t fc_hz;
    uint32_t rate;              /* the bins span the rate, centre in the middle */
    int16_t  floor_ddbfs;       /* the median bin, tenths of dBFS */
    uint16_t n;                 /* DF_PROTO_SPEC */
    uint8_t  db[DF_PROTO_SPEC]; /* 0.5 dB a step over floor - 20 dB: v = (dB - floor + 20) * 2, clamped 0-255 */
} df_sp_msg_t;

#pragma pack(pop)
