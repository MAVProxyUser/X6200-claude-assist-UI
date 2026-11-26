/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * src/nifog.c's nifog_load against files on disk (BOARD_SHARE given by the Makefile: build/share), said for
 * nifog_test.py: "load 1 pages 192" or "load 0 pages 0"; a second load after nifog_forget the same way
 */
#include "nifog.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *wefax_png_read_grey(const char *path, unsigned *w, unsigned *h) {   /* a page asked for: said, not read */
    printf("read %s\n", path);
    *w = *h = 1;
    return calloc(1, 1);
}

int main(void) {
    bool     ok = nifog_load();
    unsigned w, h;

    printf("load %d pages %d edition %s\n", ok, nifog_pages(), nifog_edition());
    if (ok) {
        uint8_t *g = nifog_page_grey(nifog_pages(), &w, &h);     /* the last page's file, by its name */

        free(g);
        g = nifog_page_grey(nifog_pages() + 1, &w, &h);           /* past the last: none */
        printf("past %s\n", g ? "read" : "none");
        free(g);
    }
    return 0;
}
