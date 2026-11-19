/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  A Mac's stand-in for libnm's header (the Makefile): the two names src/wifi.h takes from it. radio.c includes
 *  wifi.h but calls none of libnm, so the test needs no more.
 */

#pragma once

#include <glib.h>

typedef int NM80211Mode;
