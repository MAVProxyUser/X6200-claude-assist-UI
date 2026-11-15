/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The GPU (the X6200's Mali-400 through Mesa's lima, loaded at run time) drawing the Sentry's
 *  pictures straight into the display's primary plane (gpu.c)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mesa loaded (libEGL, libGLESv2, libgbm), a context on the GPU's render node, the primary plane's two buffers its
 * render targets, slots 0 and 1 (A/B's halves) of cols x rows. false: no GPU here (an image without Mesa, no planes,
 * a call failing, said once in the log); the caller draws with the CPU. Called again it answers as the first time */
bool gpu_init(int cols, int rows);
bool gpu_ok(void);

/* Slot s's data (0..3): rows of cols levels (a ring of rows, or of columns for a voiceprint) and one trace of cols
 * columns; made again when the size changes */
bool gpu_slot(int s, int cols, int rows);

/* Ramp `which` (0: the pictures' levels, 1: the voiceprint's): 256 colours, 0x00RRGGBB */
void gpu_palette(int which, const uint32_t *pal);

/* Slot s's levels: one row of its ring, one column (a voiceprint's), or all of them */
void gpu_levels_row(int s, int ring_row, const uint8_t *levels);
void gpu_levels_col(int s, int col, const uint8_t *levels);
void gpu_levels_all(int s, const uint8_t *levels);

/* Slot s's trace for this frame, per column: its line's row (0 top .. 255 bottom of the trace), the line's span
 * to the column before (top, bottom) and the level that colours it */
void gpu_trace_set(int s, const uint8_t (*cols)[4]);

/* A frame, in landscape (A, B) or portrait (G held up: the screen's own portrait coordinates): false when it cannot
 * be drawn now (the plane not the Sentry's, the display the other way); then nothing else is done. Each picture is
 * cleared first (gpu_clear: also what the trace and the rects are clipped to) */
bool gpu_frame_begin(bool portrait);
void gpu_clear(int x, int y, int w, int h, uint32_t bg);
/* Slot s's trace in (x, y, w, h): the grid every 10 dB between bot and top dB, the old tube's scan lines */
void gpu_trace(int s, int x, int y, int w, int h, float bot_db, float top_db, uint32_t grid, uint32_t bg);
/* Slot s's rows in perspective in (x, y, w, h), as panel_render: the newest at head - 1, count of them (depth at
 * most: the farthest row's depth) */
void gpu_rows(int s, int x, int y, int w, int h, int head, int count, int depth);
/* Slot s's rows flat, a waterfall: band pixels each, the newest on top */
void gpu_flat(int s, int x, int y, int w, int h, int head, int count, int band, uint32_t bg);
/* Slot s as a voiceprint: its columns a ring, the oldest (head) at the left */
void gpu_vp(int s, int x, int y, int w, int h, int head);
void gpu_rect(int x, int y, int w, int h, uint32_t rgb);
/* Drawn, waited for, shown at the next commit; (x, y, w, h) what a screen dump reads back */
void gpu_frame_end(int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif
