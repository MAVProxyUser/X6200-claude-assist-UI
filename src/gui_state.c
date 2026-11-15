/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - The GUI's state across its own restart (gui_state.h).
 *
 *  A crash, an update, a hot install, a restart of S95gui or a power-off brought the GUI back at its start screen
 *  (the Sentry's A, its scans off) whatever it had been doing. Now each place keeps what it has going under a few keys
 *  (gui_state_set: the Sentry from its half-second timer, the main screen the place itself), and the next GUI reads
 *  them back and opens there as it was. They go to two files: /tmp/gui_state.conf within 2 s of a change (the RAM's:
 *  a crash loses nothing, and the card is not written each time), and /mnt/gui_state.conf on DATA at most once a
 *  minute and at the power-off (gui_state_flush), for the next time the radio is switched on. A start reads the
 *  first if it is there (the same boot), else the second. Settings themselves (the dial, the volumes, each screen's
 *  LISTEN and view) were kept already, in params.db.
 *
 *  Not kept on purpose: an armed transmitter (A/B on A and G), a lock on a signal, what an app had on the air.
 *  A start that restores is counted in /tmp/gui_state.tries, cleared after a minute up: two restores in a row that
 *  did not last a minute, and the third start is a clean one (a state that crashes the GUI is not one to go back to)
 */

#include "gui_state.h"

#include "lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STATE_FILE "/tmp/gui_state.conf"        /* this boot's, within 2 s of a change */
#define KEEP_FILE  "/mnt/gui_state.conf"        /* DATA's, over a power-off: a minute at most behind */
#define KEEP_MS    60000
#define TRIES_FILE "/tmp/gui_state.tries"
#define KEYS       64

static struct {
    char    k[24];
    int32_t v;
} kv[KEYS];
static int      nkv, start_place;
static bool     dirty, keep_dirty, restoring;
static uint32_t kept_ms;

static int find(const char *k) {
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].k, k)) return i;
    return -1;
}

static int add(const char *k, int32_t v) {
    if (nkv == KEYS || strlen(k) >= sizeof(kv[0].k)) return -1;
    snprintf(kv[nkv].k, sizeof(kv[nkv].k), "%s", k);
    kv[nkv].v = v;
    return nkv++;
}

int32_t gui_state_get(const char *key, int32_t def) {
    int i = find(key);

    return i < 0 ? def : kv[i].v;
}

void gui_state_set(const char *key, int32_t val) {
    int i = find(key);

    if (i < 0) {
        if (add(key, val) >= 0) dirty = keep_dirty = true;
    } else if (kv[i].v != val) {
        kv[i].v = val;
        dirty = keep_dirty = true;
    }
}

bool gui_state_restoring(void) {
    return restoring;
}

int gui_state_start_place(void) {
    return start_place;
}

/* Written whole, to a new file renamed over the old one: a crash while writing leaves the last whole one. DATA's
 * (vfat) is on the card before the rename too, so a power cut leaves one whole file or the other (read_file) */
static bool write_file(const char *path, bool durable) {
    char  tmp[48];
    FILE *f;
    bool  ok = true;

    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return false;
    for (int i = 0; i < nkv && ok; i++) ok = fprintf(f, "%s=%d\n", kv[i].k, (int)kv[i].v) > 0;
    ok = fflush(f) == 0 && ok;
    if (durable) ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

static void write_cb(lv_timer_t *t) {
    (void)t;
    if (dirty && write_file(STATE_FILE, false)) dirty = false;
    if (keep_dirty && lv_tick_elaps(kept_ms) >= KEEP_MS) {
        kept_ms = lv_tick_get();        /* tried: once a minute, also when DATA cannot be written */
        if (write_file(KEEP_FILE, true)) keep_dirty = false;
    }
}

void gui_state_flush(void) {
    if (dirty && write_file(STATE_FILE, false)) dirty = false;
    if (keep_dirty && write_file(KEEP_FILE, true)) keep_dirty = false;
}

/* The keys of a file, those not read already; how many */
static int read_file(const char *path) {
    FILE *f = fopen(path, "r");
    char  line[80];
    int   n = 0;

    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');

        if (!eq) continue;
        *eq = 0;
        if (find(line) < 0 && add(line, (int32_t)strtol(eq + 1, NULL, 10)) >= 0) n++;
    }
    fclose(f);
    return n;
}

/* A minute up: this start lasted, the next restart may restore again */
static void settled_cb(lv_timer_t *t) {
    (void)t;
    unlink(TRIES_FILE);
}

void gui_state_init(void) {
    const char *from = STATE_FILE;
    FILE       *f;
    int         tries = 0;

    if (!read_file(STATE_FILE) && !read_file(from = KEEP_FILE)) read_file(from = KEEP_FILE ".new");
    if ((f = fopen(TRIES_FILE, "r"))) {
        if (fscanf(f, "%d", &tries) != 1) tries = 0;
        fclose(f);
    }
    restoring = nkv > 0 && tries < 2;
    if (restoring) {
        if ((f = fopen(TRIES_FILE, "w"))) {
            fprintf(f, "%d\n", tries + 1);
            fclose(f);
        }
        start_place = gui_state_get("place", 0);
        fprintf(stderr, "gui state: %d keys from %s: back where it was\n", nkv, from);
    } else {
        if (nkv) fprintf(stderr, "gui state: not restored, two restarts in a row each went down within a minute\n");
        nkv = 0;                        /* a clean start, kept afresh from here */
    }
    kept_ms = lv_tick_get();
    lv_timer_create(write_cb, 2000, NULL);
    lv_timer_set_repeat_count(lv_timer_create(settled_cb, 60000, NULL), 1);
}
