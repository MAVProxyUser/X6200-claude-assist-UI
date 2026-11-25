/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - CISA's NIFOG (National Interoperability Field Operations Guide) on the radio, for the Sentry's F
 *  NIFOG: its contents and its pages' printed numbers (BOARD_SHARE/nifog/nifog.idx, tools/nifog/render.py --index)
 *  and its pages, drawn at build time from CISA's PDF (p001.png ..., 4-bit grey, tools/nifog/render.py). The PDF
 *  itself is beside them, for the web configurator's /nifog page.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  page;          /* the PDF's page it starts on, 1 the first */
    int  level;         /* 0 a chapter, 1 a section, 2 a part of one */
    char title[96];     /* as the contents have it (ASCII) */
} nifog_sec_t;

/* The contents read (once; again after nifog_forget): false when the radio has none, or no page of it */
bool nifog_load(void);
void nifog_forget(void);

int  nifog_pages(void);                  /* 0 when not loaded */
const char *nifog_edition(void);         /* "2.02, January 2025"; "" when the index says none */
const char *nifog_label(int page);       /* the page's printed number ("31", "iv"), "" when it has none */
int  nifog_secs(void);
const nifog_sec_t *nifog_sec(int i);
int  nifog_sec_of(int page);             /* the last entry starting on or before page (level 0 or 1), -1 none */
int  nifog_toc_page(void);               /* where the contents are, 1 when the index does not say */

/* The page after (dir > 0) or before (dir < 0) page where an entry of level <= max_level starts, page itself when
 * there is none that way */
int  nifog_step(int page, int dir, int max_level);

/* The page as 8-bit grey (0 ink, 255 paper), malloc'ed w x h: NULL when it can't be read */
uint8_t *nifog_page_grey(int page, unsigned *w, unsigned *h);

/* The parser alone (the host test): the index's text into the tables nifog_load fills */
bool nifog_parse(const char *text);

#ifdef __cplusplus
}
#endif
