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
/* The context alone (no slots): what the main screen's waterfall needs */
bool gpu_start(void);

/* Slot s's data (0..4): rows of cols levels (a ring of rows, or of columns for a voiceprint) and one trace of cols
 * columns; made again when the size changes */
bool gpu_slot(int s, int cols, int rows);

/* Ramp `which` (0: the pictures' levels, 1: the voiceprint's, 2: the main waterfall's, 3: the radar's afterglow, 4: the
 * classic 3D picture's): 256 colours, 0x00RRGGBB */
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
/* A frame into the primary plane's buffer buf as it is (drm_wf_commit_hook: the main screen's), no flip asked; then
 * gpu_frame_finish: waited for, (x, y, w, h) what a dump reads back */
bool gpu_frame_begin_buf(int buf, bool portrait);
void gpu_frame_finish(int x, int y, int w, int h);
/* Slot s as the main screen's waterfall, ramp pal: its ring of rows, the newest (head) on top */
void gpu_wf(int s, int pal, int x, int y, int w, int h, int head);
/* AIRSPACE's scope (size square at x, y): the ink outside radius r, the afterglow behind the sweep (tenths of a degree
 * clockwise from north) from ramp 3 (its first 64 colours) */
void gpu_radar(int x, int y, int size, float r, float sweep, uint32_t ink);
/* Coloured points (a pixel), lines (both ends lit) and filled triangles in the frame's coordinates, batched and drawn
 * by gpu_prims_flush (clipped to the picture last cleared): triangles first, lines and points over them */
void gpu_point(int x, int y, uint32_t rgb);
void gpu_line(int x0, int y0, int x1, int y1, uint32_t rgb);
void gpu_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb);
void gpu_prims_flush(void);

/* The classic screen's 3D picture (waterfall_view.c): its column records (gpu_d3_size: cols x rows RGBA texels, two
 * rows a trace as fs_d3_src reads them, a row a line of joins), its background (XRGB, uploaded when it changed),
 * then in a frame: the background (also the clip), the traces in one draw, the lines of joins over them */
typedef struct {
    int   y0, y1;           /* the rows it covers in the picture, from its top */
    int   row;              /* its first record row */
    float foot_px, foot_frac, per_px;
    float fog, fill_fog;    /* 256ths: the ridge's, the fill's */
} gpu_d3_trace_t;
bool gpu_d3_size(int cols, int rows);
void gpu_d3_cols(int row0, int rows, const uint8_t *rgba);
bool gpu_d3_bg(const uint32_t *xrgb, int w, int h);
void gpu_d3_draw_bg(int x, int y, int w, int h);
void gpu_d3_traces(int x, int y, int w, const gpu_d3_trace_t *t, int n);
/* Record row `row` (each column's ends, quarter pixels) as a line of rgb at opacity op, rows y0..y1 of the picture */
void gpu_d3_join(int x, int y, int w, int row, int y0, int y1, uint32_t rgb, float op);

#ifdef __cplusplus
}
#endif
