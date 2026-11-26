/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - CISA's NIFOG on the radio: the contents and the pages (nifog.h)
 */

#include "nifog.h"

#include "board.h"
#include "wefax/thumb.h"        /* wefax_png_read_grey: any PNG as grey */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIFOG_DIR     BOARD_SHARE "/nifog"
#define NIFOG_MAX_PG  400
#define NIFOG_MAX_SEC 300
#define NIFOG_IDX_MAX 65536

static bool        loaded;
static int         n_pages, n_secs, toc_page;
static char        labels[NIFOG_MAX_PG + 1][8];
static nifog_sec_t secs[NIFOG_MAX_SEC];
static char        edition[48];

void nifog_forget(void) {
    loaded  = false;
    n_pages = n_secs = toc_page = 0;
    edition[0] = 0;
    memset(labels, 0, sizeof(labels));
}

/* One line of the index (tools/nifog/render.py --index): pages N | page P LABEL | sec P LEVEL TITLE | edition TEXT */
static void parse_line(char *l) {
    int p, lv, n;

    l[strcspn(l, "\r\n")] = 0;
    if (!l[0] || l[0] == '#') return;
    if (sscanf(l, "pages %d", &p) == 1) {
        n_pages = p > 0 && p <= NIFOG_MAX_PG ? p : 0;
    } else if (!strncmp(l, "edition ", 8)) {
        snprintf(edition, sizeof(edition), "%s", l + 8);
    } else if (sscanf(l, "page %d %n", &p, &n) == 1 && p >= 1 && p <= NIFOG_MAX_PG) {
        snprintf(labels[p], sizeof(labels[p]), "%s", l + n);
    } else if (sscanf(l, "sec %d %d %n", &p, &lv, &n) == 2 && p >= 1 && n_secs < NIFOG_MAX_SEC) {
        nifog_sec_t *s = &secs[n_secs++];

        s->page  = p;
        s->level = lv;
        snprintf(s->title, sizeof(s->title), "%s", l + n);
        if (!toc_page && !strcmp(s->title, "Table of Contents")) toc_page = p;
    }
}

bool nifog_parse(const char *text) {
    char line[256];

    nifog_forget();
    while (*text) {
        size_t k = strcspn(text, "\n");
        size_t c = k < sizeof(line) - 1 ? k : sizeof(line) - 1;

        memcpy(line, text, c);
        line[c] = 0;
        parse_line(line);
        text += k + (text[k] == '\n');
    }
    for (int i = 0; i < n_secs; i++)    /* an entry past the last page: none */
        if (secs[i].page > n_pages) n_secs = i;
    loaded = n_pages > 0;
    return loaded;
}

bool nifog_load(void) {
    if (loaded) return true;
    FILE *f = fopen(NIFOG_DIR "/nifog.idx", "r");

    if (!f) return false;
    char  *buf = malloc(NIFOG_IDX_MAX + 1);
    size_t len = buf ? fread(buf, 1, NIFOG_IDX_MAX, f) : 0;

    fclose(f);
    if (!buf) return false;
    buf[len] = 0;
    bool ok = nifog_parse(buf);

    free(buf);
    if (ok) {                           /* the pages drawn, the first one at least */
        char p[512];                    /* BOARD_SHARE is a build folder on a PC: a long path */

        snprintf(p, sizeof(p), NIFOG_DIR "/p%03d.png", 1);
        FILE *t = fopen(p, "rb");

        if (t) fclose(t);
        else nifog_forget(), ok = false;
    }
    return ok;
}

int nifog_pages(void) {
    return loaded ? n_pages : 0;
}

const char *nifog_edition(void) {
    return edition;
}

const char *nifog_label(int page) {
    return page >= 1 && page <= NIFOG_MAX_PG ? labels[page] : "";
}

int nifog_secs(void) {
    return n_secs;
}

const nifog_sec_t *nifog_sec(int i) {
    return i >= 0 && i < n_secs ? &secs[i] : NULL;
}

int nifog_sec_of(int page) {
    int best = -1;

    for (int i = 0; i < n_secs; i++)    /* in page order: the last that starts here or before */
        if (secs[i].page <= page && secs[i].level <= 1) best = i;
    return best;
}

int nifog_toc_page(void) {
    return toc_page ? toc_page : 1;
}

int nifog_step(int page, int dir, int max_level) {
    int to = page;

    for (int i = 0; i < n_secs; i++) {
        const nifog_sec_t *s = &secs[i];

        if (s->level > max_level) continue;
        if (dir > 0 && s->page > page && (to == page || s->page < to)) to = s->page;
        if (dir < 0 && s->page < page && (to == page || s->page > to)) to = s->page;
    }
    return to;
}

uint8_t *nifog_page_grey(int page, unsigned *w, unsigned *h) {
    char path[512];

    *w = *h = 0;
    if (page < 1 || page > n_pages) return NULL;
    snprintf(path, sizeof(path), NIFOG_DIR "/p%03d.png", page);
    return wefax_png_read_grey(path, w, h);
}
