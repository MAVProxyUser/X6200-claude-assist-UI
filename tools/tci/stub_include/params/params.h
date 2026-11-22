#pragma once
/* The settings tci_server.c and tx_hold.c use: TCI audio's learned level (tx_hold.h), the playback volume */
typedef struct {
    float x;
} params_float_t;
typedef struct {
    params_float_t tci_tx_level, play_gain_db_f;
} params_t;
extern params_t params;
void params_float_set(params_float_t *var, float x);
