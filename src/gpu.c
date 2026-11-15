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

/* ---- State ---- */

#define SLOTS 4                     /* the pictures' data: A/B's halves (0, 1), G's spectrum (2), its voiceprint (3) */

enum { P_ROWS, P_TRACE, P_RECT, P_FLAT, P_VP, P_N };

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
static GLuint tex_pal[2], tex_lv[SLOTS], tex_tr[SLOTS];
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
          LOAD(gles, glEnableVertexAttribArray) && LOAD(gles, glDrawArrays) && LOAD(gles, glFinish) &&
          LOAD(gles, glGenTextures) && LOAD(gles, glDeleteTextures) && LOAD(gles, glBindTexture) && LOAD(gles, glTexImage2D) &&
          LOAD(gles, glTexSubImage2D) && LOAD(gles, glTexParameteri) && LOAD(gles, glActiveTexture) &&
          LOAD(gles, glPixelStorei) && LOAD(gles, glGetString) && LOAD(gles, glGetError) &&
          LOAD(gles, glBindBuffer);
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

static GLuint program(const char *fs_src) {
    GLuint vs = shader(GL_VERTEX_SHADER, vs_src), fs = shader(GL_FRAGMENT_SHADER, fs_src), p;
    GLint  ok = 0;

    if (!vs || !fs) return 0;
    p = p_glCreateProgram();
    p_glAttachShader(p, vs);
    p_glAttachShader(p, fs);
    p_glBindAttribLocation(p, 0, "a_pos");
    p_glBindAttribLocation(p, 1, "a_uv");
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

static bool setup(void) {
    int          fds[2] = {-1, -1}, rfd;
    uint32_t     pitch[2];
    int32_t      w, h;
    EGLDisplay   dpy;
    EGLContext   ctx;
    EGLConfig    cfg = NULL;
    const char  *ext;
    static const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

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
        const char *src[P_N] = {fs_rows_src, fs_trace_src, fs_rect_src, fs_flat_src, fs_vp_src};

        for (int i = 0; i < P_N; i++) {
            if (!(prog[i] = program(src[i]))) goto fail;
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
    tex_pal[0] = texture(256, 1, GL_RGBA);
    tex_pal[1] = texture(256, 1, GL_RGBA);
    p_glDisable(GL_DITHER);
    p_glDisable(GL_BLEND);
    p_glBindBuffer(GL_ARRAY_BUFFER, 0);     /* client-side vertex arrays */
    p_glEnableVertexAttribArray(0);
    p_glEnableVertexAttribArray(1);
    if (p_glGetError()) {
        fprintf(stderr, "gpu: GL error while setting up: the pictures drawn by the CPU\n");
        goto fail;
    }
    fprintf(stderr, "gpu: %s draws the Sentry's pictures into the display's plane (%dx%d)\n",
            (const char *)p_glGetString(GL_RENDERER), w, h);
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

bool gpu_init(int cols, int rows) {
    if (!tried) tried = setup() ? 1 : -1;
    return tried > 0 && slot(0, cols, rows) && slot(1, cols, rows);
}

bool gpu_ok(void) {
    return tried > 0;
}

bool gpu_slot(int s, int cols, int rows) {
    return slot(s, cols, rows);
}

void gpu_palette(int which, const uint32_t *pal) {
    uint8_t px[256 * 4];

    if (tried <= 0 || which < 0 || which > 1) return;
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

bool gpu_frame_begin(bool port) {
    int b;

    if (tried <= 0 || (b = drm_wf_back(port)) < 0) return false;
    portrait = port;
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo[b]);
    p_glViewport(0, 0, pw, ph);
    p_glEnable(GL_SCISSOR_TEST);
    in_frame = true;
    return true;
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

#else  /* no DRM (the Mac's build): no GPU */

bool gpu_init(int cols, int rows) {
    (void)cols, (void)rows;
    return false;
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
