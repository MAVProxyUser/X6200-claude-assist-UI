/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  This fork's own releases, looked for once a day: rootfs/etc/init.d/S99updatecheck runs `x6100-update check`
 *  into /mnt/update/check.json, and the GUI says a newer OpenX6X00 once per version and shows it on Settings > Info
 *  next to Update. x6100-update takes only this radio's own bundle (-x6100- or -x6200- in its name), never the other
 *  radio's. Nothing is downloaded or installed by this: Settings > Info > Update does that, when asked.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The repository the radio updates from, owner/name (/usr/share/x6100/update-repo, written by the build) */
const char *update_repo(void);

/* The daily check's word for Settings > Info: "0.3.9.2 out", "" when there is none newer or no check yet */
void update_watch_status(char *buf, size_t n);

/* A newer release said on the screen once per version: a minute after the start, then every ten minutes */
void update_watch_init(void);

#ifdef __cplusplus
}
#endif
