/* Host test shim: the LVGL macros tx_player.c uses */
#pragma once
#define LV_MIN(a, b) ((a) < (b) ? (a) : (b))
#define LV_MAX(a, b) ((a) > (b) ? (a) : (b))
/* tx_player.c logs where each app TX keys (LV_LOG_USER): nothing here */
#define LV_LOG_USER(...) ((void)0)
