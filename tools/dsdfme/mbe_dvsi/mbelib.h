/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - libmbe-dvsi: the calls dsd-fme makes to mbelib, answered by DVSI hardware instead: an AMBE-3000 (a
 *  USB-3000, a ThumbDV, a DV3000) decodes the voice, through the SDR daemon's port (x6x00-rtlspec's ambe.c,
 *  127.0.0.1:2460, as AMBEserver gives it) or straight on its serial port. No software vocoder: the owner's
 *  requirement (2026-11-16, "hard no on the mbelib, USB-3000 is hard requirement"). The error correction dsd-fme asks
 *  for (Golay, Hamming, the scrambling of the second codeword) is done here, written from the frames' structure and
 *  checked against the chip's own frames. Built as libmbe with this header, so dsd-fme builds against it unchanged.
 *
 *  Where: DSD_DVSI in the environment: "host:port" (default 127.0.0.1:2460), or a serial device ("/dev/ttyUSB0",
 *  "/dev/ttyUSB0:230400"; 460800 by default).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* A voice stream's state, as dsd-fme keeps one per slot: here only which stream it is (the chip has one channel) */
typedef struct {
    int stream;
    int pad[63];
} mbe_parms;

void mbe_printVersion(char *str);
void mbe_initMbeParms(mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced);
void mbe_floattoshort(float *float_buf, short *aout_buf);

/* AMBE+2 3600x2450 (DMR, P25 phase 2, NXDN, dPMR, YSF): the FEC frame as dsd-fme lays it out (C0 24, C1 23, C2 11,
 * C3 14 bits), the 49 voice bits out */
int  mbe_eccAmbe3600x2450C0(char ambe_fr[4][24]);
void mbe_demodulateAmbe3600x2450Data(char ambe_fr[4][24]);
int  mbe_eccAmbe3600x2450Data(char ambe_fr[4][24], char *ambe_d);
void mbe_processAmbe2450Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_d[49],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality);
void mbe_processAmbe3600x2450Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_fr[4][24],
                                    char ambe_d[49], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality);

/* IMBE 7200x4400 (P25 phase 1): c0-c3 Golay (23,12), c4-c6 Hamming (15,11), c7 7 bits; the 88 voice bits out */
int  mbe_eccImbe7200x4400C0(char imbe_fr[8][23]);
void mbe_demodulateImbe7200x4400Data(char imbe_fr[8][23]);
int  mbe_eccImbe7200x4400Data(char imbe_fr[8][23], char *imbe_d);
void mbe_processImbe4400Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char imbe_d[88],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality);
void mbe_processImbe7200x4400Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char imbe_fr[8][23],
                                    char imbe_d[88], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality);

/* IMBE 7100x4400 (EDACS ProVoice) */
int  mbe_eccImbe7100x4400C0(char imbe_fr[7][24]);
void mbe_demodulateImbe7100x4400Data(char imbe_fr[7][24]);
int  mbe_eccImbe7100x4400Data(char imbe_fr[7][24], char *imbe_d);
void mbe_convertImbe7100to7200(char *imbe_d);

/* AMBE 3600x2400 (D-STAR) */
void mbe_processAmbe2400Dataf(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_d[49],
                              mbe_parms *cur_mp, mbe_parms *prev_mp, mbe_parms *prev_mp_enhanced, int uvquality);
void mbe_processAmbe3600x2400Framef(float *aout_buf, int *errs, int *errs2, char *err_str, char ambe_fr[4][24],
                                    char ambe_d[49], mbe_parms *cur_mp, mbe_parms *prev_mp,
                                    mbe_parms *prev_mp_enhanced, int uvquality);

#ifdef __cplusplus
}
#endif
