/* Host test shim: cw_encoder.c includes LVGL for its log line alone */
#pragma once
#define LV_LOG_USER(...) ((void)0)
