/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The GPU drawing the Sentry's spectrum pictures (gpu.h).
 *
 *  Mesa (libEGL, libGLESv2, libgbm) is loaded here at run time, not linked: the GUI runs the same on an image
 *  without it, drawing with the CPU. A GLES 2 context with no window (GBM on the render node, surfaceless), the
 *  display's primary plane's two buffers imported as its render targets (dma-buf -> EGLImage -> renderbuffer ->
 *  framebuffer): what it draws is on the panel at the next flip, never copied by the CPU. Landscape coordinates are
 *  turned to the portrait panel by the vertex shader. The pictures are drawn from small textures only: the levels'
 *  rows (a ring: one row uploaded each time one is added), the trace's per column (once a frame), the ramp's 256
 *  colours; the fragment shaders decide each pixel as panel_trace()'s and panel_render()'s loops did on the CPU
 *  (app_sentry.c), which turned them onto the panel after (most of the GUI thread's time on A and B, 2026-11-14).
 */

#include "gpu.h"

#include "lv_drivers/display/drm.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if USE_DRM

/* ---- The few EGL, GLES 2 and GBM names used, declared here: no headers for them in the build's sysroot ---- */

typedef void        *EGLDisplay, *EGLContext, *EGLConfig, *EGLSurface, *EGLImageKHR, *EGLClientBuffer;
typedef int32_t      EGLint;
typedef unsigned int EGLBoolean, EGLenum;
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int          GLint, GLsizei;
typedef float        GLfloat;
typedef unsigned char GLboolean, GLubyte;
typedef char         GLchar;

#define EGL_NONE                      0x3038
#define EGL_EXTENSIONS                0x3055
#define EGL_OPENGL_ES_API             0x30A0
#define EGL_CONTEXT_CLIENT_VERSION    0x3098
#define EGL_RENDERABLE_TYPE           0x3040
#define EGL_OPENGL_ES2_BIT            0x0004
#define EGL_PLATFORM_GBM_KHR          0x31D7
#define EGL_WIDTH                     0x3057
#define EGL_HEIGHT                    0x3056
#define EGL_LINUX_DMA_BUF_EXT         0x3270
#define EGL_LINUX_DRM_FOURCC_EXT      0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT     0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT  0x3274
#define DRM_FOURCC_XRGB8888           0x34325258    /* 'X' 'R' '2' '4' */
#define DRM_FOURCC_ARGB8888           0x34325241    /* 'A' 'R' '2' '4' */

#define GL_FRAMEBUFFER          0x8D40
#define GL_RENDERBUFFER         0x8D41
#define GL_COLOR_ATTACHMENT0    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE_2D           0x0DE1
#define GL_TEXTURE0             0x84C0
#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_NEAREST              0x2600
#define GL_CLAMP_TO_EDGE        0x812F
#define GL_LUMINANCE            0x1909
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401
#define GL_FLOAT                0x1406
#define GL_UNPACK_ALIGNMENT     0x0CF5
#define GL_VERTEX_SHADER        0x8B31
#define GL_FRAGMENT_SHADER      0x8B30
#define GL_COMPILE_STATUS       0x8B81
#define GL_LINK_STATUS          0x8B82
#define GL_TRIANGLE_STRIP       0x0005
#define GL_COLOR_BUFFER_BIT     0x00004000
#define GL_SCISSOR_TEST         0x0C11
#define GL_BLEND                0x0BE2
#define GL_DITHER               0x0BD0
#define GL_RENDERER             0x1F01
#define GL_ARRAY_BUFFER         0x8892

struct gbm_device;

/* ---- Loaded ---- */

static EGLDisplay (*p_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLDisplay (*p_eglGetDisplay)(void *);
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*p_eglBindAPI)(EGLenum);
static const char *(*p_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLint (*p_eglGetError)(void);
static void *(*p_eglGetProcAddress)(const char *);
static EGLImageKHR (*p_eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
static void (*p_glEGLImageTargetRenderbufferStorageOES)(GLenum, void *);
static void (*p_glEGLImageTargetTexture2DOES)(GLenum, void *);
static struct gbm_device *(*p_gbm_create_device)(int);

static void (*p_glGenFramebuffers)(GLsizei, GLuint *);
static void (*p_glBindFramebuffer)(GLenum, GLuint);
static void (*p_glGenRenderbuffers)(GLsizei, GLuint *);
static void (*p_glBindRenderbuffer)(GLenum, GLuint);
static void (*p_glFramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
static GLenum (*p_glCheckFramebufferStatus)(GLenum);
static void (*p_glViewport)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glScissor)(GLint, GLint, GLsizei, GLsizei);
static void (*p_glEnable)(GLenum);
static void (*p_glDisable)(GLenum);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static GLuint (*p_glCreateShader)(GLenum);
static void (*p_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*p_glCompileShader)(GLuint);
static void (*p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void (*p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static GLuint (*p_glCreateProgram)(void);
static void (*p_glAttachShader)(GLuint, GLuint);
static void (*p_glBindAttribLocation)(GLuint, GLuint, const GLchar *);
static void (*p_glLinkProgram)(GLuint);
static void (*p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static void (*p_glUseProgram)(GLuint);
static GLint (*p_glGetUniformLocation)(GLuint, const GLchar *);
static void (*p_glUniform1i)(GLint, GLint);
static void (*p_glUniform1f)(GLint, GLfloat);
static void (*p_glUniform2f)(GLint, GLfloat, GLfloat);
static void (*p_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
static void (*p_glEnableVertexAttribArray)(GLuint);
static void (*p_glDisableVertexAttribArray)(GLuint);
static void (*p_glDrawArrays)(GLenum, GLint, GLsizei);
static void (*p_glFinish)(void);
static void (*p_glGenTextures)(GLsizei, GLuint *);
static void (*p_glDeleteTextures)(GLsizei, const GLuint *);
static void (*p_glBindTexture)(GLenum, GLuint);
static void (*p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
static void (*p_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static void (*p_glTexParameteri)(GLenum, GLenum, GLint);
static void (*p_glActiveTexture)(GLenum);
static void (*p_glPixelStorei)(GLenum, GLint);
static const GLubyte *(*p_glGetString)(GLenum);
static GLenum (*p_glGetError)(void);
static void (*p_glBindBuffer)(GLenum, GLuint);
static void (*p_glBlendFunc)(GLenum, GLenum);

/* ---- State ---- */

#define SLOTS 5                     /* the pictures' data: A/B's halves (0, 1), G's spectrum (2), its voiceprint (3), the
                                     * main screen's waterfall (4) */
#define PALS  5                     /* the ramps: the pictures' levels, the voiceprint's, the main waterfall's, the
                                     * radar's afterglow (AIRSPACE), the classic 3D picture's */
#define PRIMS 8192                  /* points, lines' ends, triangles' corners a frame (each kind) */

enum { P_ROWS, P_TRACE, P_RECT, P_FLAT, P_VP, P_WF, P_RADAR, P_PRIM, P_IMG, P_D3, P_JOIN, P_GUI, P_N };

static int    tried;                /* 0 not yet, 1 working, -1 not here */
static int    pw, ph;               /* the panel, portrait */
static GLuint fbo[2];
static GLuint prog[P_N];
static GLint  u_panel[P_N], u_port[P_N];
static GLint  u_rows_lv, u_rows_pal, u_rows_t, u_rows_fade, u_rows_amp;
static GLint  u_tr_tr, u_tr_pal, u_tr_bg, u_tr_grid, u_tr_h, u_tr_db;
static GLint  u_rect_color;
static GLint  u_fl_lv, u_fl_pal, u_fl_bg, u_fl_band, u_fl_head, u_fl_ring, u_fl_count;
static GLint  u_vp_lv, u_vp_pal, u_vp_head, u_vp_cols;
static GLint  u_wf_lv, u_wf_pal, u_wf_head, u_wf_ring;
static GLint  u_rd_pal, u_rd_ink, u_rd_c, u_rd_r, u_rd_sweep;
static GLint  u_img_img, u_d3_col, u_d3_pal, u_d3_dr, u_jn_col, u_jn_row, u_jn_c;
static GLuint tex_d3col, tex_img;   /* the 3D picture's column records, its background */
static GLint  u_gui_img;
static GLuint ov_fbo[2];            /* the overlay plane's buffers: the GUI's layer (drm_gui_export) */
static GLuint tex_gui[2];           /* LVGL's frame as the GPU reads it: landscape, portrait */
static int    d3col_w, d3col_h, img_w, img_h;
static struct {                     /* a frame's coloured points, lines and triangles (gpu_point, gpu_line, gpu_tri) */
    float   xy[PRIMS * 2];
    uint8_t c[PRIMS * 4];
    int     n;
} prim[3];
static GLuint tex_pal[PALS], tex_lv[SLOTS], tex_tr[SLOTS];
static int    s_cols[SLOTS], s_rows[SLOTS];
static bool   in_frame, portrait;
static int    cx0, cy0, cw0, ch0;   /* the clip: the picture last cleared (gpu_clear) */

static void *sym(void *lib, const char *name) {
    void *p = lib ? dlsym(lib, name) : NULL;

    if (!p && p_eglGetProcAddress) p = p_eglGetProcAddress(name);
    return p;
}

#define LOAD(lib, name) (*(void **)&p_##name = sym(lib, #name))

static bool load(void) {
    void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    void *gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    bool  ok = true;

    if (!egl || !gles || !gbm) {
        fprintf(stderr, "gpu: no Mesa on this image (%s): the pictures drawn by the CPU\n",
                !egl ? "libEGL" : !gles ? "libGLESv2" : "libgbm");
        return false;
    }
    ok &= LOAD(egl, eglGetProcAddress) != NULL;
    ok &= LOAD(egl, eglGetDisplay) != NULL;
    LOAD(egl, eglGetPlatformDisplayEXT);
    ok &= LOAD(egl, eglInitialize) != NULL;
    ok &= LOAD(egl, eglBindAPI) != NULL;
    ok &= LOAD(egl, eglQueryString) != NULL;
    ok &= LOAD(egl, eglChooseConfig) != NULL;
    ok &= LOAD(egl, eglCreateContext) != NULL;
    ok &= LOAD(egl, eglMakeCurrent) != NULL;
    ok &= LOAD(egl, eglGetError) != NULL;
    ok &= LOAD(egl, eglCreateImageKHR) != NULL;
    ok &= LOAD(gles, glEGLImageTargetRenderbufferStorageOES) != NULL;
    LOAD(gles, glEGLImageTargetTexture2DOES);      /* the GUI's layer only: without it the processor turns the GUI */
    ok &= LOAD(gbm, gbm_create_device) != NULL;
    ok &= LOAD(gles, glGenFramebuffers) && LOAD(gles, glBindFramebuffer) && LOAD(gles, glGenRenderbuffers) &&
          LOAD(gles, glBindRenderbuffer) && LOAD(gles, glFramebufferRenderbuffer) &&
          LOAD(gles, glCheckFramebufferStatus) && LOAD(gles, glViewport) && LOAD(gles, glScissor) &&
          LOAD(gles, glEnable) && LOAD(gles, glDisable) && LOAD(gles, glClearColor) && LOAD(gles, glClear) &&
          LOAD(gles, glCreateShader) && LOAD(gles, glShaderSource) && LOAD(gles, glCompileShader) &&
          LOAD(gles, glGetShaderiv) && LOAD(gles, glGetShaderInfoLog) && LOAD(gles, glCreateProgram) &&
          LOAD(gles, glAttachShader) && LOAD(gles, glBindAttribLocation) && LOAD(gles, glLinkProgram) &&
          LOAD(gles, glGetProgramiv) && LOAD(gles, glGetProgramInfoLog) && LOAD(gles, glUseProgram) &&
          LOAD(gles, glGetUniformLocation) && LOAD(gles, glUniform1i) && LOAD(gles, glUniform1f) &&
          LOAD(gles, glUniform2f) && LOAD(gles, glUniform4f) && LOAD(gles, glVertexAttribPointer) &&
          LOAD(gles, glEnableVertexAttribArray) && LOAD(gles, glDisableVertexAttribArray) && LOAD(gles, glDrawArrays) && LOAD(gles, glFinish) &&
          LOAD(gles, glGenTextures) && LOAD(gles, glDeleteTextures) && LOAD(gles, glBindTexture) && LOAD(gles, glTexImage2D) &&
          LOAD(gles, glTexSubImage2D) && LOAD(gles, glTexParameteri) && LOAD(gles, glActiveTexture) &&
          LOAD(gles, glPixelStorei) && LOAD(gles, glGetString) && LOAD(gles, glGetError) &&
          LOAD(gles, glBindBuffer) && LOAD(gles, glBlendFunc);
    if (!ok) fprintf(stderr, "gpu: Mesa without a function used here: the pictures drawn by the CPU\n");
    return ok;
}

/* ---- Shaders ---- */

/* Pixel edges turned onto the portrait panel: landscape x edge E is the panel's row edge LAND_W - E, landscape y
 * its column; portrait (G held up: the GUI turned 180) x, y are the panel's PANEL_W - x, PANEL_H - y. GL's y 0 is
 * the buffer's first row (the panel's top) */
static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_panel;\n"
    "uniform float u_port;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = u_port > 0.5 ? u_panel - a_pos : vec2(a_pos.y, u_panel.y - a_pos.x);\n"
    "    gl_Position = vec4(p / u_panel * 2.0 - 1.0, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "}\n";

/* One row of the 3D picture (panel_render): x across the row's own width (0..1, the source columns), y in pixels
 * down from where a full-scale level's top would be. Covered from its level's top down to the baseline: the top
 * pixel the ramp's colour, the next at 0.8 of it, the rest the half level's colour at 0.45; all faded with depth */
static const char *fs_rows_src =
    "precision mediump float;\n"
    "uniform sampler2D u_lv;\n"
    "uniform sampler2D u_pal;\n"
    "uniform float u_t;\n"
    "uniform float u_fade;\n"
    "uniform float u_amp;\n"
    "varying vec2 v_uv;\n"
    "vec3 pal(float v) { return texture2D(u_pal, vec2(v * 0.99609375 + 0.001953125, 0.5)).rgb; }\n"
    "void main() {\n"
    "    float v = texture2D(u_lv, vec2(v_uv.x, u_t)).r;\n"
    "    float d = v_uv.y - (1.0 - v) * u_amp;\n"
    "    if (d < 0.0) discard;\n"
    "    vec3 c = d < 1.0 ? pal(v) * u_fade : d < 2.0 ? pal(v) * (u_fade * 0.8) : pal(v * 0.5) * (u_fade * 0.45);\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

/* The trace (panel_trace): per column its line's row and its span to the column before (the line drawn there),
 * under it the level's colours, over it the background with the grid's dotted rows every 10 dB; every third row
 * darker, as an old tube's scan lines */
static const char *fs_trace_src =
    "precision mediump float;\n"
    "uniform sampler2D u_tr;\n"
    "uniform sampler2D u_pal;\n"
    "uniform vec4 u_bg;\n"
    "uniform vec4 u_grid;\n"
    "uniform float u_h;\n"
    "uniform vec4 u_db;\n"
    "varying vec2 v_uv;\n"
    "vec3 pal(float v) { return texture2D(u_pal, vec2(v * 0.99609375 + 0.001953125, 0.5)).rgb; }\n"
    "void main() {\n"
    "    vec4 t = texture2D(u_tr, vec2(v_uv.x, 0.5));\n"
    "    float hm = u_h - 1.0;\n"
    "    float row = floor(v_uv.y);\n"
    "    float y = floor(t.r * hm + 0.5), y0 = floor(t.g * hm + 0.5), y1 = floor(t.b * hm + 0.5);\n"
    "    vec3 c;\n"
    "    if (row >= y0 && row <= y1) c = pal(t.a);\n"
    "    else if (row > y) c = row < y + 1.5 ? pal(t.a) * 0.8 : pal(t.a * 0.5) * 0.45;\n"
    "    else {\n"
    "        float db = u_db.x + (hm - row) / hm * (u_db.y - u_db.x);\n"
    "        float st = (u_db.y - u_db.x) / hm;\n"
    "        float near = abs(db - floor(db / 10.0 + 0.5) * 10.0);\n"
    "        float col = floor(v_uv.x * u_db.z);\n"
    "        c = (near < st * 0.5 && mod(col, 3.0) < 0.5) ? u_grid.rgb : u_bg.rgb;\n"
    "    }\n"
    "    if (row >= 2.0 && mod(row, 3.0) > 1.5) c = c * 0.75;\n"
    "    gl_FragColor = vec4(c, 1.0);\n"
    "}\n";

static const char *fs_rect_src =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

/* The rows flat, a waterfall (G's 2D, ws_draw): u_band pixels a row, the newest on top, each its level's colour
 * dimmed by how weak it is; below the rows there are, the background */
static const char *fs_flat_src =
    "precision mediump float;\n"
    "uniform sampler2D u_lv;\n"
    "uniform sampler2D u_pal;\n"
    "uniform vec4 u_bg;\n"
    "uniform float u_band;\n"
    "uniform float u_head;\n"
    "uniform float u_ring;\n"
    "uniform float u_count;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    float r = floor(v_uv.y / u_band);\n"
    "    if (r >= u_count) { gl_FragColor = u_bg; return; }\n"
    "    float ring = mod(u_head - 1.0 - r + u_ring, u_ring);\n"
    "    float v = texture2D(u_lv, vec2(v_uv.x, (ring + 0.5) / u_ring)).r;\n"
    "    vec3 c = texture2D(u_pal, vec2(v * 0.99609375 + 0.001953125, 0.5)).rgb;\n"
    "    gl_FragColor = vec4(c * (0.12 + 0.88 * v), 1.0);\n"
    "}\n";

/* The voiceprint (vp_column): a ring of columns, the oldest at the left from u_head on, each column's rows the
 * levels from the top (the highest frequency) down, coloured by the voiceprint's own ramp */
static const char *fs_vp_src =
    "precision mediump float;\n"
    "uniform sampler2D u_lv;\n"
    "uniform sampler2D u_pal;\n"
    "uniform float u_head;\n"
    "uniform float u_cols;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    float c = mod(u_head + floor(v_uv.x), u_cols);\n"
    "    float v = texture2D(u_lv, vec2((c + 0.5) / u_cols, v_uv.y)).r;\n"
    "    gl_FragColor = vec4(texture2D(u_pal, vec2(v * 0.99609375 + 0.001953125, 0.5)).rgb, 1.0);\n"
    "}\n";

/* The main screen's waterfall (waterfall_view.c, direct mode): a ring of rows, row age (0 the newest, at the top)
 * at ring row head + age, each level its theme's colour */
static const char *fs_wf_src =
    "precision mediump float;\n"
    "uniform sampler2D u_lv;\n"
    "uniform sampler2D u_pal;\n"
    "uniform float u_head;\n"
    "uniform float u_ring;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    float ring = mod(u_head + floor(v_uv.y), u_ring);\n"
    "    float v = texture2D(u_lv, vec2(v_uv.x, (ring + 0.5) / u_ring)).r;\n"
    "    gl_FragColor = vec4(texture2D(u_pal, vec2(v * 0.99609375 + 0.001953125, 0.5)).rgb, 1.0);\n"
    "}\n";

/* AIRSPACE's scope (app_adsb.c draw_scope): outside the outer ring the ink; inside, the afterglow a quarter turn behind
 * the sweep (u_sweep, tenths of a degree clockwise from north), from the glow ramp (64 of the 256) */
static const char *fs_radar_src =
    "precision highp float;\n"
    "uniform sampler2D u_pal;\n"
    "uniform vec4 u_ink;\n"
    "uniform vec2 u_c;\n"
    "uniform float u_r;\n"
    "uniform float u_sweep;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2(v_uv.x - u_c.x, u_c.y - v_uv.y);\n"
    "    if (dot(p, p) > u_r * u_r) { gl_FragColor = u_ink; return; }\n"
    "    float a = mod(floor(degrees(atan(p.x, p.y)) * 10.0 + 3600.5), 3600.0);\n"
    "    float d = mod(u_sweep - a + 3600.0, 3600.0);\n"
    "    if (d >= 900.0) { gl_FragColor = u_ink; return; }\n"
    "    float i = floor((900.0 - d) * 63.0 / 900.0);\n"
    "    gl_FragColor = vec4(texture2D(u_pal, vec2((i + 0.5) / 256.0, 0.5)).rgb, 1.0);\n"
    "}\n";

/* Coloured points, lines and triangles, each corner its colour; points a pixel */
static const char *vs_prim_src =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_col;\n"
    "uniform vec2 u_panel;\n"
    "uniform float u_port;\n"
    "varying vec4 v_col;\n"
    "void main() {\n"
    "    vec2 p = u_port > 0.5 ? u_panel - a_pos : vec2(a_pos.y, u_panel.y - a_pos.x);\n"
    "    gl_Position = vec4(p / u_panel * 2.0 - 1.0, 0.0, 1.0);\n"
    "    gl_PointSize = 1.0;\n"
    "    v_col = a_col;\n"
    "}\n";

static const char *fs_prim_src =
    "precision mediump float;\n"
    "varying vec4 v_col;\n"
    "void main() { gl_FragColor = v_col; }\n";

/* The GUI's layer: LVGL's ARGB frame as it is, alpha too (what the GUI leaves see-through) */
static const char *fs_gui_src =
    "precision mediump float;\n"
    "uniform sampler2D u_img;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = texture2D(u_img, v_uv); }\n";

/* A picture (the 3D background): XRGB in memory, B G R X bytes */
static const char *fs_img_src =
    "precision mediump float;\n"
    "uniform sampler2D u_img;\n"
    "varying vec2 v_uv;\n"
    "void main() { gl_FragColor = vec4(texture2D(u_img, v_uv).bgr, 1.0); }\n";

/* The classic screen's 3D traces (waterfall_view.c d3_trace): what each column of a trace shows was worked out by the
 * CPU (its floating horizon), two texels a column: start, top, bottom (9 bits each: their low bytes, the high bits in
 * A), then the ridge's extent below the top, the level (its colour), the top pixel's coverage and seen. The pixel's
 * colour as d3_trace gives it: the ridge (the level's colour a third to white) at the top, joined to the column
 * before; below it the colour of its height. a_t1: the trace's first record row, its foot (whole, fraction), levels a
 * pixel; a_t2: its fog and the fill's, in 256ths */
static const char *vs_d3_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_t1;\n"
    "attribute vec4 a_t2;\n"
    "uniform vec2 u_panel;\n"
    "uniform float u_port;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_t1;\n"
    "varying vec4 v_t2;\n"
    "void main() {\n"
    "    vec2 p = u_port > 0.5 ? u_panel - a_pos : vec2(a_pos.y, u_panel.y - a_pos.x);\n"
    "    gl_Position = vec4(p / u_panel * 2.0 - 1.0, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "    v_t1 = a_t1;\n"
    "    v_t2 = a_t2;\n"
    "}\n";

static const char *fs_d3_src =
    "precision mediump float;\n"
    "uniform sampler2D u_col;\n"
    "uniform sampler2D u_pal;\n"
    "uniform float u_dr;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_t1;\n"
    "varying vec4 v_t2;\n"
    "vec3 pal(float l) { return floor(texture2D(u_pal, vec2((l + 0.5) / 256.0, 0.5)).rgb * 255.0 + 0.5); }\n"
    "void main() {\n"
    "    vec4 b = floor(texture2D(u_col, vec2(v_uv.x, v_t1.x + u_dr)) * 255.0 + 0.5);\n"
    "    if (b.a < 0.5) discard;\n"
    "    vec4 a = floor(texture2D(u_col, vec2(v_uv.x, v_t1.x)) * 255.0 + 0.5);\n"
    "    float start = a.r + 256.0 * mod(a.a, 2.0);\n"
    "    float top = a.g + 256.0 * mod(floor(a.a * 0.5), 2.0);\n"
    "    float bottom = a.b + 256.0 * floor(a.a * 0.25);\n"
    "    float y = floor(v_uv.y);\n"
    "    if (y < start || y >= bottom) discard;\n"
    "    if (y > top + b.r) {\n"
    "        float l = clamp(floor((max(0.0, v_t1.y - y) + v_t1.z) * v_t1.w), 0.0, 255.0);\n"
    "        gl_FragColor = vec4(floor(pal(l) * v_t2.y) / 255.0, 1.0);\n"
    "        return;\n"
    "    }\n"
    "    vec3 p = pal(b.g);\n"
    "    vec3 r = floor((p + floor((255.0 - p) / 3.0)) * v_t2.x);\n"
    "    gl_FragColor = vec4(r / 255.0, y == top ? b.b / 255.0 : 1.0);\n"
    "}\n";

/* A line of one segment a column (d3_join): each column's ends in quarter pixels (two bytes each), the pixels it
 * covers that much of u_c (its opacity in a) */
static const char *fs_join_src =
    "precision mediump float;\n"
    "uniform sampler2D u_col;\n"
    "uniform float u_row;\n"
    "uniform vec4 u_c;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec4 a = floor(texture2D(u_col, vec2(v_uv.x, u_row)) * 255.0 + 0.5);\n"
    "    float lo = (a.r + 256.0 * a.g) * 0.25, hi = (a.b + 256.0 * a.a) * 0.25;\n"
    "    float y = floor(v_uv.y);\n"
    "    float cov = min(y + 1.0, hi) - max(y, lo);\n"
    "    if (cov <= 0.0) discard;\n"
    "    gl_FragColor = vec4(u_c.rgb, min(cov, 1.0) * u_c.a);\n"
    "}\n";

static GLuint shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    GLint  ok = 0;

    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "gpu: a shader did not compile: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint program_vs(const char *vsrc, const char *fs_src) {
    GLuint vs = shader(GL_VERTEX_SHADER, vsrc), fs = shader(GL_FRAGMENT_SHADER, fs_src), p;
    GLint  ok = 0;

    if (!vs || !fs) return 0;
    p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glBindAttribLocation(p, 0, "a_pos");
    p_glBindAttribLocation(p, 1, "a_uv");
    p_glBindAttribLocation(p, 2, "a_col");
    p_glBindAttribLocation(p, 3, "a_t1");
    p_glBindAttribLocation(p, 4, "a_t2");
    p_glLinkProgram(p);
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        p_glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "gpu: a program did not link: %s\n", log);
        return 0;
    }
    return p;
}

static GLuint texture(int w, int h, GLenum fmt) {
    GLuint t;

    p_glGenTextures(1, &t);
    p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    {
        size_t n   = (size_t)w * h * (fmt == GL_RGBA ? 4 : 1);
        void  *zero = calloc(1, n);
        p_glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, zero);
        free(zero);
    }
    return t;
}

static bool has(const char *list, const char *ext) {
    size_t n = strlen(ext);

    for (const char *p = list; p && (p = strstr(p, ext)); p += n)
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}

static bool gui_draw(int back, bool port, const lv_area_t *r, int n);

/* The GUI's layer (drm_gui_export): the overlay's buffers as render targets, LVGL's memory as two textures (the
 * same bytes landscape and portrait). false: the processor keeps turning the GUI (said why in the log) */
static bool gui_layer(EGLDisplay dpy) {
    int      ov[2] = {-1, -1}, g = -1;
    uint32_t pitch[2];
    bool     ok = false;

    if (!p_glEGLImageTargetTexture2DOES) {
        fprintf(stderr, "gpu: no EGLImage textures: the GUI's layer turned by the processor\n");
        return false;
    }
    if (!drm_gui_export(ov, pitch, &g)) {
        fprintf(stderr, "gpu: no /dev/udmabuf, not switched on (/mnt/gpu_gui) or no planes: the GUI's layer turned by "
                        "the processor\n");
        return false;
    }
    for (int i = 0; i < 2; i++) {       /* the overlay's buffers, portrait ARGB */
        const EGLint attr[] = {EGL_WIDTH, pw, EGL_HEIGHT, ph, EGL_LINUX_DRM_FOURCC_EXT, DRM_FOURCC_ARGB8888,
                               EGL_DMA_BUF_PLANE0_FD_EXT, ov[i], EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                               EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch[i], EGL_NONE};
        EGLImageKHR  img = p_eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
        GLuint       rb;

        if (!img) {
            fprintf(stderr, "gpu: the overlay's buffer %d not imported (0x%x)\n", i, p_eglGetError());
            goto done;
        }
        p_glGenRenderbuffers(1, &rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img);
        p_glGenFramebuffers(1, &ov_fbo[i]);
        p_glBindFramebuffer(GL_FRAMEBUFFER, ov_fbo[i]);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gpu: the overlay's buffer %d cannot be drawn into\n", i);
            goto done;
        }
    }
    for (int i = 0; i < 2; i++) {       /* LVGL's memory: landscape ph x pw, portrait pw x ph */
        int          w = i ? pw : ph, h = i ? ph : pw;
        const EGLint attr[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FOURCC_ARGB8888,
                               EGL_DMA_BUF_PLANE0_FD_EXT, g, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                               EGL_DMA_BUF_PLANE0_PITCH_EXT, w * 4, EGL_NONE};
        EGLImageKHR  img = p_eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, attr);

        if (!img) {
            fprintf(stderr, "gpu: the GUI's memory not imported as a texture (0x%x)\n", p_eglGetError());
            goto done;
        }
        p_glGenTextures(1, &tex_gui[i]);
        p_glBindTexture(GL_TEXTURE_2D, tex_gui[i]);
        p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    ok = !p_glGetError();
    if (!ok) fprintf(stderr, "gpu: GL error importing the GUI's layer: turned by the processor\n");
done:
    for (int i = 0; i < 2; i++)
        if (ov[i] >= 0) close(ov[i]);
    close(g);
    return ok;
}

static bool setup(void) {
    int          fds[2] = {-1, -1}, rfd;
    uint32_t     pitch[2];
    int32_t      w, h;
    EGLDisplay   dpy;
    EGLContext   ctx;
    EGLConfig    cfg = NULL;
    const char  *ext;
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

    /* An empty /mnt/gpu.off (the DATA partition, over SFTP): the processor draws everything, as before Mesa was on
       the image, without taking Mesa off it. For a picture the GPU draws wrongly, or a GUI that stops in Mesa */
    if (access("/mnt/gpu.off", F_OK) == 0) {
        fprintf(stderr, "gpu: /mnt/gpu.off: the pictures drawn by the CPU\n");
        return false;
    }
    if (!load()) return false;
    if (!drm_wf_export(fds, pitch, &w, &h)) {
        fprintf(stderr, "gpu: the display's planes not shared: the pictures drawn by the CPU\n");
        return false;
    }
    pw = w, ph = h;
    if ((rfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC)) < 0) {
        fprintf(stderr, "gpu: no render node: the pictures drawn by the CPU\n");
        goto fail;
    }
    {
        struct gbm_device *gbm = p_gbm_create_device(rfd);

        if (!gbm) goto fail;
        dpy = p_eglGetPlatformDisplayEXT ? p_eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbm, NULL)
                                         : p_eglGetDisplay(gbm);
    }
    if (!dpy || !p_eglInitialize(dpy, NULL, NULL)) {
        fprintf(stderr, "gpu: EGL not started (0x%x)\n", p_eglGetError());
        goto fail;
    }
    ext = p_eglQueryString(dpy, EGL_EXTENSIONS);
    if (!has(ext, "EGL_KHR_surfaceless_context") || !has(ext, "EGL_EXT_image_dma_buf_import")) {
        fprintf(stderr, "gpu: EGL without surfaceless contexts or dma-buf images: the pictures drawn by the CPU\n");
        goto fail;
    }
    p_eglBindAPI(EGL_OPENGL_ES_API);
    if (!has(ext, "EGL_KHR_no_config_context")) {
        static const EGLint want[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLint              n = 0;

        if (!p_eglChooseConfig(dpy, want, &cfg, 1, &n) || n < 1) goto fail;
    }
    ctx = p_eglCreateContext(dpy, cfg, NULL, ctx_attr);
    if (!ctx || !p_eglMakeCurrent(dpy, NULL, NULL, ctx)) {
        fprintf(stderr, "gpu: no GLES 2 context (0x%x)\n", p_eglGetError());
        goto fail;
    }
    for (int i = 0; i < 2; i++) {       /* the plane's buffers, this context's render targets */
        const EGLint attr[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_LINUX_DRM_FOURCC_EXT, DRM_FOURCC_XRGB8888,
                               EGL_DMA_BUF_PLANE0_FD_EXT, fds[i], EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                               EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch[i], EGL_NONE};
        EGLImageKHR  img = p_eglCreateImageKHR(dpy, NULL, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
        GLuint       rb;

        if (!img) {
            fprintf(stderr, "gpu: the plane's buffer %d not imported (0x%x)\n", i, p_eglGetError());
            goto fail;
        }
        p_glGenRenderbuffers(1, &rb);
        p_glBindRenderbuffer(GL_RENDERBUFFER, rb);
        p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img);
        p_glGenFramebuffers(1, &fbo[i]);
        p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gpu: the plane's buffer %d cannot be drawn into\n", i);
            goto fail;
        }
        close(fds[i]);                  /* the image holds its own reference */
        fds[i] = -1;
    }
    {
        const char *src[P_N] = {fs_rows_src, fs_trace_src, fs_rect_src, fs_flat_src, fs_vp_src, fs_wf_src,
                                fs_radar_src, fs_prim_src, fs_img_src, fs_d3_src, fs_join_src, fs_gui_src};

        for (int i = 0; i < P_N; i++) {
            const char *v = i == P_PRIM ? vs_prim_src : i == P_D3 ? vs_d3_src : vs_src;

            if (!(prog[i] = program_vs(v, src[i]))) goto fail;
            u_panel[i] = p_glGetUniformLocation(prog[i], "u_panel");
            u_port[i]  = p_glGetUniformLocation(prog[i], "u_port");
        }
    }
    u_rows_lv    = p_glGetUniformLocation(prog[P_ROWS], "u_lv");
    u_rows_pal   = p_glGetUniformLocation(prog[P_ROWS], "u_pal");
    u_rows_t     = p_glGetUniformLocation(prog[P_ROWS], "u_t");
    u_rows_fade  = p_glGetUniformLocation(prog[P_ROWS], "u_fade");
    u_rows_amp   = p_glGetUniformLocation(prog[P_ROWS], "u_amp");
    u_tr_tr      = p_glGetUniformLocation(prog[P_TRACE], "u_tr");
    u_tr_pal     = p_glGetUniformLocation(prog[P_TRACE], "u_pal");
    u_tr_bg      = p_glGetUniformLocation(prog[P_TRACE], "u_bg");
    u_tr_grid    = p_glGetUniformLocation(prog[P_TRACE], "u_grid");
    u_tr_h       = p_glGetUniformLocation(prog[P_TRACE], "u_h");
    u_tr_db      = p_glGetUniformLocation(prog[P_TRACE], "u_db");
    u_rect_color = p_glGetUniformLocation(prog[P_RECT], "u_color");
    u_fl_lv      = p_glGetUniformLocation(prog[P_FLAT], "u_lv");
    u_fl_pal     = p_glGetUniformLocation(prog[P_FLAT], "u_pal");
    u_fl_bg      = p_glGetUniformLocation(prog[P_FLAT], "u_bg");
    u_fl_band    = p_glGetUniformLocation(prog[P_FLAT], "u_band");
    u_fl_head    = p_glGetUniformLocation(prog[P_FLAT], "u_head");
    u_fl_ring    = p_glGetUniformLocation(prog[P_FLAT], "u_ring");
    u_fl_count   = p_glGetUniformLocation(prog[P_FLAT], "u_count");
    u_vp_lv      = p_glGetUniformLocation(prog[P_VP], "u_lv");
    u_vp_pal     = p_glGetUniformLocation(prog[P_VP], "u_pal");
    u_vp_head    = p_glGetUniformLocation(prog[P_VP], "u_head");
    u_vp_cols    = p_glGetUniformLocation(prog[P_VP], "u_cols");
    u_wf_lv      = p_glGetUniformLocation(prog[P_WF], "u_lv");
    u_wf_pal     = p_glGetUniformLocation(prog[P_WF], "u_pal");
    u_wf_head    = p_glGetUniformLocation(prog[P_WF], "u_head");
    u_wf_ring    = p_glGetUniformLocation(prog[P_WF], "u_ring");
    u_rd_pal     = p_glGetUniformLocation(prog[P_RADAR], "u_pal");
    u_rd_ink     = p_glGetUniformLocation(prog[P_RADAR], "u_ink");
    u_rd_c       = p_glGetUniformLocation(prog[P_RADAR], "u_c");
    u_rd_r       = p_glGetUniformLocation(prog[P_RADAR], "u_r");
    u_rd_sweep   = p_glGetUniformLocation(prog[P_RADAR], "u_sweep");
    u_img_img    = p_glGetUniformLocation(prog[P_IMG], "u_img");
    u_d3_col     = p_glGetUniformLocation(prog[P_D3], "u_col");
    u_d3_pal     = p_glGetUniformLocation(prog[P_D3], "u_pal");
    u_d3_dr      = p_glGetUniformLocation(prog[P_D3], "u_dr");
    u_jn_col     = p_glGetUniformLocation(prog[P_JOIN], "u_col");
    u_jn_row     = p_glGetUniformLocation(prog[P_JOIN], "u_row");
    u_jn_c       = p_glGetUniformLocation(prog[P_JOIN], "u_c");
    u_gui_img    = p_glGetUniformLocation(prog[P_GUI], "u_img");
    for (int i = 0; i < PALS; i++) tex_pal[i] = texture(256, 1, GL_RGBA);
    p_glDisable(GL_DITHER);
    p_glDisable(GL_BLEND);
    p_glBlendFunc(0x0302, 0x0303);     /* GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA: where blending is on */
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);     /* client-side vertex arrays */
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    if (p_glGetError()) {
        fprintf(stderr, "gpu: GL error while setting up: the pictures drawn by the CPU\n");
        goto fail;
    }
    fprintf(stderr, "gpu: %s draws the Sentry's pictures into the display's plane (%dx%d)\n",
            (const char *)p_glGetString(GL_RENDERER), w, h);
    if (gui_layer(dpy)) {
        drm_gui_gpu(gui_draw);
        fprintf(stderr, "gpu: the GUI's layer turned onto the panel by the GPU (udmabuf)\n");
    }
    return true;
fail:
    for (int i = 0; i < 2; i++)
        if (fds[i] >= 0) close(fds[i]);
    return false;
}

static bool slot(int s, int cols, int rows) {
    if (tried <= 0 || s < 0 || s >= SLOTS || cols < 1 || rows < 1) return false;
    if (s_cols[s] == cols && s_rows[s] == rows) return true;
    if (tex_lv[s]) {
        p_glDeleteTextures(1, &tex_lv[s]);
        p_glDeleteTextures(1, &tex_tr[s]);
    }
    tex_lv[s] = texture(cols, rows, GL_LUMINANCE);
    tex_tr[s] = texture(cols, 1, GL_RGBA);
    s_cols[s] = cols, s_rows[s] = rows;
    return true;
}

bool gpu_start(void) {
    if (!tried) tried = setup() ? 1 : -1;
    return tried > 0;
}

bool gpu_init(int cols, int rows) {
    return gpu_start() && slot(0, cols, rows) && slot(1, cols, rows);
}

bool gpu_ok(void) {
    return tried > 0;
}

bool gpu_slot(int s, int cols, int rows) {
    return slot(s, cols, rows);
}

void gpu_palette(int which, const uint32_t *pal) {
    uint8_t px[256 * 4];

    if (tried <= 0 || which < 0 || which >= PALS) return;
    for (int i = 0; i < 256; i++) {
        px[i * 4]     = (uint8_t)(pal[i] >> 16);
        px[i * 4 + 1] = (uint8_t)(pal[i] >> 8);
        px[i * 4 + 2] = (uint8_t)pal[i];
        px[i * 4 + 3] = 255;
    }
    p_glBindTexture(GL_TEXTURE_2D, tex_pal[which]);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static bool slot_ok(int s) {
    return tried > 0 && s >= 0 && s < SLOTS && tex_lv[s];
}

void gpu_levels_row(int s, int ring_row, const uint8_t *levels) {
    if (!slot_ok(s) || ring_row < 0 || ring_row >= s_rows[s]) return;
    p_glBindTexture(GL_TEXTURE_2D, tex_lv[s]);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, ring_row, s_cols[s], 1, GL_LUMINANCE, GL_UNSIGNED_BYTE, levels);
}

void gpu_levels_col(int s, int col, const uint8_t *levels) {
    if (!slot_ok(s) || col < 0 || col >= s_cols[s]) return;
    p_glBindTexture(GL_TEXTURE_2D, tex_lv[s]);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, col, 0, 1, s_rows[s], GL_LUMINANCE, GL_UNSIGNED_BYTE, levels);
}

void gpu_levels_all(int s, const uint8_t *levels) {
    if (!slot_ok(s)) return;
    p_glBindTexture(GL_TEXTURE_2D, tex_lv[s]);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_cols[s], s_rows[s], GL_LUMINANCE, GL_UNSIGNED_BYTE, levels);
}

void gpu_trace_set(int s, const uint8_t (*cols)[4]) {
    if (!slot_ok(s)) return;
    p_glBindTexture(GL_TEXTURE_2D, tex_tr[s]);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s_cols[s], 1, GL_RGBA, GL_UNSIGNED_BYTE, cols);
}

/* A rectangle of the frame's coordinates as the panel's scissor box: landscape, its columns are y and its rows
 * LAND_W - x; portrait, turned 180 */
static void scissor(int x, int y, int w, int h) {
    if (portrait) p_glScissor(pw - (x + w), ph - (y + h), w, h);
    else p_glScissor(y, ph - (x + w), h, w);
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const float q[16] = {x0, y0, u0, v0, x1, y0, u1, v0, x0, y1, u0, v1, x1, y1, u1, v1};

    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 16, q);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, 0, 16, q + 2);
    p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void rgb4(uint32_t c, float *f) {
    f[0] = ((c >> 16) & 0xff) / 255.0f, f[1] = ((c >> 8) & 0xff) / 255.0f, f[2] = (c & 0xff) / 255.0f;
}

static void use(int i) {
    p_glUseProgram(prog[i]);
    p_glUniform2f(u_panel[i], (float)pw, (float)ph);
    p_glUniform1f(u_port[i], portrait ? 1.0f : 0.0f);
}

static void bind(GLuint data, GLuint pal) {
    p_glActiveTexture(GL_TEXTURE0);
    p_glBindTexture(GL_TEXTURE_2D, data);
    p_glActiveTexture(GL_TEXTURE0 + 1);
    p_glBindTexture(GL_TEXTURE_2D, pal);
}

static bool begin_on(int b, bool port) {
    portrait = port;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[b]);
    p_glViewport(0, 0, pw, ph);
    p_glEnable(GL_SCISSOR_TEST);
    in_frame = true;
    return true;
}

bool gpu_frame_begin(bool port) {
    int b;

    if (tried <= 0 || (b = drm_wf_back(port)) < 0) return false;
    return begin_on(b, port);
}

static int cur_buf = -1;

bool gpu_frame_begin_buf(int buf, bool port) {
    if (tried <= 0 || buf < 0 || buf > 1) return false;
    cur_buf = buf;
    return begin_on(buf, port);
}

void gpu_frame_finish(int x, int y, int w, int h) {
    if (!in_frame) return;
    in_frame = false;
    p_glDisable(GL_SCISSOR_TEST);
    p_glFinish();                       /* drawn before the plane is flipped to it */
    drm_wf_gpu_area(portrait, cur_buf, x, y, w, h);
}

void gpu_wf(int s, int pal, int x, int y, int w, int h, int head) {
    if (!in_frame || !slot_ok(s) || pal < 0 || pal >= PALS) return;
    scissor(x, y, w, h);
    use(P_WF);
    bind(tex_lv[s], tex_pal[pal]);
    p_glUniform1i(u_wf_lv, 0);
    p_glUniform1i(u_wf_pal, 1);
    p_glUniform1f(u_wf_head, (float)head);
    p_glUniform1f(u_wf_ring, (float)s_rows[s]);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 1, (float)h);
}

void gpu_clear(int x, int y, int w, int h, uint32_t bg) {
    float c[3];

    if (!in_frame) return;
    cx0 = x, cy0 = y, cw0 = w, ch0 = h;
    scissor(x, y, w, h);
    rgb4(bg, c);
    p_glClearColor(c[0], c[1], c[2], 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
}

void gpu_trace(int s, int x, int y, int w, int h, float bot_db, float top_db, uint32_t grid, uint32_t bg) {
    float b[3], gr[3];

    if (!in_frame || h < 2 || !slot_ok(s)) return;
    rgb4(bg, b);
    rgb4(grid, gr);
    scissor(cx0, cy0, cw0, ch0);
    use(P_TRACE);
    bind(tex_tr[s], tex_pal[0]);
    p_glUniform1i(u_tr_tr, 0);
    p_glUniform1i(u_tr_pal, 1);
    p_glUniform4f(u_tr_bg, b[0], b[1], b[2], 1);
    p_glUniform4f(u_tr_grid, gr[0], gr[1], gr[2], 1);
    p_glUniform1f(u_tr_h, (float)h);
    p_glUniform4f(u_tr_db, bot_db, top_db, (float)s_cols[s], 0);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 1, (float)h);
}

void gpu_rows(int s, int x, int y, int w, int h, int head, int count, int depth) {
    float cx = w / 2.0f;
    int   ring = slot_ok(s) ? s_rows[s] : 0;

    if (!in_frame || h < 8 || !ring || depth < 2) return;
    if (count > depth) count = depth;
    scissor(x, y, w, h);                /* the back rows' tops clipped at the picture's top, as on the CPU */
    use(P_ROWS);
    bind(tex_lv[s], tex_pal[0]);
    p_glUniform1i(u_rows_lv, 0);
    p_glUniform1i(u_rows_pal, 1);
    for (int r = count - 1; r >= 0; r--) {  /* the oldest first, at the back: the newer over them */
        float t      = (float)r / (depth - 1);
        int   yb     = y + h - 2 - (int)(t * h * 0.6f);
        float shrink = 1.0f - 0.36f * t, amp = h * 0.62f * (1.0f - 0.3f * t), fade = 1.0f - 0.62f * t;
        int   rr     = ((head - 1 - r) % ring + ring) % ring;

        p_glUniform1f(u_rows_t, (rr + 0.5f) / ring);
        p_glUniform1f(u_rows_fade, fade);
        p_glUniform1f(u_rows_amp, amp);
        quad(x + cx - cx * shrink, yb - amp, x + cx + cx * shrink, yb + 1.0f, 0, 0, 1, amp + 1.0f);
    }
}

void gpu_flat(int s, int x, int y, int w, int h, int head, int count, int band, uint32_t bg) {
    float b[3];

    if (!in_frame || h < 1 || band < 1 || !slot_ok(s)) return;
    rgb4(bg, b);
    scissor(x, y, w, h);
    use(P_FLAT);
    bind(tex_lv[s], tex_pal[0]);
    p_glUniform1i(u_fl_lv, 0);
    p_glUniform1i(u_fl_pal, 1);
    p_glUniform4f(u_fl_bg, b[0], b[1], b[2], 1);
    p_glUniform1f(u_fl_band, (float)band);
    p_glUniform1f(u_fl_head, (float)head);
    p_glUniform1f(u_fl_ring, (float)s_rows[s]);
    p_glUniform1f(u_fl_count, (float)(count < h / band ? count : h / band));
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 1, (float)h);
}

void gpu_vp(int s, int x, int y, int w, int h, int head) {
    if (!in_frame || !slot_ok(s)) return;
    scissor(x, y, w, h);
    use(P_VP);
    bind(tex_lv[s], tex_pal[1]);
    p_glUniform1i(u_vp_lv, 0);
    p_glUniform1i(u_vp_pal, 1);
    p_glUniform1f(u_vp_head, (float)head);
    p_glUniform1f(u_vp_cols, (float)s_cols[s]);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, (float)s_cols[s], 1);
}

void gpu_radar(int x, int y, int size, float r, float sweep, uint32_t ink) {
    float c[3];

    if (!in_frame || size < 2) return;
    rgb4(ink, c);
    scissor(x, y, size, size);
    use(P_RADAR);
    bind(tex_pal[3], tex_pal[3]);
    p_glUniform1i(u_rd_pal, 1);
    p_glUniform4f(u_rd_ink, c[0], c[1], c[2], 1);
    p_glUniform2f(u_rd_c, size / 2.0f, size / 2.0f);
    p_glUniform1f(u_rd_r, r);
    p_glUniform1f(u_rd_sweep, sweep);
    quad((float)x, (float)y, (float)(x + size), (float)(y + size), 0, 0, (float)size, (float)size);
}

static void prim_add(int k, float x, float y, uint32_t rgb) {
    int n = prim[k].n;

    if (n >= PRIMS) return;
    prim[k].xy[n * 2]     = x;
    prim[k].xy[n * 2 + 1] = y;
    prim[k].c[n * 4]      = (uint8_t)(rgb >> 16);
    prim[k].c[n * 4 + 1]  = (uint8_t)(rgb >> 8);
    prim[k].c[n * 4 + 2]  = (uint8_t)rgb;
    prim[k].c[n * 4 + 3]  = 255;
    prim[k].n             = n + 1;
}

/* Pixel x, y: its centre (the edges' coordinates + 0.5) */
void gpu_point(int x, int y, uint32_t rgb) {
    if (in_frame) prim_add(0, x + 0.5f, y + 0.5f, rgb);
}

void gpu_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (!in_frame || prim[1].n > PRIMS - 2) return;
    prim_add(1, x0 + 0.5f, y0 + 0.5f, rgb);
    prim_add(1, x1 + 0.5f, y1 + 0.5f, rgb);
    prim_add(0, x1 + 0.5f, y1 + 0.5f, rgb);     /* its last pixel too, as the CPU's line */
}

void gpu_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb) {
    if (!in_frame || prim[2].n > PRIMS - 3) return;
    prim_add(2, x0, y0, rgb);
    prim_add(2, x1, y1, rgb);
    prim_add(2, x2, y2, rgb);
}

void gpu_prims_flush(void) {
    static const GLenum mode[3] = {0x0000, 0x0001, 0x0004};    /* GL_POINTS, GL_LINES, GL_TRIANGLES */

    if (!in_frame) return;
    scissor(cx0, cy0, cw0, ch0);
    use(P_PRIM);
    p_glDisableVertexAttribArray(1);
    p_glEnableVertexAttribArray(2);
    for (int k = 2; k >= 0; k--) {      /* the filled shapes first, the lines and points over them */
        if (!prim[k].n) continue;
        p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, prim[k].xy);
        p_glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, 0, prim[k].c);
        p_glDrawArrays(mode[k], 0, prim[k].n);
        prim[k].n = 0;
    }
    p_glDisableVertexAttribArray(2);
    p_glEnableVertexAttribArray(1);
}

/* ---- The classic screen's 3D picture ---- */

bool gpu_d3_size(int cols, int rows) {
    if (tried <= 0 || cols < 1 || rows < 1) return false;
    if (d3col_w == cols && d3col_h == rows) return true;
    if (tex_d3col) p_glDeleteTextures(1, &tex_d3col);
    tex_d3col = texture(cols, rows, GL_RGBA);
    d3col_w = cols, d3col_h = rows;
    return tex_d3col != 0;
}

void gpu_d3_cols(int row0, int rows, const uint8_t *rgba) {
    if (!tex_d3col || row0 < 0 || rows < 1 || row0 + rows > d3col_h) return;
    p_glBindTexture(GL_TEXTURE_2D, tex_d3col);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row0, d3col_w, rows, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

bool gpu_d3_bg(const uint32_t *xrgb, int w, int h) {
    if (tried <= 0 || w < 1 || h < 1) return false;
    if (img_w != w || img_h != h) {
        if (tex_img) p_glDeleteTextures(1, &tex_img);
        tex_img = texture(w, h, GL_RGBA);
        img_w = w, img_h = h;
    }
    p_glBindTexture(GL_TEXTURE_2D, tex_img);
    p_glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, xrgb);
    return true;
}

void gpu_d3_draw_bg(int x, int y, int w, int h) {
    if (!in_frame || !tex_img) return;
    cx0 = x, cy0 = y, cw0 = w, ch0 = h;
    scissor(x, y, w, h);
    use(P_IMG);
    bind(tex_img, tex_img);
    p_glUniform1i(u_img_img, 0);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, (float)w / img_w, (float)h / img_h);
}

void gpu_d3_traces(int x, int y, int w, const gpu_d3_trace_t *t, int n) {
    static float v[6 * 12 * 72];
    int          k = 0;

    if (!in_frame || !tex_d3col || n < 1) return;
    if (n > 72) n = 72;
    for (int i = 0; i < n; i++) {
        float x0 = (float)x, x1 = (float)(x + w), y0 = (float)(y + t[i].y0), y1 = (float)(y + t[i].y1);
        float r = (t[i].row + 0.5f) / d3col_h;
        float q[6][4] = {{x0, y0, 0, (float)t[i].y0}, {x1, y0, 1, (float)t[i].y0}, {x0, y1, 0, (float)t[i].y1},
                         {x1, y0, 1, (float)t[i].y0}, {x1, y1, 1, (float)t[i].y1}, {x0, y1, 0, (float)t[i].y1}};

        if (t[i].y1 <= t[i].y0) continue;
        for (int j = 0; j < 6; j++, k++) {
            float *o = v + k * 12;

            o[0] = q[j][0], o[1] = q[j][1], o[2] = q[j][2], o[3] = q[j][3];
            o[4] = r, o[5] = t[i].foot_px, o[6] = t[i].foot_frac, o[7] = t[i].per_px;
            o[8] = t[i].fog / 256.0f, o[9] = t[i].fill_fog / 256.0f, o[10] = 0, o[11] = 0;
        }
    }
    if (!k) return;
    scissor(cx0, cy0, cw0, ch0);
    use(P_D3);
    bind(tex_d3col, tex_pal[4]);
    p_glUniform1i(u_d3_col, 0);
    p_glUniform1i(u_d3_pal, 1);
    p_glUniform1f(u_d3_dr, 1.0f / d3col_h);
    p_glEnableVertexAttribArray(3);
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 48, v);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, 0, 48, v + 2);
    p_glVertexAttribPointer(3, 4, GL_FLOAT, 0, 48, v + 4);
    p_glVertexAttribPointer(4, 4, GL_FLOAT, 0, 48, v + 8);
    p_glEnable(GL_BLEND);               /* the top pixel's coverage */
    p_glDrawArrays(0x0004, 0, k);       /* GL_TRIANGLES */
    p_glDisable(GL_BLEND);
    p_glDisableVertexAttribArray(3);
    p_glDisableVertexAttribArray(4);
}

void gpu_d3_join(int x, int y, int w, int row, int y0, int y1, uint32_t rgb, float op) {
    float c[3];

    if (!in_frame || !tex_d3col || y1 <= y0) return;
    rgb4(rgb, c);
    scissor(cx0, cy0, cw0, ch0);
    use(P_JOIN);
    bind(tex_d3col, tex_d3col);
    p_glUniform1i(u_jn_col, 0);
    p_glUniform1f(u_jn_row, (row + 0.5f) / d3col_h);
    p_glUniform4f(u_jn_c, c[0], c[1], c[2], op);
    p_glEnable(GL_BLEND);
    quad((float)x, (float)(y + y0), (float)(x + w), (float)(y + y1), 0, (float)y0, 1, (float)y1);
    p_glDisable(GL_BLEND);
}

void gpu_rect(int x, int y, int w, int h, uint32_t rgb) {
    float c[3];

    if (!in_frame || w <= 0 || h <= 0) return;
    rgb4(rgb, c);
    scissor(cx0, cy0, cw0, ch0);
    use(P_RECT);
    p_glUniform4f(u_rect_color, c[0], c[1], c[2], 1);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 0, 0);
}

void gpu_frame_end(int x, int y, int w, int h) {
    if (!in_frame) return;
    in_frame = false;
    p_glDisable(GL_SCISSOR_TEST);
    p_glFinish();                       /* drawn before the plane is flipped to it */
    drm_wf_drawn(portrait, x, y, w, h);
}

/* drm_flush's (drm_gui_gpu): rects of LVGL's frame turned into the overlay's buffer back, waited for */
static bool gui_draw(int back, bool port, const lv_area_t *r, int n) {
    static float v[2 * 128 * 6 * 4];
    float        fw = (float)(port ? pw : ph), fh = (float)(port ? ph : pw);
    int          k = 0;

    if (back < 0 || back > 1 || !ov_fbo[back] || in_frame) return false;
    if (n > 2 * 128) n = 2 * 128;
    for (int i = 0; i < n; i++) {
        float x0 = (float)r[i].x1, y0 = (float)r[i].y1, x1 = (float)(r[i].x2 + 1), y1 = (float)(r[i].y2 + 1);
        float q[6][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}};

        for (int j = 0; j < 6; j++, k++) {
            v[k * 4]     = q[j][0];
            v[k * 4 + 1] = q[j][1];
            v[k * 4 + 2] = q[j][0] / fw;
            v[k * 4 + 3] = q[j][1] / fh;
        }
    }
    if (!k) return true;
    portrait = port;
    p_glBindFramebuffer(GL_FRAMEBUFFER, ov_fbo[back]);
    p_glViewport(0, 0, pw, ph);
    p_glDisable(GL_SCISSOR_TEST);
    use(P_GUI);
    bind(tex_gui[port ? 1 : 0], tex_gui[port ? 1 : 0]);
    p_glUniform1i(u_gui_img, 0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, 0, 16, v);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, 0, 16, v + 2);
    p_glDrawArrays(0x0004, 0, k);       /* GL_TRIANGLES */
    p_glFinish();                       /* turned before the plane is flipped to it */
    return !p_glGetError();
}

#else  /* no DRM (the Mac's build): no GPU */

bool gpu_start(void) {
    return false;
}
void gpu_radar(int x, int y, int size, float r, float sweep, uint32_t ink) {
    (void)x, (void)y, (void)size, (void)r, (void)sweep, (void)ink;
}
void gpu_point(int x, int y, uint32_t rgb) {
    (void)x, (void)y, (void)rgb;
}
void gpu_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    (void)x0, (void)y0, (void)x1, (void)y1, (void)rgb;
}
void gpu_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb) {
    (void)x0, (void)y0, (void)x1, (void)y1, (void)x2, (void)y2, (void)rgb;
}
void gpu_prims_flush(void) {}
bool gpu_d3_size(int cols, int rows) {
    (void)cols, (void)rows;
    return false;
}
void gpu_d3_cols(int row0, int rows, const uint8_t *rgba) {
    (void)row0, (void)rows, (void)rgba;
}
bool gpu_d3_bg(const uint32_t *xrgb, int w, int h) {
    (void)xrgb, (void)w, (void)h;
    return false;
}
void gpu_d3_draw_bg(int x, int y, int w, int h) {
    (void)x, (void)y, (void)w, (void)h;
}
void gpu_d3_traces(int x, int y, int w, const gpu_d3_trace_t *t, int n) {
    (void)x, (void)y, (void)w, (void)t, (void)n;
}
void gpu_d3_join(int x, int y, int w, int row, int y0, int y1, uint32_t rgb, float op) {
    (void)x, (void)y, (void)w, (void)row, (void)y0, (void)y1, (void)rgb, (void)op;
}
bool gpu_init(int cols, int rows) {
    (void)cols, (void)rows;
    return false;
}
bool gpu_frame_begin_buf(int buf, bool port) {
    (void)buf, (void)port;
    return false;
}
void gpu_frame_finish(int x, int y, int w, int h) {
    (void)x, (void)y, (void)w, (void)h;
}
void gpu_wf(int s, int pal, int x, int y, int w, int h, int head) {
    (void)s, (void)pal, (void)x, (void)y, (void)w, (void)h, (void)head;
}
bool gpu_ok(void) {
    return false;
}
bool gpu_slot(int s, int cols, int rows) {
    (void)s, (void)cols, (void)rows;
    return false;
}
void gpu_palette(int which, const uint32_t *pal) {
    (void)which, (void)pal;
}
void gpu_levels_row(int s, int ring_row, const uint8_t *levels) {
    (void)s, (void)ring_row, (void)levels;
}
void gpu_levels_col(int s, int col, const uint8_t *levels) {
    (void)s, (void)col, (void)levels;
}
void gpu_levels_all(int s, const uint8_t *levels) {
    (void)s, (void)levels;
}
void gpu_trace_set(int s, const uint8_t (*cols)[4]) {
    (void)s, (void)cols;
}
bool gpu_frame_begin(bool port) {
    (void)port;
    return false;
}
void gpu_clear(int x, int y, int w, int h, uint32_t bg) {
    (void)x, (void)y, (void)w, (void)h, (void)bg;
}
void gpu_trace(int s, int x, int y, int w, int h, float bot_db, float top_db, uint32_t grid, uint32_t bg) {
    (void)s, (void)x, (void)y, (void)w, (void)h, (void)bot_db, (void)top_db, (void)grid, (void)bg;
}
void gpu_rows(int s, int x, int y, int w, int h, int head, int count, int depth) {
    (void)s, (void)x, (void)y, (void)w, (void)h, (void)head, (void)count, (void)depth;
}
void gpu_flat(int s, int x, int y, int w, int h, int head, int count, int band, uint32_t bg) {
    (void)s, (void)x, (void)y, (void)w, (void)h, (void)head, (void)count, (void)band, (void)bg;
}
void gpu_vp(int s, int x, int y, int w, int h, int head) {
    (void)s, (void)x, (void)y, (void)w, (void)h, (void)head;
}
void gpu_rect(int x, int y, int w, int h, uint32_t rgb) {
    (void)x, (void)y, (void)w, (void)h, (void)rgb;
}
void gpu_frame_end(int x, int y, int w, int h) {
    (void)x, (void)y, (void)w, (void)h;
}

#endif
