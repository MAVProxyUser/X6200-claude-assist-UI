/**
 * @file drm.c
 *
 * DRM/KMS output on two planes:
 * - the overlay plane (ARGB) shows the GUI: LVGL renders landscape 800x480
 *   areas, they are rotated 90 ccw into a cached portrait shadow, and on the
 *   last area of a frame the dirty rectangles are copied to the back buffer;
 * - the primary plane (XRGB) under it shows what the GUI leaves transparent:
 *   the waterfall, drawn there directly (drm_wf_present), not by LVGL.
 * Both are double buffered and shown together with one atomic commit at
 * vblank (drm_commit, from the main loop): no tearing.
 *
 * A back buffer misses the previous frame too (it went to the other buffer),
 * so the rectangles of the previous frame are copied again: only dirty areas
 * are moved, never the whole screen.
 *
 * Without a usable overlay plane the GUI goes on the primary plane alone and
 * drm_planes() is false: the waterfall stays an LVGL image.
 */

/*********************
 *      INCLUDES
 *********************/
#include "drm.h"
#if USE_DRM

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#ifndef BOARD_DISP_W
#include "src/board.h"
#endif

/*********************
 *      DEFINES
 *********************/
#define LAND_W      BOARD_DISP_W    /* LVGL, landscape */
#define LAND_H      BOARD_DISP_H
#if BOARD_FB_ROTATE_90
#define PANEL_W     LAND_H          /* panel, portrait */
#define PANEL_H     LAND_W
#else
#define PANEL_W     LAND_W
#define PANEL_H     LAND_H
#endif
#define MAX_RECTS   128             /* per frame, more: whole screen (the screensaver's rain: two a column) */
#define FLIP_TIMEOUT_MS 100

#define err(msg, ...) fprintf(stderr, "drm: " msg "\n", ##__VA_ARGS__)

/**********************
 *      TYPEDEFS
 **********************/
struct drm_buffer {
    uint32_t    handle;
    uint32_t    pitch;
    uint32_t    size;
    uint32_t    fb_id;
    uint8_t     *map;
};

typedef struct {
    lv_area_t   rects[MAX_RECTS];   /* portrait coordinates */
    uint32_t    cnt;
    bool        full;
} rects_t;

/* A plane with two buffers: back is the one not on screen */
typedef struct {
    uint32_t            id;
    struct drm_buffer   bufs[2];
    int                 back;
    bool                ready;      /* back buffer filled, to be shown */
    /* property ids */
    uint32_t            p_fb, p_crtc, p_sx, p_sy, p_sw, p_sh, p_cx, p_cy, p_cw, p_ch;
} plane_t;

/**********************
 *  STATIC VARIABLES
 **********************/
static int                  fd = -1;
static uint32_t             conn_id, crtc_id;
static int                  crtc_idx;
static drmModeModeInfo      mode;
static plane_t              primary, overlay;
static plane_t              *gui;           /* the plane LVGL draws to */
static bool                 planes;         /* the GUI on the overlay, the waterfall on the primary */
static bool                 crtc_set;
static volatile bool        flip_pending;
static uint32_t             *shadow;        /* portrait copy of the GUI, cached RAM */
static uint32_t             *wf_shadow;     /* portrait copy of the primary plane (screen dump) */
static rects_t              frame[2];       /* this frame, previous frame */
static int                  cur;
static bool                 portrait_mode;  /* LVGL renders 480x800: turned 180, not 90 */
static bool                 wf_claimed;     /* a picture drawn on the primary plane by its owner (drm_wf_rotate) */
static const uint32_t       *wf_src;        /* its last one, landscape: into wf_shadow only for a dump */
static lv_area_t            wf_src_a;
static int                  wf_gpu_buf = -1;    /* the buffer the GPU drew last (drm_wf_drawn) */
static lv_area_t            wf_gpu_p;           /* and where, in the panel's own coordinates */

/**********************
 *   STATIC FUNCTIONS
 **********************/

static bool find_output(void) {
    drmModeRes *res = drmModeGetResources(fd);

    if (!res) {
        return false;
    }

    bool ok = false;

    for (int i = 0; i < res->count_connectors && !ok; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);

        if (!conn) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            int m = 0;

            for (int j = 0; j < conn->count_modes; j++) {
                if (conn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
                    m = j;
                    break;
                }
            }
            mode = conn->modes[m];

            /* CRTC: the one already driving the connector, else the first possible one */
            crtc_id = 0;
            for (int j = 0; j < conn->count_encoders && !crtc_id; j++) {
                drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[j]);

                if (!enc) {
                    continue;
                }
                if (enc->encoder_id == conn->encoder_id && enc->crtc_id) {
                    crtc_id = enc->crtc_id;
                }
                for (int k = 0; k < res->count_crtcs && !crtc_id; k++) {
                    if (enc->possible_crtcs & (1 << k)) {
                        crtc_id = res->crtcs[k];
                    }
                }
                drmModeFreeEncoder(enc);
            }
            for (int k = 0; k < res->count_crtcs; k++) {
                if (res->crtcs[k] == crtc_id) {
                    crtc_idx = k;
                }
            }
            conn_id = conn->connector_id;
            ok = crtc_id != 0;
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return ok;
}

static uint32_t prop_id(drmModeObjectProperties *props, const char *name) {
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        uint32_t           id = 0;

        if (p && strcmp(p->name, name) == 0) {
            id = p->prop_id;
        }
        drmModeFreeProperty(p);
        if (id) {
            return id;
        }
    }
    return 0;
}

/* The first plane of this type on our CRTC that takes the format */
static bool find_plane(plane_t *pl, uint64_t type, uint32_t fourcc, uint32_t skip) {
    drmModePlaneRes *res = drmModeGetPlaneResources(fd);
    bool            ok = false;

    if (!res) {
        return false;
    }
    for (uint32_t i = 0; i < res->count_planes && !ok; i++) {
        drmModePlane            *p = drmModeGetPlane(fd, res->planes[i]);
        drmModeObjectProperties *props;
        bool                    fmt = false;

        if (!p) {
            continue;
        }
        for (uint32_t f = 0; f < p->count_formats; f++) {
            fmt |= p->formats[f] == fourcc;
        }
        props = drmModeObjectGetProperties(fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props && fmt && p->plane_id != skip && (p->possible_crtcs & (1 << crtc_idx))) {
            uint32_t t = prop_id(props, "type");

            for (uint32_t j = 0; j < props->count_props; j++) {
                if (props->props[j] == t && props->prop_values[j] == type) {
                    pl->id = p->plane_id;
                    pl->p_fb = prop_id(props, "FB_ID");
                    pl->p_crtc = prop_id(props, "CRTC_ID");
                    pl->p_sx = prop_id(props, "SRC_X");
                    pl->p_sy = prop_id(props, "SRC_Y");
                    pl->p_sw = prop_id(props, "SRC_W");
                    pl->p_sh = prop_id(props, "SRC_H");
                    pl->p_cx = prop_id(props, "CRTC_X");
                    pl->p_cy = prop_id(props, "CRTC_Y");
                    pl->p_cw = prop_id(props, "CRTC_W");
                    pl->p_ch = prop_id(props, "CRTC_H");
                    ok = pl->p_fb && pl->p_crtc && pl->p_ch;
                }
            }
        }
        drmModeFreeObjectProperties(props);
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(res);
    return ok;
}

static bool alloc_buf(struct drm_buffer *buf, uint32_t fourcc) {
    struct drm_mode_create_dumb creq = { .width = PANEL_W, .height = PANEL_H, .bpp = 32 };
    struct drm_mode_map_dumb    mreq = { 0 };
    uint32_t                    handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        err("create dumb: %s", strerror(errno));
        return false;
    }
    buf->handle = creq.handle;
    buf->pitch = creq.pitch;
    buf->size = creq.size;

    mreq.handle = creq.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        err("map dumb: %s", strerror(errno));
        return false;
    }
    buf->map = mmap(0, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (buf->map == MAP_FAILED) {
        buf->map = NULL;
        err("mmap: %s", strerror(errno));
        return false;
    }
    memset(buf->map, 0, creq.size);

    /* The byte order of lv_color32_t (B, G, R, A) */
    handles[0] = buf->handle;
    pitches[0] = buf->pitch;
    if (drmModeAddFB2(fd, PANEL_W, PANEL_H, fourcc, handles, pitches, offsets, &buf->fb_id, 0)) {
        err("add fb: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool alloc_plane(plane_t *pl, uint32_t fourcc) {
    return alloc_buf(&pl->bufs[0], fourcc) && alloc_buf(&pl->bufs[1], fourcc);
}

static void free_plane(plane_t *pl) {
    for (int i = 0; i < 2; i++) {
        if (pl->bufs[i].map) {
            munmap(pl->bufs[i].map, pl->bufs[i].size);
            pl->bufs[i].map = NULL;
        }
    }
}

static bool open_card(const char *path) {
    uint64_t has_dumb = 0;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    /* drmSetMaster: fails if another process owns the display */
    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) == 0 && has_dumb &&
        drmSetMaster(fd) == 0 && find_output())
    {
        if (mode.hdisplay == PANEL_W && mode.vdisplay == PANEL_H) {
            return true;
        }
        err("%s: mode %ux%u, %ux%u expected", path, mode.hdisplay, mode.vdisplay, PANEL_W, PANEL_H);
    }
    close(fd);
    fd = -1;
    return false;
}

static void page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec, unsigned int tv_usec, void *data) {
    flip_pending = false;
}

static void wait_flip(void) {
    drmEventContext ctx = { .version = 2, .page_flip_handler = page_flip_handler };
    struct pollfd   pfd = { .fd = fd, .events = POLLIN };

    while (flip_pending) {
        if (poll(&pfd, 1, FLIP_TIMEOUT_MS) <= 0) {
            err("page flip timeout");
            flip_pending = false;       /* never hang the GUI on a lost event */
            break;
        }
        drmHandleEvent(fd, &ctx);
    }
}

#if !BOARD_FB_ROTATE_90
/* Landscape area -> shadow at the same coordinates (no rotation) */
static void copy_area(const lv_area_t *a, const uint32_t *src) {
    int32_t w = lv_area_get_width(a);
    int32_t h = lv_area_get_height(a);

    for (int32_t y = 0; y < h; y++) {
        memcpy(shadow + (a->y1 + y) * PANEL_W + a->x1, src + y * w, (size_t) w * 4);
    }
}
#endif

#if BOARD_FB_ROTATE_90
/* Landscape area (w x h pixels, packed) -> portrait shadow, 90 ccw:
 * landscape (x, y) goes to portrait column y, row LAND_W - 1 - x */
static void rotate_area(const lv_area_t *a, const uint32_t *src) {
    int32_t w = lv_area_get_width(a);
    int32_t h = lv_area_get_height(a);
    int32_t x = 0, y = 0;

#ifdef __ARM_NEON
    /* 4x4 blocks: transpose in registers, store 4 portrait rows of 4 pixels */
    for (y = 0; y + 4 <= h; y += 4) {
        for (x = 0; x + 4 <= w; x += 4) {
            const uint32_t *s = src + y * w + x;
            uint32_t *d = shadow + (LAND_W - 1 - (a->x1 + x)) * PANEL_W + a->y1 + y;

            uint32x4x2_t t0 = vtrnq_u32(vld1q_u32(s), vld1q_u32(s + w));
            uint32x4x2_t t1 = vtrnq_u32(vld1q_u32(s + 2 * w), vld1q_u32(s + 3 * w));

            vst1q_u32(d, vcombine_u32(vget_low_u32(t0.val[0]), vget_low_u32(t1.val[0])));
            vst1q_u32(d - PANEL_W, vcombine_u32(vget_low_u32(t0.val[1]), vget_low_u32(t1.val[1])));
            vst1q_u32(d - 2 * PANEL_W, vcombine_u32(vget_high_u32(t0.val[0]), vget_high_u32(t1.val[0])));
            vst1q_u32(d - 3 * PANEL_W, vcombine_u32(vget_high_u32(t0.val[1]), vget_high_u32(t1.val[1])));
        }
        /* right edge of this band */
        for (int32_t yy = y; yy < y + 4; yy++) {
            for (int32_t xx = x; xx < w; xx++) {
                shadow[(LAND_W - 1 - (a->x1 + xx)) * PANEL_W + a->y1 + yy] = src[yy * w + xx];
            }
        }
    }
#endif

    /* bottom rows (all rows without NEON) */
    for (; y < h; y++) {
        for (x = 0; x < w; x++) {
            shadow[(LAND_W - 1 - (a->x1 + x)) * PANEL_W + a->y1 + y] = src[y * w + x];
        }
    }
}
#endif /* BOARD_FB_ROTATE_90 */

#if BOARD_FB_ROTATE_90
/* Landscape area a (src packed, its width) turned into a portrait buffer dst of stride pixels, as rotate_area: 4x4
 * blocks, but the landscape columns outermost, so each of the four portrait rows a block writes is written on in
 * order (the display's buffers are write-combined: scattered writes are slow there) */
static void rotate_into(uint32_t *dst, int32_t stride, const lv_area_t *a, const uint32_t *src) {
    int32_t w = lv_area_get_width(a);
    int32_t h = lv_area_get_height(a);
    int32_t x = 0, y;

#ifdef __ARM_NEON
    for (; x + 4 <= w; x += 4) {
        uint32_t *d0 = dst + (LAND_W - 1 - (a->x1 + x)) * stride + a->y1;

        for (y = 0; y + 4 <= h; y += 4) {
            const uint32_t *s = src + y * w + x;
            uint32_t       *d = d0 + y;

            uint32x4x2_t t0 = vtrnq_u32(vld1q_u32(s), vld1q_u32(s + w));
            uint32x4x2_t t1 = vtrnq_u32(vld1q_u32(s + 2 * w), vld1q_u32(s + 3 * w));

            vst1q_u32(d, vcombine_u32(vget_low_u32(t0.val[0]), vget_low_u32(t1.val[0])));
            vst1q_u32(d - stride, vcombine_u32(vget_low_u32(t0.val[1]), vget_low_u32(t1.val[1])));
            vst1q_u32(d - 2 * stride, vcombine_u32(vget_high_u32(t0.val[0]), vget_high_u32(t1.val[0])));
            vst1q_u32(d - 3 * stride, vcombine_u32(vget_high_u32(t0.val[1]), vget_high_u32(t1.val[1])));
        }
        for (; y < h; y++) {            /* the bottom rows of these four columns */
            for (int32_t xx = x; xx < x + 4; xx++) {
                dst[(LAND_W - 1 - (a->x1 + xx)) * stride + a->y1 + y] = src[y * w + xx];
            }
        }
    }
#endif
    for (; x < w; x++) {                /* the last columns (all without NEON) */
        for (y = 0; y < h; y++) {
            dst[(LAND_W - 1 - (a->x1 + x)) * stride + a->y1 + y] = src[y * w + x];
        }
    }
}
#endif

/* The owner's last picture into wf_shadow, for a dump or a snapshot (not each frame: nothing else reads it): turned
 * from its source, or read back from the buffer the GPU drew (slow, write-combined, but only for a dump) */
static void wf_src_shadow(void) {
#if BOARD_FB_ROTATE_90
    if (wf_claimed && wf_src && wf_shadow) {
        rotate_into(wf_shadow, PANEL_W, &wf_src_a, wf_src);
    } else if (wf_claimed && wf_gpu_buf >= 0 && wf_shadow) {
        const struct drm_buffer *b  = &primary.bufs[wf_gpu_buf];
        int32_t                  px = wf_gpu_p.x1, pw = lv_area_get_width(&wf_gpu_p);

        for (int32_t y = wf_gpu_p.y1; y <= wf_gpu_p.y2; y++) {
            memcpy(wf_shadow + y * PANEL_W + px, b->map + y * b->pitch + px * 4, (size_t) pw * 4);
        }
    }
#endif
}

static void rects_add(rects_t *r, const lv_area_t *portrait) {
    if (r->cnt < MAX_RECTS) {
        r->rects[r->cnt++] = *portrait;
    } else {
        r->full = true;
    }
}

static void copy_rect(struct drm_buffer *buf, const lv_area_t *p) {
    size_t n = lv_area_get_width(p) * 4;

    for (int32_t y = p->y1; y <= p->y2; y++) {
        memcpy(buf->map + y * buf->pitch + p->x1 * 4, shadow + y * PANEL_W + p->x1, n);
    }
}

static void copy_rects(struct drm_buffer *buf, const rects_t *r) {
    static const lv_area_t all = { 0, 0, PANEL_W - 1, PANEL_H - 1 };

    if (r->full) {
        copy_rect(buf, &all);
    } else {
        for (uint32_t i = 0; i < r->cnt; i++) {
            copy_rect(buf, &r->rects[i]);
        }
    }
}

/* The previous frame's rects into the back buffer (it was on screen during that frame), but those this frame's
 * rects cover: copied from the shadow just after, with the newest content, they were copied twice (an area redrawn
 * each frame, A's 3D picture: half the GUI thread's memcpy, the main cost of the Sentry's frames, 2026-11-14) */
static bool rect_covered(const lv_area_t *p, const rects_t *by) {
    if (by->full) {
        return true;
    }
    for (uint32_t i = 0; i < by->cnt; i++) {
        const lv_area_t *c = &by->rects[i];

        if (c->x1 <= p->x1 && c->y1 <= p->y1 && c->x2 >= p->x2 && c->y2 >= p->y2) {
            return true;
        }
    }
    return false;
}

static void copy_rects_but(struct drm_buffer *buf, const rects_t *r, const rects_t *now) {
    static const lv_area_t all = { 0, 0, PANEL_W - 1, PANEL_H - 1 };

    if (now->full) {
        return;
    }
    if (r->full) {
        copy_rect(buf, &all);
        return;
    }
    for (uint32_t i = 0; i < r->cnt; i++) {
        if (!rect_covered(&r->rects[i], now)) {
            copy_rect(buf, &r->rects[i]);
        }
    }
}

static void add_prop(drmModeAtomicReq *req, uint32_t obj, uint32_t prop, uint64_t val) {
    if (prop) {
        drmModeAtomicAddProperty(req, obj, prop, val);
    }
}

/* Full-screen plane on our CRTC (first commit only) */
static void add_plane_setup(drmModeAtomicReq *req, plane_t *pl) {
    add_prop(req, pl->id, pl->p_crtc, crtc_id);
    add_prop(req, pl->id, pl->p_sx, 0);
    add_prop(req, pl->id, pl->p_sy, 0);
    add_prop(req, pl->id, pl->p_sw, (uint64_t) PANEL_W << 16);
    add_prop(req, pl->id, pl->p_sh, (uint64_t) PANEL_H << 16);
    add_prop(req, pl->id, pl->p_cx, 0);
    add_prop(req, pl->id, pl->p_cy, 0);
    add_prop(req, pl->id, pl->p_cw, PANEL_W);
    add_prop(req, pl->id, pl->p_ch, PANEL_H);
}

/* The first frame: modeset with the primary plane (synchronous), then the overlay on */
static int first_commit(void) {
    int ret = drmModeSetCrtc(fd, crtc_id, primary.bufs[primary.back].fb_id, 0, 0, &conn_id, 1, &mode);

    if (ret || !planes) {
        return ret;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();

    add_plane_setup(req, &overlay);
    add_prop(req, overlay.id, overlay.p_fb, overlay.bufs[overlay.back].fb_id);
    ret = drmModeAtomicCommit(fd, req, 0, NULL);
    drmModeAtomicFree(req);
    return ret;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

bool drm_init(void) {
    /* Landscape blit (BOARD_FB_ROTATE_90=0) is only finished for fbdev: the
     * primary-plane waterfall and Walkie's portrait path still assume a
     * portrait panel. Fall back so panel_open() uses fbdev instead. */
#if !BOARD_FB_ROTATE_90
    err("BOARD_FB_ROTATE_90=0 needs fbdev, DRM is unused");
    return false;
#else
    const char *path = getenv("DRM_CARD");

    if (path ? !open_card(path) : !open_card("/dev/dri/card0") && !open_card("/dev/dri/card1")) {
        err("no usable device, fbdev is used");
        return false;
    }

    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    planes = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0 &&
             find_plane(&primary, DRM_PLANE_TYPE_PRIMARY, DRM_FORMAT_XRGB8888, 0) &&
             find_plane(&overlay, DRM_PLANE_TYPE_OVERLAY, DRM_FORMAT_ARGB8888, primary.id);

    shadow = calloc(PANEL_W * PANEL_H, 4);
    wf_shadow = planes ? calloc(PANEL_W * PANEL_H, 4) : NULL;

    if (!shadow || (planes && !wf_shadow) || !alloc_plane(&primary, DRM_FORMAT_XRGB8888) ||
        (planes && !alloc_plane(&overlay, DRM_FORMAT_ARGB8888)))
    {
        err("buffers not allocated, fbdev is used");
        free_plane(&primary);
        free_plane(&overlay);
        free(shadow);
        free(wf_shadow);
        shadow = wf_shadow = NULL;
        close(fd);              /* also frees the dumb buffers and their fbs */
        fd = -1;
        return false;
    }
    gui = planes ? &overlay : &primary;

    /* Both buffers are empty: the first two frames copy the whole screen */
    frame[0].full = true;
    frame[1].full = true;

    fprintf(stderr, "drm: %ux%u, connector %u, crtc %u, %s\n", mode.hdisplay, mode.vdisplay, conn_id, crtc_id,
            planes ? "GUI on the overlay plane, waterfall on the primary" : "one plane");
    return true;
#endif /* BOARD_FB_ROTATE_90 */
}

bool drm_planes(void) {
    return planes;
}

void drm_set_portrait(bool on) {
    portrait_mode = on;
    frame[0].full = true;       /* both buffers get the whole GUI again */
    frame[1].full = true;
}

void drm_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p) {
    if (portrait_mode) {
        /* Turned 180: the panel's native top is the side away from the knobs */
        if (area->x1 >= 0 && area->y1 >= 0 && area->x2 < PANEL_W && area->y2 < PANEL_H) {
            lv_area_t      p = { PANEL_W - 1 - area->x2, PANEL_H - 1 - area->y2,
                                 PANEL_W - 1 - area->x1, PANEL_H - 1 - area->y1 };
            const uint32_t *src = (const uint32_t *) color_p;

            for (int32_t y = area->y1; y <= area->y2; y++) {
                uint32_t *d = shadow + (PANEL_H - 1 - y) * PANEL_W + PANEL_W - 1 - area->x1;

                for (int32_t x = area->x1; x <= area->x2; x++) {
                    *d-- = *src++;
                }
            }
            rects_add(&frame[cur], &p);
        }
    } else if (area->x1 >= 0 && area->y1 >= 0 && area->x2 < LAND_W && area->y2 < LAND_H) {
#if BOARD_FB_ROTATE_90
        lv_area_t p = { area->y1, LAND_W - 1 - area->x2, area->y2, LAND_W - 1 - area->x1 };

        rotate_area(area, (const uint32_t *) color_p);
        rects_add(&frame[cur], &p);
#else
        copy_area(area, (const uint32_t *) color_p);
        rects_add(&frame[cur], area);
#endif
    }

    if (lv_disp_flush_is_last(drv)) {
        wait_flip();                    /* the back buffer was on screen until this flip */
        copy_rects_but(&gui->bufs[gui->back], &frame[cur ^ 1], &frame[cur]);
        copy_rects(&gui->bufs[gui->back], &frame[cur]);
        gui->ready = true;
    }
    lv_disp_flush_ready(drv);
}

void drm_wf_present(const uint32_t *src, int32_t x, int32_t w) {
    drm_wf_present_ring(src, x, w, 0);
}

/* The strips presented (waterfall, spectrum) and the buffers that miss their latest
 * columns: those get them from wf_shadow just before they are shown (drm_commit),
 * unless the strip was presented again meanwhile. A range for all of them, copied
 * at the next present, also copied again the strips that present was redrawing. */
#define MAX_STRIPS 4
static struct {
    int32_t x, w;
    bool    stale[2];
} strips[MAX_STRIPS];
static int nstrips;

static void copy_cols(struct drm_buffer *buf, int32_t x, int32_t w) {
    for (int32_t y = 0; y < PANEL_H; y++) {
        memcpy(buf->map + y * buf->pitch + x * 4, wf_shadow + y * PANEL_W + x, w * 4);
    }
}

void drm_wf_present_ring(const uint32_t *src, int32_t x, int32_t w, int32_t head) {
    int i, b;

    /* Portrait: no waterfall on the screen, the primary plane stays black. Claimed: another's picture is there, the
     * main screen's waterfall and spectrum go on under it unseen and are not copied */
    if (!planes || portrait_mode || wf_claimed || x < 0 || w <= 0 || x + w > PANEL_W) {
        return;
    }
    for (int32_t y = 0; y < PANEL_H; y++) {
        const uint32_t *s = src + y * w;
        uint32_t       *d = wf_shadow + y * PANEL_W + x;

        memcpy(d, s + head, (w - head) * 4);
        memcpy(d + w - head, s, head * 4);
    }
    for (i = 0; i < nstrips && strips[i].x != x; i++) {}
    if (i == nstrips) {
        if (nstrips == MAX_STRIPS) {
            i = 0;              /* not expected: the oldest is forgotten */
        } else {
            nstrips++;
        }
        strips[i].x = x;
    }
    strips[i].w = w;
    wait_flip();
    b = primary.back;
    copy_cols(&primary.bufs[b], x, w);
    strips[i].stale[b] = false;
    strips[i].stale[b ^ 1] = true;
    primary.ready = true;
}

void drm_wf_claim(bool on) {
    wf_claimed = on;
    wf_src     = NULL;
    wf_gpu_buf = -1;
    nstrips    = 0;         /* the strips presented before: theirs to present again whole, never copied over it */
}

bool drm_wf_rotate(const uint32_t *src, int32_t lx, int32_t ly, int32_t w, int32_t h) {
#if BOARD_FB_ROTATE_90
    lv_area_t a = { lx, ly, lx + w - 1, ly + h - 1 };

    if (!planes || !wf_claimed || portrait_mode || fd < 0 || w <= 0 || h <= 0 || lx < 0 || ly < 0 ||
        a.x2 >= LAND_W || a.y2 >= LAND_H) {
        return false;
    }
    wait_flip();                        /* the back buffer was on screen until this flip */
    rotate_into((uint32_t *) primary.bufs[primary.back].map, (int32_t) (primary.bufs[primary.back].pitch / 4), &a,
                src);
    wf_src     = src;
    wf_src_a   = a;
    wf_gpu_buf = -1;
    primary.ready = true;
    return true;
#else
    (void) src, (void) lx, (void) ly, (void) w, (void) h;
    return false;
#endif
}

bool drm_wf_export(int fds[2], uint32_t pitch[2], int32_t *w, int32_t *h) {
#if BOARD_FB_ROTATE_90
    if (!planes || fd < 0) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        if (drmPrimeHandleToFD(fd, primary.bufs[i].handle, DRM_CLOEXEC | DRM_RDWR, &fds[i])) {
            err("primary buffer %d not exported: %s", i, strerror(errno));
            if (i) {
                close(fds[0]);
            }
            return false;
        }
        pitch[i] = primary.bufs[i].pitch;
    }
    *w = PANEL_W;
    *h = PANEL_H;
    return true;
#else
    (void) fds, (void) pitch, (void) w, (void) h;
    return false;
#endif
}

int drm_wf_back(bool portrait) {
    if (!planes || !wf_claimed || portrait_mode != portrait || fd < 0) {
        return -1;
    }
    wait_flip();                        /* the back buffer was on screen until this flip */
    return primary.back;
}

void drm_wf_drawn(bool portrait, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_area_t p;

    if (portrait) {                     /* the GUI turned 180 there: x, y the panel's PANEL_W - x, PANEL_H - y */
        p = (lv_area_t){ PANEL_W - x - w, PANEL_H - y - h, PANEL_W - 1 - x, PANEL_H - 1 - y };
    } else {                            /* landscape x, y: the panel's row LAND_W - 1 - x, column y */
        p = (lv_area_t){ y, LAND_W - x - w, y + h - 1, LAND_W - 1 - x };
    }
    wf_src     = NULL;
    wf_gpu_buf = primary.back;
    if (_lv_area_intersect(&wf_gpu_p, &p, &(lv_area_t){ 0, 0, PANEL_W - 1, PANEL_H - 1 })) {
        primary.ready = true;
    } else {
        wf_gpu_buf = -1;
    }
}

uint32_t drm_frame_ms(void) {
    return mode.vrefresh ? (1000 + mode.vrefresh - 1) / mode.vrefresh : 17;
}

void drm_commit(void) {
    static bool failed;
    int         ret;

    if (fd < 0 || !(gui->ready || (planes && primary.ready))) {
        return;
    }
    wait_flip();
    if (planes && primary.ready) {
        for (int i = 0; i < nstrips; i++) {
            if (strips[i].stale[primary.back]) {
                copy_cols(&primary.bufs[primary.back], strips[i].x, strips[i].w);
                strips[i].stale[primary.back] = false;
            }
        }
    }

    if (!crtc_set) {
        ret = first_commit();
        crtc_set = ret == 0;
    } else if (planes) {
        drmModeAtomicReq *req = drmModeAtomicAlloc();

        if (overlay.ready) {
            add_prop(req, overlay.id, overlay.p_fb, overlay.bufs[overlay.back].fb_id);
        }
        if (primary.ready) {
            add_prop(req, primary.id, primary.p_fb, primary.bufs[primary.back].fb_id);
        }
        /* shown at the next vblank, the event clears flip_pending */
        ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL);
        drmModeAtomicFree(req);
        flip_pending = ret == 0;
    } else {
        ret = drmModePageFlip(fd, crtc_id, primary.bufs[primary.back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, NULL);
        flip_pending = ret == 0;
    }

    if (ret) {
        if (!failed) {
            err("%s: %s", crtc_set ? "commit" : "set crtc", strerror(errno));
        }
        failed = true;
        /* Not shown, buffers not swapped: both get the whole GUI once commits work again */
        frame[0].full = true;
        frame[1].full = true;
        return;
    }
    failed = false;

    if (gui->ready) {
        gui->ready = false;
        gui->back ^= 1;
        /* this frame becomes the previous one */
        cur ^= 1;
        frame[cur].cnt = 0;
        frame[cur].full = false;
    }
    if (planes && primary.ready) {
        primary.ready = false;
        primary.back ^= 1;
    }
}

bool drm_dump(const char *path) {
    FILE *f;
    bool  ok;

    if (!shadow || !(f = fopen(path, "wb"))) {
        return false;
    }
    if (planes) {
        /* What the panel shows: the GUI over the waterfall */
        static uint32_t row[PANEL_W];

        wf_src_shadow();
        ok = true;
        for (int32_t y = 0; y < PANEL_H && ok; y++) {
            drm_blend_under(row, shadow + y * PANEL_W, wf_shadow + y * PANEL_W, PANEL_W);
            ok = fwrite(row, 4, PANEL_W, f) == PANEL_W;
        }
    } else {
        ok = fwrite(shadow, 4, PANEL_W * PANEL_H, f) == PANEL_W * PANEL_H;
    }
    return fclose(f) == 0 && ok;
}

void drm_blend_under(uint32_t *dst, const uint32_t *top, const uint32_t *bottom, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        uint32_t t = top[i], b = bottom[i];
        uint32_t a = t >> 24;

        if (a == 255) {
            dst[i] = t;
        } else if (a == 0) {
            dst[i] = b | 0xff000000;
        } else {
            uint32_t r = (((t >> 16) & 0xff) * a + ((b >> 16) & 0xff) * (255 - a) + 127) / 255;
            uint32_t g = (((t >> 8) & 0xff) * a + ((b >> 8) & 0xff) * (255 - a) + 127) / 255;
            uint32_t bl = ((t & 0xff) * a + (b & 0xff) * (255 - a) + 127) / 255;

            dst[i] = 0xff000000 | (r << 16) | (g << 8) | bl;
        }
    }
}

const uint32_t * drm_wf_pixels(void) {
    wf_src_shadow();
    return planes ? wf_shadow : NULL;
}

void drm_compose_landscape(uint32_t *px) {
    if (!planes) {
        return;
    }
    wf_src_shadow();
    for (int32_t y = 0; y < LAND_H; y++) {
        uint32_t *row = px + y * LAND_W;
        uint32_t under[LAND_W];

#if BOARD_FB_ROTATE_90
        for (int32_t x = 0; x < LAND_W; x++) {
            under[x] = wf_shadow[(LAND_W - 1 - x) * PANEL_W + y];
        }
#else
        memcpy(under, wf_shadow + y * PANEL_W, (size_t) LAND_W * 4);
#endif
        drm_blend_under(row, row, under, LAND_W);
    }
}

#endif
