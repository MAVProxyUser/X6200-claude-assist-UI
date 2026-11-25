/* Host test shim for the tune: the LVGL macros tx_player.c and ext_tune.c use */
#pragma once
#define LV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LV_MAX(a, b) ((a) > (b) ? (a) : (b))
#define LV_CLAMP(min, val, max) (LV_MAX(min, (LV_MIN(val, max))))
/* tx_player.c logs where each app TX keys (LV_LOG_USER): nothing here */
#define LV_LOG_USER(...) ((void)0)
