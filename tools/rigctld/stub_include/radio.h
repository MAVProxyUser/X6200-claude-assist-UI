/* Host test shim of src/radio.h: what the network's rigctld calls */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radio_types.h"
typedef enum { RADIO_RX = 0, RADIO_TX } radio_state_t;
radio_state_t radio_get_state(void);
uint32_t      radio_caps(void);
radio_vfo_t   radio_toggle_vfo(void);
