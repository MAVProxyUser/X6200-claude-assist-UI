/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The GPU (the X6200's Mali-400 through Mesa's lima, loaded at run time) drawing the Sentry's
 *  spectrum pictures straight into the display's primary plane (gpu.c)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mesa loaded (libEGL, libGLESv2, libgbm), a context on the GPU's render node, the primary plane's two buffers its
 * render targets. false: no GPU here (an image without Mesa, no planes, a call failing, said once in the log); the
 * caller draws with the CPU. Called again it answers as the first time */
bool gpu_init(int cols, int rows);
bool gpu_ok(void);

/* The 256 colours of the pictures' ramp (0x00RRGGBB) */
void gpu_palette(const uint32_t *pal);

/* Slot s's (0, 1: a picture's half) rows of levels, a ring of `rows` rows of `cols` levels: one row, or all */
void gpu_levels_row(int s, int ring_row, const uint8_t *levels);
void gpu_levels_all(int s, const uint8_t *levels);

/* Slot s's trace for this frame, per column: its line's row (0 top .. 255 bottom of the trace), the line's span
 * to the column before (top, bottom) and the level that colours it */
void gpu_trace_set(int s, const uint8_t (*cols)[4]);

/* A frame: false when it cannot be drawn now (the plane not the Sentry's, portrait); then nothing else is done.
 * Areas are landscape pixels; the picture's whole area is cleared to bg first */
bool gpu_frame_begin(int x, int y, int w, int h, uint32_t bg);
/* Slot s's trace in (x, y, w, h): the grid every 10 dB between bot and top dB, the old tube's scan lines */
void gpu_trace(int s, int x, int y, int w, int h, float bot_db, float top_db, uint32_t grid);
/* Slot s's rows in perspective in (x, y, w, h), as panel_render: the newest at head - 1, count of them */
void gpu_rows(int s, int x, int y, int w, int h, int head, int count);
void gpu_rect(int x, int y, int w, int h, uint32_t rgb);
/* Drawn, waited for, shown at the next commit */
void gpu_frame_end(void);

#ifdef __cplusplus
}
#endif
