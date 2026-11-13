#include "sstv_mode.h"

#include <stddef.h>

/* tx_ms: VIS (910 ms) + lines (+ 9 ms start sync for Scottie), rounded.
 * PD (JL Barber N7CXI, Dayton 2000): one sync for two rows, 20 ms sync, 2.08 ms
 * porch, then Y of the first row, R-Y and B-Y of both, Y of the second; line
 * 22.08 ms + 4 scans of 170.24 (PD90), 121.6 (PD120), 183.04 ms (PD180) */
static const sstv_mode_info_t table[] = {
    { SSTV_ROBOT8,     2, "Robot 8 BW", "Robot 8",    160, 120, 1,   66.667,   8.0f,   8910 },
    { SSTV_ROBOT36,    8, "Robot 36",   "Robot 36",   320, 240, 1,  150.000,  36.5f,  36910 },
    { SSTV_ROBOT72,   12, "Robot 72",   "Robot 72",   320, 240, 1,  300.000,  72.5f,  72910 },
    { SSTV_MARTIN1,   44, "Martin M1",  "Martin M1",  320, 256, 1,  446.446, 115.0f, 115200 },
    { SSTV_MARTIN2,   40, "Martin M2",  "Martin M2",  320, 256, 1,  226.798,  59.0f,  58970 },
    { SSTV_SCOTTIE1,  60, "Scottie S1", "Scottie S1", 320, 256, 1,  428.220, 111.0f, 110543 },
    { SSTV_SCOTTIE2,  56, "Scottie S2", "Scottie S2", 320, 256, 1,  277.692,  72.0f,  72008 },
    { SSTV_SCOTTIE_DX, 76, "Scottie DX", "Scottie DX", 320, 256, 1, 1050.300, 270.0f,     0 },
    { SSTV_PD90,      99, "PD90",       "PD90",       320, 256, 2,  703.040,  91.0f,      0 },
    { SSTV_PD120,     95, "PD120",      "PD120",      640, 496, 2,  508.480, 127.0f,      0 },
    { SSTV_PD180,     96, "PD180",      "PD180",      640, 496, 2,  754.240, 188.0f,      0 },
};

const sstv_mode_t sstv_mode_cycle[7] = {
    SSTV_ROBOT8, SSTV_ROBOT36, SSTV_ROBOT72, SSTV_MARTIN1, SSTV_MARTIN2, SSTV_SCOTTIE1, SSTV_SCOTTIE2,
};

const sstv_mode_info_t *sstv_mode_info(sstv_mode_t mode)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].mode == mode)
            return &table[i];
    return NULL;
}

const sstv_mode_info_t *sstv_mode_by_vis(int vis)
{
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (table[i].vis == vis)
            return &table[i];
    return NULL;
}
