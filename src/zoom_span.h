/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the main screen's span by its zoom (#131). Zoom 1, 2, 4 and 8 show 100 kHz over the zoom, as ever.
 *  The X6200's base sends its spectrum up to 384 kHz wide, so it has two wider steps, kept in the same setting as
 *  zoom 0 (192 kHz: the stream zoom 1 asks for, shown whole) and -1 (384 kHz: the base's widest). The X6100's base
 *  gives 100 kHz: zoom 1 is its widest.
 */

#pragma once

#include <stdint.h>
#include <stdio.h>

#define ZOOM_SPAN_HZ 100000         /* zoom 1 */
#define ZOOM_MAX     8
#ifdef BOARD_X6200
#define ZOOM_MIN     (-1)
#else
#define ZOOM_MIN     1
#endif

/* The span shown at a zoom, Hz */
static inline int32_t zoom_span_hz(int32_t zoom) {
    return zoom >= 1 ? ZOOM_SPAN_HZ / zoom : zoom == 0 ? 192000 : 384000;
}

/* One step: d > 0 narrower (more zoom), d < 0 wider; -1 0 1 2 4 8, held within ZOOM_MIN and ZOOM_MAX */
static inline int32_t zoom_step(int32_t zoom, int d) {
    for (; d > 0; d--) zoom = zoom < 1 ? zoom + 1 : zoom * 2;
    for (; d < 0; d++) zoom = zoom > 1 ? zoom / 2 : zoom - 1;
    return zoom < ZOOM_MIN ? ZOOM_MIN : zoom > ZOOM_MAX ? ZOOM_MAX : zoom;
}

/* How it is named: "x2", or the span for the wide steps ("384 kHz") */
static inline const char *zoom_name(int32_t zoom, char *buf, int n) {
    if (zoom >= 1) snprintf(buf, (size_t)n, "x%d", (int)zoom);
    else snprintf(buf, (size_t)n, "%d kHz", (int)(zoom_span_hz(zoom) / 1000));
    return buf;
}
