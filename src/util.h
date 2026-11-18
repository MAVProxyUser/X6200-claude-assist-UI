/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "helpers.h"

#ifdef __cplusplus

#include "vector"


extern "C" {
#endif
#include "radio_types.h"
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <liquid/liquid.h>

/* Macros */

#define ARRAY_SIZE(arr) (sizeof((arr)) / sizeof((arr)[0]))

uint64_t get_time();
void get_time_str(char *str, size_t str_size);

void split_freq(int32_t freq, uint16_t *mhz, uint16_t *khz, uint16_t *hz);
/* x down to a multiple of step (towards 0). step int32_t: the main knob aligns to a whole turn, 100 kHz a detent
 * (#234) and more with acceleration, which a uint16_t cut to step % 65536 */
int32_t align_int(int32_t x, int32_t step);
int32_t limit(int32_t x, int32_t min, int32_t max);
float sqr(float x);
void lpf(float *x, float current, float beta, float initial);
void lpf_block(float *x, float *current, float beta, unsigned int count);

char *util_canonize_callsign(const char *callsign, bool strip_slashes);

void sleep_usec(uint32_t msec);

int32_t util_compare_version(radio_base_ver_t a, radio_base_ver_t b);

/* A heartbeat for x6x00-gui-watch: the file's time set to now (made when missing). ALIVE_RADIO the radio thread's
 * (with the X6200's base keepalive), ALIVE_UI the screen loop's: either still for long while the GUI's process is
 * there is a hung GUI, and the radio is restarted (x6x00-reboot) before the base switches it off */
#define ALIVE_RADIO "/run/x6100_gui.radio"
#define ALIVE_UI    "/run/x6100_gui.ui"
void alive_mark(const char *path);

#ifdef __cplusplus
}
#endif
