/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The k-th smallest of a list of floats, found without sorting it all.
 */
#pragma once

/* The k-th smallest of v[0..n-1] (0 the smallest; v reordered), by Hoare's selection: a median for the price of
 * about two passes over the list where qsort paid n log n calls of its compare function. qsort did it 86 times a
 * second for G's voiceprint and 20 times a second for each of the stick's 2048-bin rows (glibc's merge sort 4% of
 * G's GUI thread, and 64% of the stick's reader thread: perf, 2026-11-15 and 2026-11-17) */
static inline float float_kth(float *v, int n, int k) {
    int lo = 0, hi = n - 1;

    while (lo < hi) {
        float p = v[k];
        int   i = lo, j = hi;

        while (i <= j) {
            while (v[i] < p) i++;
            while (v[j] > p) j--;
            if (i <= j) {
                float t = v[i];
                v[i++]  = v[j];
                v[j--]  = t;
            }
        }
        if (j < k) lo = i;
        if (k < i) hi = j;
    }
    return v[k];
}
