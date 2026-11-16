/* Host test shim of src/cfg/cfg_api.h: the parameters the network's rigctld reads and sets (rigctld_test.c keeps them) */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct ParamInt         ParamInt;
typedef struct ComputedParamInt ComputedParamInt;
int32_t param_i_get(const ParamInt *p);
void    param_i_set(ParamInt *p, int32_t v);
int32_t cparam_i_get(const ComputedParamInt *p);
void    cparam_i_set(ComputedParamInt *p, int32_t value);
bool    cfg_is_valid_hw_freq(int32_t freq);
extern ParamInt *cfg_volume, *cfg_rfgain, *cfg_squelch, *cfg_band_current_vfo;
extern ComputedParamInt *cfg_fg_freq, *cfg_cur_mode, *cfg_cur_filter_low, *cfg_cur_filter_high, *cfg_cur_pre,
    *cfg_cur_att;
