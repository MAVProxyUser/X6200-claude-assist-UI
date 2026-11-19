/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  The bench suite's line across the top of the screen: what test runs, which radio sends and which listens
 *  (tools/bench: suite.py and bench.py set it over the remote protocol's "banner TEXT"). On the LVGL thread.
 */

#pragma once

/* TEXT shown, with this radio's own TX or RX in front (from its transmit state, live); "" or NULL takes it away */
void bench_banner_set(const char *text);
