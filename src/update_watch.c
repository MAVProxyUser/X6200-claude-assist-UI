/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  This fork's releases, as the daily check found them (update_watch.h).
 */
#include "update_watch.h"

#include "board.h"
#include "msg.h"

#include "lvgl/lvgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_JSON   BOARD_DATA "/update/check.json"     /* x6100-update check, from S99updatecheck */
#define ANNOUNCED    BOARD_DATA "/update/announced"      /* the version it was said for */
#define REPO_FILE    BOARD_SHARE "/update-repo"
#define APP_VERSION  BOARD_SHARE "/app-version"
#define DEFAULT_REPO "MAVProxyUser/X6200-claude-assist-UI" /* x6100-update's own default */
#define JSON_MAX     32768                                /* the release notes are in it too, up to 2000 chars */

const char *update_repo(void) {
    static char repo[128];
    FILE       *f;

    if (repo[0]) return repo;
    if ((f = fopen(REPO_FILE, "r"))) {
        if (fgets(repo, sizeof(repo), f)) repo[strcspn(repo, " \t\r\n")] = '\0';
        fclose(f);
    }
    if (!repo[0] || !strchr(repo, '/')) snprintf(repo, sizeof(repo), "%s", DEFAULT_REPO);
    return repo;
}

/* x6100-update prints its JSON with Python's default separators: "key": "value", "key": true */
static void json_str(const char *json, const char *key, char *out, size_t n) {
    char        pat[64];
    const char *p;

    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    if ((p = strstr(json, pat))) {
        p += strlen(pat);
        size_t l = strcspn(p, "\"");
        snprintf(out, n, "%.*s", (int)(l < n ? l : n - 1), p);
    }
}

static bool json_true(const char *json, const char *key) {
    char        pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\": ", key);
    return (p = strstr(json, pat)) && !strncmp(p + strlen(pat), "true", 4);
}

/* The newer release the last check found, for the software running now; false when none (or the check was made
 * for another version: one installed since) */
static bool newer_release(char *latest, size_t n, bool *image) {
    char  *json = malloc(JSON_MAX), running[32], now[32] = "";
    FILE  *f;
    size_t r = 0;
    bool   newer = false;

    latest[0] = '\0';
    *image    = false;
    if (!json) return false;
    if ((f = fopen(CHECK_JSON, "r"))) {
        r = fread(json, 1, JSON_MAX - 1, f);
        fclose(f);
    }
    json[r] = '\0';
    if ((f = fopen(APP_VERSION, "r"))) {
        if (fgets(now, sizeof(now), f)) now[strcspn(now, " \t\r\n")] = '\0';
        fclose(f);
    }
    json_str(json, "running", running, sizeof(running));
    if (r && json_true(json, "newer") && !strcmp(running, now)) {
        json_str(json, "latest", latest, n);
        *image = json_true(json, "image_needed");
        newer  = latest[0] != '\0';
    }
    free(json);
    return newer;
}

void update_watch_status(char *buf, size_t n) {
    char latest[24];
    bool image;

    buf[0] = '\0';
    if (newer_release(latest, sizeof(latest), &image))
        snprintf(buf, n, "%s out%s", latest, image ? ", new card image" : "");
}

static void watch_cb(lv_timer_t *t) {
    char  latest[24], seen[24] = "";
    bool  image;
    FILE *f;

    lv_timer_set_period(t, 600000);
    if (!newer_release(latest, sizeof(latest), &image)) return;
    if ((f = fopen(ANNOUNCED, "r"))) {
        if (fgets(seen, sizeof(seen), f)) seen[strcspn(seen, "\r\n")] = '\0';
        fclose(f);
    }
    if (!strcmp(seen, latest)) return;
    msg_schedule_long_text_fmt("OpenX6X00 %s is out: Settings > Info > Update", latest);
    if ((f = fopen(ANNOUNCED, "w"))) {
        fprintf(f, "%s\n", latest);
        fclose(f);
    }
}

void update_watch_init(void) {
    lv_timer_create(watch_cb, 60000, NULL);
}
