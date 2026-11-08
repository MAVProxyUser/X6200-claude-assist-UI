/**
 * @file drm.h
 *
 */

#ifndef DRM_H
#define DRM_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#ifndef LV_DRV_NO_CONF
#ifdef LV_CONF_INCLUDE_SIMPLE
#include "lv_drv_conf.h"
#else
#include "../../lv_drv_conf.h"
#endif
#endif

#if USE_DRM

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/**********************
 * GLOBAL PROTOTYPES
 **********************/
/* false: no usable DRM device, nothing was changed on the screen (use fbdev) */
bool drm_init(void);
/* true: the GUI is on the overlay plane and what it leaves transparent shows
 * the primary plane, where the waterfall is drawn (drm_wf_present) */
bool drm_planes(void);
/* LVGL renders portrait 480x800 from now on: areas are turned 180 (the knobs on
 * top), not 90, and the waterfall plane is left alone; false: landscape again */
void drm_set_portrait(bool on);
/* flush_cb, partial rendering: area-sized color_p */
void drm_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p);
/* A strip (waterfall, spectrum) into the primary plane, in the panel's own
 * layout. Portrait panel (BOARD_FB_ROTATE_90): portrait columns x .. x + w - 1,
 * all 800 rows of src (w pixels each, XRGB). Landscape panel: landscape rows
 * x .. x + w - 1, src row-major, w rows of BOARD_DISP_W pixels, XRGB */
void drm_wf_present(const uint32_t *src, int32_t x, int32_t w);
/* The same with each row of src a ring: column x shows src[head], then on.
 * Landscape panel: the rows of src are the ring, row x shows src row head */
void drm_wf_present_ring(const uint32_t *src, int32_t x, int32_t w, int32_t head);
/* The primary plane claimed by one that draws it whole each time (the Sentry's picture), or given back: meanwhile
 * the strips above are not shown nor copied (they present themselves whole again after) */
void drm_wf_claim(bool on);
/* Claimed now: the main screen's waterfall and spectrum need not draw at all meanwhile */
bool drm_wf_claimed(void);
/* Not claimed, landscape: cb(back) draws (the GPU) into the primary plane's back buffer at each drm_commit() that shows
 * it, after the strips presented; drm_wf_want_flip() asks for one when only cb's part changed. drm_wf_forget(x): the
 * strip at x presented no more (the GPU draws it) */
void drm_wf_commit_hook(void (*cb)(int back));
void drm_wf_want_flip(void);
void drm_wf_forget(int32_t x);
/* The area the GPU drew into buffer buf (the frame's own coordinates), what a dump reads back */
void drm_wf_gpu_area(bool portrait, int buf, int32_t x, int32_t y, int32_t w, int32_t h);
/* Claimed: a landscape picture (src packed, w x h, ARGB/XRGB) at landscape lx, ly turned straight into the primary
 * plane's back buffer, shown at the next drm_commit(). false (nothing done): no planes, not claimed, portrait */
bool drm_wf_rotate(const uint32_t *src, int32_t lx, int32_t ly, int32_t w, int32_t h);
/* The GPU's way to the same (gpu.c): the primary plane's two buffers as dma-bufs (fds the caller owns, their pitch),
 * portrait PANEL x PANEL pixels XRGB8888; false without planes or a turned panel */
bool drm_wf_export(int fds[2], uint32_t pitch[2], int32_t *w, int32_t *h);
/* The GUI's layer for the GPU (gpu.c): memory for LVGL's frames as a dma-buf (gui_fd, the caller's; LAND_W x LAND_H
 * ARGB8888 landscape, or the same bytes portrait, PANEL_W wide) and the overlay's two buffers (ov_fds, portrait ARGB);
 * false without /dev/udmabuf (a kernel without CONFIG_UDMABUF), without /mnt/gpu_gui (off by default until tested
 * on a radio) or without planes. Then drm_gui_gpu(draw): drm_flush copies
 * each area into that memory and at a frame's last one calls draw(back, portrait, rects in the frame's coordinates,
 * n) to turn them into the overlay's buffer back; a false from it, or drm_gui_gpu(NULL), and the processor does it
 * again */
bool drm_gui_export(int ov_fds[2], uint32_t ov_pitch[2], int *gui_fd);
void drm_gui_gpu(bool (*draw)(int back, bool portrait, const lv_area_t *rects, int n));
/* Claimed, and the display turned as asked (landscape, or portrait: G held up): the back buffer's index (0, 1),
 * free once the last flip is done (waited for here); -1 not now. Then, drawn and finished, drm_wf_drawn(): shown at
 * the next drm_commit(), the area drawn (in the frame's own coordinates) noted for a dump */
int drm_wf_back(bool portrait);
void drm_wf_drawn(bool portrait, int32_t x, int32_t y, int32_t w, int32_t h);
#if !BOARD_FB_ROTATE_90
/* The same after a ring turn: only rows head .. head + n - 1 (mod w) of src are new, the
 * others as last presented. On a strip plane only those rows are written */
void drm_wf_scroll(const uint32_t *src, int32_t x, int32_t w, int32_t head, int32_t n);
/* A whole picture of strip x, w written straight into its plane's buffer not on screen (no
 * copy from RAM): its rows, *pitch_px pixels apart. NULL without a strip plane: present it
 * from RAM then. Waits for the flip in flight; nothing else may present the strip until
 * drm_strip_back_done(), which shows it at the next commit. The buffer may be written from
 * another thread meanwhile; both calls are the GUI thread's */
uint32_t * drm_strip_back(int32_t x, int32_t w, int32_t *pitch_px);
void drm_strip_back_done(int32_t x, int32_t w);
#endif
/* Shows what was flushed or presented since the last call, at the next vblank:
 * once per main loop, after lv_timer_handler() */
void drm_commit(void);
/* One frame of the panel's mode in ms, rounded up (17 at 60 Hz); 17 before drm_init or when the mode has no rate */
uint32_t drm_frame_ms(void);
/* The screen as /dev/fb0 would hold it (480x800 BGRX) into path; false if not active */
bool drm_dump(const char *path);
/* dst = top (ARGB) over bottom (XRGB), n pixels */
void drm_blend_under(uint32_t *dst, const uint32_t *top, const uint32_t *bottom, int32_t n);
/* The primary plane, portrait 480x800 XRGB (NULL without planes) */
const uint32_t * drm_wf_pixels(void);
/* A landscape 800x480 ARGB copy of the GUI (snapshot, recorder) completed
 * with the waterfall under its transparent parts; nothing without planes */
void drm_compose_landscape(uint32_t *px);

#endif  /*USE_DRM*/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /*DRM_H*/
