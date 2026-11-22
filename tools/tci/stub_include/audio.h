#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_PLAY_RATE (44100)
void audio_play_wait(void);
void audio_play_flush(void);
bool audio_play_lowlat_begin(void);
void audio_play_lowlat_end(void);
bool audio_play_if_room(int16_t *buf, size_t samples);
void audio_gain_db(int16_t *buf, size_t samples, float gain, int16_t *out);
void audio_gain_db_transition(int16_t *buf, size_t samples, float gain1, float gain2, int16_t *out);
float audio_set_play_vol(float db);
