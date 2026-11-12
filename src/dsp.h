/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "helpers.h"
#include "board.h"

#ifdef __cplusplus

#include <liquid/liquid.h>

extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
}
#endif

#define AUDIO_DECIM 4
#define WATERFALL_NFFT (RADIO_SAMPLES * 2)
#define SPECTRUM_NFFT BOARD_DISP_W

#ifdef __cplusplus
extern "C" {
#endif

void dsp_init();
void dsp_samples(cfloat *buf_samples, uint16_t size, bool tx, uint32_t base_freq, bool vary_freq, uint8_t fft_dec);
/* A spectrum the base computed itself (hw_pack_t.psd, HW_CAP_PSD), with its S-meter reading */
void dsp_psd(const float *db, uint16_t n, uint32_t span_hz, bool tx, int16_t smeter_dbm);
void dsp_reset();

void dsp_set_waterfall_enabled(bool enabled);
void dsp_set_spectrum_enabled(bool enabled);

void dsp_put_audio_samples(size_t nsamples, int16_t *samples);

/* The latest receive spectrum (WATERFALL_NFFT points in dB, centre and width in Hz; bin i is at
 * centre - width/2 + i * width / n). seq counts the spectra, so a caller sees a new one. Any thread;
 * false before the first */
bool dsp_spectrum_snapshot(float *db, uint16_t n, uint32_t *center_hz, uint32_t *width_hz, uint32_t *seq);

/* The base's own last packet of bins as it came (HW_CAP_PSD bases: the X6200's 512 in dB, 0 dBm = 0), not cut to the
 * zoom: span_hz wide around the tuned frequency (384 kHz with HW_PSD_WIDE), the lowest first, bin n/2 on it; tx while it was on the
 * air. seq counts the packets. Any thread; false before the first, and always on a base without its own spectrum */
bool dsp_psd_raw(float *db, uint16_t cap, uint16_t *n, uint32_t *span_hz, bool *tx, uint32_t *seq);
#ifdef __cplusplus
}
#endif
