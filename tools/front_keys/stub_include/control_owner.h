/* tools/front_keys: who controls the radio, not asked here */
#pragma once
#include <stdint.h>
void ctl_local_input(void);
void ctl_local_event(int64_t sec, int64_t usec);
void ctl_local_from(const char *what);
void ctl_local_event_from(int64_t sec, int64_t usec, const char *what);
