#pragma once

#include "dialog.h"

#ifdef __cplusplus
extern "C" {
#endif

extern dialog_t *dialog_cw_trainer;

/* Learn mode: the group being played or keyed back ("" when none). For tests. */
const char *dialog_cw_trainer_target(void);
/* Where the round is: "closed" (not open), "due" (Start or Replay pressed, the group a moment away), "ready",
 * "playing" (the group or call keyed), "listen" (your turn), "result". For tests (the remote's cfg? cwt_state) */
const char *dialog_cw_trainer_state(void);

#ifdef __cplusplus
}
#endif
