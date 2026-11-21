#pragma once
/* The one setting tci_server.c keeps: TCI audio's learned peak on a base without ALC */
typedef struct {
    float x;
} params_float_t;
typedef struct {
    params_float_t tci_tx_level;
} params_t;
extern params_t params;
void params_float_set(params_float_t *var, float x);
