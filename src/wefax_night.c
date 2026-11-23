/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - WeFax charts at night, the setting (wefax_night.h).
 */

#include "wefax_night.h"

#include "cfg/cfg_api.h"
#include "station.h"
#include "theme.h"

#include <time.h>

bool wefax_night(void)
{
    static time_t asked;
    static bool   night;
    int           mode = (int)param_i_get(cfg_wefax_night);
    time_t        now  = time(NULL);

    if (mode != WEFAX_NIGHT_AUTO)
        return wefax_night_at(mode, NULL, now);
    if (asked && now - asked < 60 && now >= asked)
        return night;
    asked = now;
    night = wefax_night_at(mode, station_qth(), now);
    return night;
}

const char *wefax_night_label(void)
{
    switch (param_i_get(cfg_wefax_night)) {
        case WEFAX_NIGHT_ON:  return "Night\nOn";
        case WEFAX_NIGHT_OFF: return "Night\nOff";
        default:              return wefax_night() ? "Night\nAuto (on)" : "Night\nAuto";
    }
}

void wefax_night_next(void)
{
    param_i_set(cfg_wefax_night, (param_i_get(cfg_wefax_night) + 1) % WEFAX_NIGHT_MODES);
}

uint32_t wefax_screen_grey(uint8_t g, bool night)
{
    uint32_t ink = theme_hex(TC_text), paper = theme_hex(TC_bg), out = 0;

    for (int k = 0; k < 3; k++) {
        unsigned i = (ink >> (16 - 8 * k)) & 0xFF, p = (paper >> (16 - 8 * k)) & 0xFF;
        uint8_t  c = (uint8_t)((p * g + i * (255 - g) + 127) / 255);

        out |= (uint32_t)wefax_night_grey(c, night) << (16 - 8 * k);
    }
    return out;
}
