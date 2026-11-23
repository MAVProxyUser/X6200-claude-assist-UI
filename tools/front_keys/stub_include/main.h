/* tools/front_keys: the MFK encoder, whose press front_keys.c sets */
#pragma once
#include <stdbool.h>
typedef struct {
    bool pressed;
    bool paged;     /* GoldCorpHF's: held while the main dial turned pages (src/mfk_device.h), cleared at a new press */
} encoder_t;
extern encoder_t *mfk;
