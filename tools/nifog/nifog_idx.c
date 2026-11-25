/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * src/nifog.c's reading of an index (tools/nifog/nifog.idx), said line by line for nifog_test.py to compare with the
 * web page's (webserver nifog_web.py): nifog_idx FILE
 */
#include "nifog.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *wefax_png_read_grey(const char *path, unsigned *w, unsigned *h) {   /* the pages: not read here */
    (void)path;
    *w = *h = 0;
    return NULL;
}

int main(int argc, char **argv) {
    static char buf[1 << 17];
    FILE       *f = argc > 1 ? fopen(argv[1], "r") : NULL;

    if (!f) return 2;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);

    fclose(f);
    buf[n] = 0;
    printf("parsed %d\n", nifog_parse(buf));
    printf("pages %d\nedition %s\ntoc %d\n", nifog_pages(), nifog_edition(), nifog_toc_page());
    for (int i = 0; i < nifog_secs(); i++) {
        const nifog_sec_t *s = nifog_sec(i);

        printf("sec %d %d %s\n", s->page, s->level, s->title);
    }
    for (int p = 1; p <= nifog_pages(); p++)
        printf("page %d label=%s sec=%d back=%d next=%d chapter=%d\n", p, nifog_label(p), nifog_sec_of(p),
               nifog_step(p, -1, 1), nifog_step(p, 1, 1), nifog_step(p, 1, 0));
    return 0;
}
