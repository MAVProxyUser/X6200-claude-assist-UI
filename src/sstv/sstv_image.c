/*
 * SSTV picture decoding (see sstv_image.h).
 *
 * Front end: a 9-tap Hilbert pair gives the phase of every sample, its step
 * is the instant frequency, smoothed over 11 samples.  1500 Hz is black,
 * 2300 Hz white.
 *
 * Line timing: 1200-Hz syncs are looked for in the audio itself (coherent
 * 1200-Hz energy in windows one sync long), which survives the short holes
 * fading and noise punch in a pulse.  Then, as QSSTV does in its
 * syncProcessor and slantAdjust (ON4QZ, GPL), the syncs are numbered with
 * whole lines (lost syncs leave gaps) and a straight line through
 * (line, position) gives start and period, so slant is corrected and lost
 * syncs are rebuilt.  A sync far off the line is dropped and the fit done
 * again; a second pass over all syncs can lengthen a short early chain.
 * A chain found late in the picture is moved back by whole lines.  With no
 * usable chain the picture is still drawn at the nominal timing.
 *
 * PD: a line (one sync) carries two rows, so the line timing above works on
 * lines and every line is drawn as two rows (Y of each, the colour shared).
 */
#include "sstv_image.h"
#include "sstv_vis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR        SSTV_RATE
#define PI_F      3.14159265358979323846f
#define HIL_LEN   9
#define HIL_MID   (HIL_LEN / 2)
#define AVG_LEN   11
#define PULSES    300
#define MAX_GAP   8           /* lines between two chained syncs */
#define SYNC_WIN  224         /* longest sync window, samples (PD: 20 ms) */
#define FM_LAG    8.0         /* samples sstv_fm() lags its input (see pd_edges) */

/* ---- Front end ---- */

float *sstv_fm(const float *x, long n)
{
    float *hz = x && n > 0 ? malloc((size_t)n * sizeof(float)) : NULL;
    float  h[HIL_LEN], ring[AVG_LEN] = { 0 }, sum = 0.0f, last = 0.0f;
    int    rp = 0;

    if (!hz) return NULL;
    for (int k = 0; k < HIL_LEN; ++k) {
        int m = k - HIL_MID;
        h[k] = (m & 1 ? 2.0f / (PI_F * (float)m) : 0.0f) *
               (0.54f - 0.46f * cosf(2.0f * PI_F * (float)k / (float)(HIL_LEN - 1)));
    }
    for (long i = 0; i < n; ++i) {
        float q = 0.0f, re = i >= HIL_MID ? x[i - HIL_MID] : 0.0f;
        if (i >= HIL_LEN - 1)
            for (int k = 0; k < HIL_LEN; ++k) q += h[k] * x[i - k];
        float ph = atan2f(q, re), d = ph - last;
        last = ph;
        while (d > PI_F) d -= 2.0f * PI_F;
        while (d < -PI_F) d += 2.0f * PI_F;
        float f = fabsf(d) * ((float)SR / (2.0f * PI_F));
        if (f > 4000.0f) f = 4000.0f;
        sum -= ring[rp];        /* moving average: oldest out, newest in */
        sum += f;
        ring[rp] = f;
        rp = (rp + 1) % AVG_LEN;
        hz[i] = sum / (float)AVG_LEN;
    }
    return hz;
}

static uint8_t byte(float v)
{
    return v < 0.0f ? 0 : v > 255.0f ? 255 : (uint8_t)(v + 0.5f);
}

/* Brightness 0..255 at a fractional sample position */
static float level(const float *hz, long n, float pos)
{
    if (pos < 0) pos = 0;
    if (pos >= n - 1) pos = (float)(n - 2);
    long  i = (long)pos;
    float f = hz[i] + (hz[i + 1] - hz[i]) * (pos - (float)i);
    float v = (f - 1500.0f) * (255.0f / 800.0f);
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* count pixels from samples [a, b), each read at its centre */
static void scan(const float *hz, long n, long a, long b, float *px, int count)
{
    float step = (float)(b - a) / (float)count;
    for (int i = 0; i < count; ++i)
        px[i] = level(hz, n, (float)a + ((float)i + 0.5f) * step);
}

static void ycc(float y, float cr, float cb, uint8_t *p)
{
    p[0] = byte(y + 1.402f * cr);
    p[1] = byte(y - 0.714136f * cr - 0.344136f * cb);
    p[2] = byte(y + 1.772f * cb);
}

/* ---- How each mode is laid out ---- */

typedef struct {
    sstv_mode_t mode;
    float       pulse_ms;      /* sync looked for                                    */
    int         pulses;        /* at most this many syncs                            */
    double      first_s;       /* row-0 sync of an Auto RX capture, also the Manual pre-roll */
    double      blind_s;       /* row-0 sync assumed when no chain is found          */
    double      reach;         /* ...moved to a real sync this many lines away       */
    bool        martin;        /* Martin keeps its own chain rules (see fit_grid)    */
} layout_t;

static const layout_t layouts[] = {
    { SSTV_ROBOT8,     10.0f,  180, 2.000, 2.000, 1.00, false },
    { SSTV_ROBOT36,     9.0f, PULSES, 2.000, 2.000, 1.00, false },
    { SSTV_ROBOT72,     9.0f, PULSES, 2.000, 2.000, 0.75, false },
    { SSTV_MARTIN1,   4.862f, PULSES, 2.350, 1.930, 0.75, true },
    { SSTV_MARTIN2,   4.862f, PULSES, 2.200, 1.930, 0.75, true },
    { SSTV_SCOTTIE1,    9.0f, PULSES, 2.229, 2.229, 0.50, false },
    { SSTV_SCOTTIE2,    9.0f, PULSES, 2.229, 2.229, 0.50, false },
    { SSTV_SCOTTIE_DX,  9.0f, PULSES, 2.229, 2.229, 0.50, false },
    { SSTV_PD90,       20.0f, PULSES, 2.000, 2.000, 0.50, false },
    { SSTV_PD120,      20.0f, PULSES, 2.000, 2.000, 0.50, false },
    { SSTV_PD180,      20.0f, PULSES, 2.000, 2.000, 0.50, false },
};

static const layout_t *layout(sstv_mode_t mode)
{
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i)
        if (layouts[i].mode == mode) return &layouts[i];
    return NULL;
}

/* Colour scan length of Martin and Scottie, scan of one component of PD, ms */
static float scan_ms(sstv_mode_t mode)
{
    switch (mode) {
    case SSTV_MARTIN1:    return 146.432f;
    case SSTV_MARTIN2:    return 73.216f;
    case SSTV_SCOTTIE1:   return 138.240f;
    case SSTV_SCOTTIE2:   return 88.064f;
    case SSTV_SCOTTIE_DX: return 345.600f;
    case SSTV_PD90:       return 170.240f;
    case SSTV_PD120:      return 121.600f;
    case SSTV_PD180:      return 183.040f;
    default:              return 0.0f;
    }
}

static bool pd(sstv_mode_t mode) { return mode == SSTV_PD90 || mode == SSTV_PD120 || mode == SSTV_PD180; }

/* PD scan edges from the start of the sync, ms: Y0, R-Y, B-Y, Y1, each read
 * FM_LAG later.  The front end (Hilbert pair + 11-sample mean) answers a
 * frequency step half-way 9.7 samples late; at PD120's 2.1 samples a pixel
 * that is worth 4 px.  8 samples put PD90, PD120 and PD180 test pictures
 * exactly on their pixels (the other modes are read without it). */
static void pd_edges(sstv_mode_t mode, double *ms)
{
    const double ch = scan_ms(mode), lag = FM_LAG * 1000.0 / SR;
    for (int i = 0; i < 8; ++i) ms[i] = 20.0 + 2.08 + ch * (double)((i + 1) / 2) + lag;
}

/* Line period, ms */
static float line_ms(sstv_mode_t mode)
{
    switch (mode) {
    case SSTV_ROBOT8:  return 1000.0f / 15.0f;
    case SSTV_ROBOT36: return 150.0f;
    case SSTV_ROBOT72: return 300.0f;
    case SSTV_MARTIN1: return 446.446f;
    case SSTV_MARTIN2: return 226.798f;
    case SSTV_PD90:
    case SSTV_PD120:
    case SSTV_PD180:   return 20.0f + 2.08f + 4.0f * scan_ms(mode);
    default:           return 9.0f + 3.0f * 1.5f + 3.0f * scan_ms(mode);   /* Scottie */
    }
}

/* The sample ranges of a row whose sync starts at base (Scottie: the sync
 * between B and R), line length scale x nominal.  seg[2i], seg[2i+1]: one
 * scan; Robot 8 Y; Robot 36 Y, chroma; Robot 72 Y, R-Y, B-Y; Martin and
 * Scottie G, B, R; PD Y0, R-Y, B-Y, Y1 (a line of two rows).  Returns the
 * number of scans.  Every scan is read FM_LAG later than its nominal edges,
 * as the front end answers that late (pd_edges); without it the pictures sat
 * 2-4 px to the right (measured through sstv_tx and sstv_decode). */
static int segments_nominal(sstv_mode_t mode, long base, double scale, long *seg);

static int segments(sstv_mode_t mode, long base, double scale, long *seg)
{
    int k = segments_nominal(mode, base, scale, seg);

    if (!pd(mode)) {            /* PD's edges have the lag in them already */
        const long lag = (long)llround(FM_LAG * scale);
        for (int i = 0; i < 2 * k; ++i) seg[i] += lag;
    }
    return k;
}

static int segments_nominal(sstv_mode_t mode, long base, double scale, long *seg)
{
    switch (mode) {
    case SSTV_ROBOT8: {
        const float sync = 10.0f, video = 56.0f;
        seg[0] = base + (long)llround(sync * SR / 1000.0 * scale);
        seg[1] = seg[0] + (long)llround(video * SR / 1000.0 * scale);
        return 1;
    }
    case SSTV_ROBOT36:
    case SSTV_ROBOT72: {
        static const double r36[] = { .012, .100, .106, .150 };
        static const double r72[] = { .012, .150, .156, .225, .231, .300 };
        const double *t = mode == SSTV_ROBOT36 ? r36 : r72;
        int           k = mode == SSTV_ROBOT36 ? 4 : 6;
        for (int i = 0; i < k; ++i)
            seg[i] = base + (long)llround(t[i] * SR * scale);
        return k / 2;
    }
    case SSTV_MARTIN1:
    case SSTV_MARTIN2: {
        const float ch = scan_ms(mode), gap = 0.572f;
        const float g = 4.862f + gap, b = g + ch + gap, r = b + ch + gap;
        const float at[6] = { g, g + ch, b, b + ch, r, r + ch };
        for (int i = 0; i < 6; ++i)
            seg[i] = base + (long)llround(at[i] * SR / 1000.0 * scale);
        return 3;
    }
    case SSTV_SCOTTIE1:
    case SSTV_SCOTTIE2:
    case SSTV_SCOTTIE_DX: {
        const float ch = scan_ms(mode), gap = 1.5f, sync = 9.0f;
        seg[2] = base - (long)llround(ch * SR / 1000.0 * scale);       /* B */
        seg[3] = base;
        seg[1] = seg[2] - (long)llround(gap * SR / 1000.0 * scale);    /* G */
        seg[0] = seg[1] - (long)llround(ch * SR / 1000.0 * scale);
        seg[4] = base + (long)llround((sync + gap) * SR / 1000.0 * scale);   /* R */
        seg[5] = seg[4] + (long)llround(ch * SR / 1000.0 * scale);
        return 3;
    }
    case SSTV_PD90:
    case SSTV_PD120:
    case SSTV_PD180: {
        double ms[8];
        pd_edges(mode, ms);
        for (int i = 0; i < 8; ++i)
            seg[i] = base + (long)llround(ms[i] * SR / 1000.0 * scale);
        return 4;
    }
    default:
        return 0;
    }
}

/* The same ranges for the rows drawn while receiving, every edge placed
 * from the sync on its own (no chaining); Robot 36: Y only */
static int segments_live(sstv_mode_t mode, long base, double scale, long *seg)
{
    static const double r8[] = { 10.0, 66.0 }, r36[] = { 12.0, 100.0 },
                        r72[] = { 12.0, 150.0, 156.0, 225.0, 231.0, 300.0 };
    const double  k = (double)SR / 1000.0 * scale;     /* samples per ms */
    double        ms[8];
    const double *t = ms;
    int           n = 6;

    switch (mode) {
    case SSTV_ROBOT8:  t = r8; n = 2; break;
    case SSTV_ROBOT36: t = r36; n = 2; break;
    case SSTV_ROBOT72: t = r72; break;
    case SSTV_MARTIN1:
    case SSTV_MARTIN2: {
        double ch = mode == SSTV_MARTIN1 ? 146.432 : 73.216, g = 4.862 + 0.572, b = g + ch + 0.572, r = b + ch + 0.572;
        ms[0] = g, ms[1] = g + ch, ms[2] = b, ms[3] = b + ch, ms[4] = r, ms[5] = r + ch;
        break;
    }
    case SSTV_SCOTTIE1:
    case SSTV_SCOTTIE2:
    case SSTV_SCOTTIE_DX: {
        double ch = mode == SSTV_SCOTTIE1 ? 138.240 : mode == SSTV_SCOTTIE2 ? 88.064 : 345.600;
        ms[0] = -(2.0 * ch + 1.5), ms[1] = -(ch + 1.5), ms[2] = -ch, ms[3] = 0.0, ms[4] = 10.5, ms[5] = 10.5 + ch;
        break;
    }
    case SSTV_PD90:
    case SSTV_PD120:
    case SSTV_PD180:
        pd_edges(mode, ms);
        n = 8;
        break;
    default:
        return 0;
    }
    for (int i = 0; i < n; ++i)
        seg[i] = base + (long)llround(t[i] * k);
    return n / 2;
}

/* The row's audio must all be there */
static bool inside(long base, const long *seg, int k, long n)
{
    long lo = base, hi = base;
    for (int i = 0; i < 2 * k; ++i) {
        if (seg[i] < lo) lo = seg[i];
        if (seg[i] > hi) hi = seg[i];
    }
    return lo >= 0 && hi < n;
}

/* PD colour: ITU-R BT.601, Y 16..235 and R-Y, B-Y 16..240 around 128, the
 * conversion of the PD definition (JL Barber N7CXI, Dayton 2000).  QSSTV and
 * slowrx read PD with the full 0..255, as the Robot modes here do. */
static void ycc601(float y, float cr, float cb, uint8_t *p)
{
    y = 1.164383f * (y - 16.0f);
    cr -= 128.0f;
    cb -= 128.0f;
    p[0] = byte(y + 1.596027f * cr);
    p[1] = byte(y - 0.812968f * cr - 0.391762f * cb);
    p[2] = byte(y + 2.017232f * cb);
}

/* One row of any mode but Robot 36 colour; Robot 36 gives its Y in grey.
 * PD: the two rows of a line, out holds 2 x width pixels.  live: the edges
 * of the rows drawn while receiving */
static bool draw(sstv_mode_t mode, const float *hz, long n, long base, double scale, int width, bool live,
                 uint8_t *out)
{
    float a[SSTV_MAX_W], b[SSTV_MAX_W], c[SSTV_MAX_W];
    long  seg[8];
    int   k = live ? segments_live(mode, base, scale, seg) : segments(mode, base, scale, seg);

    if (!k || !inside(base, seg, k, n)) return false;
    scan(hz, n, seg[0], seg[1], a, width);
    if (k == 1 || mode == SSTV_ROBOT36) {
        for (int x = 0; x < width; ++x) out[3 * x] = out[3 * x + 1] = out[3 * x + 2] = byte(a[x]);
        return true;
    }
    scan(hz, n, seg[2], seg[3], b, width);
    scan(hz, n, seg[4], seg[5], c, width);
    if (k == 4) {               /* PD: Y0, R-Y, B-Y, then the second row's Y in a */
        for (int x = 0; x < width; ++x) ycc601(a[x], b[x], c[x], out + 3 * x);
        scan(hz, n, seg[6], seg[7], a, width);
        for (int x = 0; x < width; ++x) ycc601(a[x], b[x], c[x], out + 3 * (width + x));
        return true;
    }
    for (int x = 0; x < width; ++x) {
        uint8_t *p = out + 3 * x;
        if (mode == SSTV_ROBOT72) {
            ycc(a[x], b[x] - 128.0f, c[x] - 128.0f, p);
        } else {            /* G, B, R */
            p[0] = byte(c[x]);
            p[1] = byte(a[x]);
            p[2] = byte(b[x]);
        }
    }
    return true;
}

/* ---- Line timing ---- */

/* Starts of 1200-Hz pulses about pulse_ms long, from 0.5 s on: windows
 * every ~1 ms; a run of windows over the threshold gives its best one */
static int find_syncs(const float *x, long n, float pulse_ms, long *at, int max)
{
    const int   hop = (SR + 500) / 1000;
    const float need = 0.45f;
    float       cs[SYNC_WIN], sn[SYNC_WIN], top = 0.0f;
    int         win = (int)lroundf(pulse_ms * (float)SR / 1000.0f), found = 0;
    bool        run = false;
    long        top_at = 0;

    if (!x || n <= 0 || max <= 0) return 0;
    if (win < 24) win = 24;
    if (win > SYNC_WIN) win = SYNC_WIN;
    for (int k = 0; k < win; ++k) {
        float ph = 2.0f * PI_F * 1200.0f * (float)k / (float)SR;
        cs[k] = cosf(ph);
        sn[k] = sinf(ph);
    }
    for (long a = SR / 2; a + win < n; a += hop) {
        double re = 0.0, im = 0.0, pw = 0.0;
        for (int k = 0; k < win; ++k) {
            float v = x[a + k];
            re += (double)v * (double)cs[k];
            im -= (double)v * (double)sn[k];
            pw += (double)v * (double)v;
        }
        float score = pw > 1e-12 ? (float)(sqrt(re * re + im * im) / sqrt(pw * (double)win)) : 0.0f;
        if (score >= need) {
            if (!run || score > top) {
                top = score;
                top_at = a;
            }
            run = true;
        } else if (run) {
            at[found++] = top_at;
            run = false;
            top = 0.0f;
            if (found >= max) break;
        }
    }
    if (run && found < max) at[found++] = top_at;
    return found;
}

typedef struct { double sl, st, sll, slt; int n; } sums_t;

static void sums_add(sums_t *s, double line, double at)
{
    s->sl += line;
    s->st += at;
    s->sll += line * line;
    s->slt += line * at;
    s->n++;
}

/* Least squares at = start + period * line; false if degenerate */
static bool sums_fit(const sums_t *s, double *start, double *period)
{
    double d = (double)s->n * s->sll - s->sl * s->sl;
    if (fabs(d) < 1e-9) return false;
    *period = ((double)s->n * s->slt - s->sl * s->st) / d;
    *start = (s->st - *period * s->sl) / (double)s->n;
    return true;
}

/* Second pass: every sync close (2% of a line) to a row of the fitted grid,
 * the nearest one per row, refitted.  Taken only when it covers at least 12
 * more rows and moves the clock by less than 400 ppm: a slant refinement,
 * not a new timing. */
static void widen(const long *at, int count, int height, double nominal, int had_points, int had_span,
                  double *start, double *period)
{
    double s0 = *start, p0 = *period;
    if (count <= 0 || p0 <= 0.0 || s0 > 5.0 * (double)SR) return;   /* late chain: moved back later */

    long   pos[SSTV_MAX_H];
    double off[SSTV_MAX_H];
    for (int r = 0; r < SSTV_MAX_H; ++r) off[r] = 1e30;
    const double gate = nominal * 0.020;
    for (int k = 0; k < count; ++k) {
        long row = lround(((double)at[k] - s0) / p0);
        if (row < 0 || row >= height) continue;
        double e = fabs((double)at[k] - (s0 + p0 * (double)row));
        if (e <= gate && e < off[row]) {
            pos[row] = at[k];
            off[row] = e;
        }
    }
    sums_t s = { 0 };
    int    first = -1, last = -1;
    for (int r = 0; r < height; ++r)
        if (off[r] < 1e30) {
            if (first < 0) first = r;
            last = r;
            sums_add(&s, (double)r, (double)pos[r]);
        }
    if (s.n < 12 || first < 0 || last - first < had_span + 12) return;

    double s1, p1;
    if (!sums_fit(&s, &s1, &p1)) return;
    double ppm = fabs(p1 / p0 - 1.0) * 1000000.0;
    if (ppm > 400.0) {
        fprintf(stderr, "SSTV: second sync pass refused, %.0f ppm away (%d syncs)\n", ppm, s.n);
        return;
    }
    fprintf(stderr, "SSTV: second sync pass: %d -> %d syncs, %d -> %d rows, %+.1f -> %+.1f ppm\n", had_points, s.n,
            had_span + 1, last - first + 1, (p0 / nominal - 1.0) * 1e6, (p1 / nominal - 1.0) * 1e6);
    *start = s1;
    *period = p1;
}

/* Nominal period, phase kept: mean start of the chain at that period */
static double start_at(const long *at, const int *idx, const int *line, int count, double period)
{
    double sum = 0.0;
    for (int k = 0; k < count; ++k) sum += (double)at[idx[k]] - period * (double)line[k];
    return sum / (double)count;
}

/* Row-sync grid from the syncs at[0..count).  false: no usable chain (the
 * caller then draws at the nominal timing).  Martin keeps rules of its own:
 * tighter chain links, a two-sync chain at the expected place instead of a
 * failure, no check of the first fit, at least 12 syncs to refit, the
 * second pass before the weak-chain check, which also looks at the span. */
static bool fit_grid(const layout_t *L, long *at, int count, int height, double nominal, long *grid, double *period_out)
{
    const double tol = L->martin ? 0.035 : 0.040;
    int          idx[SSTV_MAX_H], line[SSTV_MAX_H], best_idx[SSTV_MAX_H], best_line[SSTV_MAX_H];
    int          points = 0, span = 0;

    /* longest chain: every sync a whole number of lines after the last one */
    for (int s = 0; s < count; ++s) {
        int  c = 1, ln = 0;
        long last = at[s];
        idx[0] = s;
        line[0] = 0;
        for (int j = s + 1; j < count && c < height; ++j) {
            double d = (double)(at[j] - last);
            int    lines = (int)floor(d / nominal + 0.5);
            if (lines < 1) continue;
            if (lines > MAX_GAP) break;
            if (fabs((double)lines - d / nominal) < tol) {
                ln += lines;
                if (ln >= height) break;
                idx[c] = j;
                line[c++] = ln;
                last = at[j];
            }
        }
        if (line[c - 1] > span || (line[c - 1] == span && c > points)) {
            span = line[c - 1];
            points = c;
            memcpy(best_idx, idx, (size_t)c * sizeof(int));
            memcpy(best_line, line, (size_t)c * sizeof(int));
        }
    }

    if (points < 4 || span < 4) {
        if (!L->martin) {
            fprintf(stderr, "SSTV: no sync chain (%d syncs over %d lines)\n", points, span);
            return false;
        }
        /* two syncs one line apart at the expected place, or the real sync
         * closest to it */
        double first = L->blind_s * (double)SR;
        if (count > 0) {
            long   near = at[0];
            double dist = fabs((double)near - first);
            for (int k = 1; k < count; ++k)
                if (fabs((double)at[k] - first) < dist) {
                    dist = fabs((double)at[k] - first);
                    near = at[k];
                }
            if (dist <= nominal * L->reach) first = (double)near;
        }
        at[0] = (long)llround(first);
        at[1] = (long)llround(first + nominal);
        best_idx[0] = 0;
        best_line[0] = 0;
        best_idx[1] = 1;
        best_line[1] = 1;
        points = 2;
        span = 1;
        fprintf(stderr, "SSTV: no sync chain, nominal timing (%d syncs)\n", count);
    }

    sums_t s = { 0 };
    double start, period;
    for (int k = 0; k < points; ++k) sums_add(&s, best_line[k], at[best_idx[k]]);
    if (!sums_fit(&s, &start, &period)) return false;

    /* a few noisy syncs must not invent a silly clock */
    if (!L->martin && fabs(period / nominal - 1.0) > 0.015) {
        period = nominal;
        start = start_at(at, best_idx, best_line, points, period);
    }

    /* without the syncs far off the line */
    s = (sums_t){ 0 };
    for (int k = 0; k < points; ++k) {
        double l = best_line[k], p = at[best_idx[k]];
        if (fabs(p - (start + period * l)) <= nominal * 0.08) sums_add(&s, l, p);
    }
    int used = s.n;
    if (used >= (L->martin ? 12 : 8)) {
        double s1, p1;
        if (sums_fit(&s, &s1, &p1)) {
            start = s1;
            period = p1;
        }
    }

    /* A weak chain can fit neatly and still be noise: then its clock is not
     * believed, only its phase */
    if (L->martin) widen(at, count, height, nominal, points, span, &start, &period);
    bool weak = L->martin ? points < 10 || span < 10 : points < 10;
    if (weak && fabs(period / nominal - 1.0) * 1000000.0 > 500.0) {
        fprintf(stderr, "SSTV: weak chain, %+.0f ppm not believed\n", (period / nominal - 1.0) * 1e6);
        period = nominal;
        start = start_at(at, best_idx, best_line, points, period);
    }
    if (!L->martin) widen(at, count, height, nominal, points, span, &start, &period);

    for (int r = 0; r < height; ++r) grid[r] = (long)llround(start + period * (double)r);
    fprintf(stderr, "SSTV: chain %d syncs over %d lines, %d in the fit, line %.6f ms (%+.1f ppm)\n", points,
            span + 1, used, period * 1000.0 / (double)SR, (period / nominal - 1.0) * 1e6);
    *period_out = period;
    return true;
}

/* Sync of every row: grid fit, or nominal timing from the expected place */
static void row_syncs(const layout_t *L, const float *x, long n, int height, double nominal, long *grid,
                      double *period)
{
    long at[PULSES];
    int  count = find_syncs(x, n, L->pulse_ms, at, L->pulses);

    fprintf(stderr, "SSTV: %d sync pulses\n", count);
    if (!fit_grid(L, at, count, height, nominal, grid, period)) {
        double first = L->blind_s * (double)SR;
        if (count > 0) {
            long   near = at[0];
            double dist = fabs((double)near - first);
            for (int k = 1; k < count; ++k)
                if (fabs((double)at[k] - first) < dist) {
                    dist = fabs((double)at[k] - first);
                    near = at[k];
                }
            if (dist <= nominal * L->reach) first = (double)near;
        }
        for (int r = 0; r < height; ++r) grid[r] = (long)llround(first + nominal * (double)r);
        *period = nominal;
        return;
    }
    /* a chain deep in the picture was numbered from its first sync */
    if ((double)grid[0] > 5.0 * (double)SR && *period > 0.0) {
        long back = (long)llround(((double)grid[0] - L->first_s * (double)SR) / *period);
        if (back > 0 && back < height) {
            long d = (long)llround((double)back * *period);
            for (int r = 0; r < height; ++r) grid[r] -= d;
            fprintf(stderr, "SSTV: chain started at row %ld, grid moved back\n", back);
        }
    } else if (pd(L->mode) && *period > 0.0) {
        /* PD: the first sync follows the VIS stop bit (30 ms, 1200 Hz too)
         * with no gap, reads up to 30 ms early and is left off the chain.
         * A pulse there one line before the chain is that line. */
        double want = (double)grid[0] - *period;
        for (int k = 0; k < count; ++k)
            if ((double)at[k] >= want - 0.035 * SR && (double)at[k] <= want + 0.005 * SR) {
                long d = (long)llround(*period);
                for (int r = 0; r < height; ++r) grid[r] -= d;
                fprintf(stderr, "SSTV: first sync after the VIS stop bit, grid moved back a line\n");
                break;
            }
    }
}

/* ---- Decoders ---- */

int sstv_decode(const float *x, long n, sstv_mode_t mode, uint8_t *rgb)
{
    const sstv_mode_info_t *mi = sstv_mode_info(mode);
    const layout_t         *L = layout(mode);
    long                    grid[SSTV_MAX_H], seg[8];
    double                  period;
    int                     w, h, lines, rows;

    if (!x || n <= 0 || !rgb) return -1;
    if (!mi || !L) return -2;
    w = mi->width;
    h = mi->height;
    lines = h / mi->line_rows;
    fprintf(stderr, "SSTV: decoding %s, %.1f s of audio\n", mi->name, (double)n / SR);

    float *hz = sstv_fm(x, n);
    if (!hz) return -3;
    const double nominal = (double)line_ms(mode) * SR / 1000.0;
    row_syncs(L, x, n, lines, nominal, grid, &period);
    const double scale = period / nominal;

    /* Where the audio has the VIS, line 0's sync is where the VIS says (as the rows drawn while receiving have
     * it): a chain begun on the header's own 1200 Hz pulses, its break, start and stop bits, had them numbered as
     * picture lines, Robot's and Scottie 2's pictures one or two rows low. PD keeps its own rule (row_syncs) */
    if (!pd(mode) && period > 0.0) {
        sstv_vis_t v;
        long       from = -1;

        /* sstv_vis_find takes 2-s blocks: the header (0.91 s) ends just before line 0, which the chain has within
         * a line or two; blocks ending around there, a quarter of a second apart */
        for (double back = 2.0; back >= 0.75 && from < 0; back -= 0.25) {
            long a = (long)grid[0] - lround(back * SR);
            if (a < 0) a = 0;
            if (a + 2L * SR > n) continue;
            if (sstv_vis_find(x + a, 2 * SR, &v) && v.mode == mode) from = a;
        }
        if (from >= 0) {
            double row0 = (double)from + (v.start_s + v.gap_s + SSTV_VIS_S + sstv_row0_after_vis(mode)) * SR;
            long   k    = lround((row0 - (double)grid[0]) / period);

            if (k != 0 && labs(k) <= 3) {
                long d = lround((double)k * period);
                for (int r = 0; r < lines; ++r) grid[r] += d;
                fprintf(stderr, "SSTV: line 0 from the VIS, the sync chain moved %+ld lines\n", k);
            }
        }
    }

    if (pd(mode)) {
        int line;
        for (line = 0; line < lines; ++line)
            if (!draw(mode, hz, n, grid[line], scale, w, false, rgb + (size_t)line * 2 * w * 3)) break;
        rows = 2 * line;
    } else if (mode != SSTV_ROBOT36) {
        for (rows = 0; rows < h; ++rows)
            if (!draw(mode, hz, n, grid[rows], scale, w, false, rgb + (size_t)rows * w * 3)) break;
    } else {
        /* Y on every line, R-Y and B-Y on alternate lines: the missing one
         * from the lines above and below */
        float *Y = calloc((size_t)h * w, sizeof(float)), *C = calloc((size_t)h * w, sizeof(float));
        float  half[160];
        if (!Y || !C) {
            free(Y);
            free(C);
            free(hz);
            return -6;
        }
        for (rows = 0; rows < h; ++rows) {
            segments(mode, grid[rows], scale, seg);
            if (!inside(grid[rows], seg, 2, n)) break;
            scan(hz, n, seg[0], seg[1], &Y[rows * w], w);
            scan(hz, n, seg[2], seg[3], half, 160);
            for (int p = 0; p < w; ++p) {
                float u = p * 159.0f / 319.0f, f = u - (int)u;
                int   j = (int)u;
                C[rows * w + p] = (j < 159 ? half[j] + (half[j + 1] - half[j]) * f : half[159]) - 128;
            }
        }
        for (int r = 0; r < rows; ++r)
            for (int p = 0; p < w; ++p) {
                const float *c = C + (size_t)r * w + p;
                float        both = r > 0 && r + 1 < rows ? .5f * (c[-w] + c[w]) : 0.0f;
                float        cr, cb;
                if (r & 1) {
                    cr = *c;
                    cb = r > 0 && r + 1 < rows ? both : r > 0 ? c[-w] : *c;
                } else {
                    cb = *c;
                    cr = r > 0 && r + 1 < rows ? both : r + 1 < rows ? c[w] : *c;
                }
                ycc(Y[r * w + p], cr, cb, rgb + ((size_t)r * w + p) * 3);
            }
        free(Y);
        free(C);
    }
    if (rows < h) memset(rgb + (size_t)rows * w * 3, 0, (size_t)(h - rows) * w * 3);
    free(hz);
    fprintf(stderr, "SSTV: %s decoded, %d rows\n", mi->name, rows);
    return rows;
}

int sstv_decode_manual(const float *x, long n, sstv_mode_t mode, uint8_t *rgb)
{
    const layout_t *L = layout(mode);

    if (!x || n <= 0 || !rgb) return -1;
    if (!L) return -2;

    /* A sync more than 5 s in counts as a chain deep in the picture: when
     * the picture starts late (Start pressed early), drop the audio before
     * it, from a few lines before the first sync the follower locked on */
    sstv_sync_t s;
    sstv_sync_init(&s, mode);
    sstv_sync_feed(&s, x, (size_t)n);
    if (s.st.state != SSTV_SYNC_SEARCH) {
        double back = 4.0 * sstv_sync_line_s(mode);
        if (back > 2.5) back = 2.5;
        long cut = (long)s.st.first_sync - (long)llround((back + 0.2) * SR);
        if (cut > 0) {
            fprintf(stderr, "SSTV: picture starts %.1f s in, audio before it skipped\n", (double)s.st.first_sync / SR);
            x += cut;
            n -= cut;
        }
    }

    /* silence in front, so the first sync sits where an Auto RX capture has it */
    long   pad = (long)llround(L->first_s * (double)SR);
    float *buf = calloc((size_t)(pad + n), sizeof(float));
    if (!buf) return -3;
    memcpy(buf + pad, x, (size_t)n * sizeof(float));
    int rows = sstv_decode(buf, pad + n, mode, rgb);
    free(buf);
    return rows;
}

/* ---- Rows while receiving ---- */

#define ROWS_MARGIN 100      /* audio around a row for the front end to settle */

double sstv_row0_after_vis(sstv_mode_t mode)
{
    double ch;

    switch (mode) {
    case SSTV_SCOTTIE1:   ch = 138.240; break;
    case SSTV_SCOTTIE2:   ch = 88.064; break;
    case SSTV_SCOTTIE_DX: ch = 345.600; break;
    default:              return sstv_mode_info(mode) ? 0.0 : -1.0;
    }
    return (9.0 + 1.5 + ch + 1.5 + ch) / 1000.0;     /* start sync, G, B, then the B/R sync */
}

void sstv_rows_init(sstv_rows_t *r, sstv_mode_t mode, double row0_sync)
{
    const sstv_mode_info_t *mi = sstv_mode_info(mode);

    memset(r, 0, sizeof(*r));
    r->mode = mode;
    r->width = mi ? mi->width : 0;
    r->height = mi ? mi->height : 0;
    r->per_line = mi ? mi->line_rows : 1;
    r->nominal = sstv_sync_line_s(mode) * SR;
    r->row0 = row0_sync >= 0.0 ? row0_sync + SSTV_ROWS_VIS_BIAS : -1.0;
}

bool sstv_rows_ready(sstv_rows_t *r, long written, const sstv_sync_status_t *sync, long *from, long *to)
{
    if (r->width <= 0 || r->nominal <= 0.0 || sstv_rows_done(r)) return false;
    bool locked = sync && sync->state != SSTV_SYNC_SEARCH && sync->mode == r->mode && sync->hits >= 3;

    if (r->row0 < 0.0) {                  /* no VIS: the first sync of the lock */
        if (!locked) return false;
        r->row0 = (double)sync->first_sync + SSTV_ROWS_SYNC_BIAS;
    }

    double at, period;
    if (locked) {
        /* phase and period from the syncs; the row number of the last sync
         * from the row-0 estimate (a whole number, robust) */
        double last = (double)sync->last_sync + SSTV_ROWS_SYNC_BIAS;
        double row = floor((last - r->row0) / r->nominal + 0.5);
        period = sync->period;
        at = last + ((double)r->next - row) * period;
    } else {
        period = r->nominal;
        at = r->row0 + (double)r->next * period;
    }
    /* any mode's row lies within [sync - 0.8 line, sync + 1.1 line] */
    long a = (long)floor(at - 0.8 * period) - ROWS_MARGIN;
    long b = (long)ceil(at + 1.1 * period) + ROWS_MARGIN;
    if (b > written) return false;
    if (a < 0) a = 0;
    r->at = at;
    r->scale = period / r->nominal;
    r->from = *from = a;
    r->to = *to = b;
    return true;
}

int sstv_rows_draw(sstv_rows_t *r, const float *audio, uint8_t *rgb_row)
{
    int    row = r->next++ * r->per_line;
    long   n = r->to - r->from;
    float *hz = sstv_fm(audio, n);

    if (!hz) return -1;
    bool ok = draw(r->mode, hz, n, lround(r->at) - r->from, r->scale, r->width, true, rgb_row);
    free(hz);
    return ok ? row : -1;
}
