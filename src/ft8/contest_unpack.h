/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  FT8/FT4 messages ft8_lib does not unpack, shown as WSJT-X shows them
 *  (lib/77bit/packjt77.f90 unpack77, WSJT-X 2.7.0, GPL v3, K1JT and the WSJT-X team)
 */

#pragma once

#include <stdbool.h>
#include <ft8lib/message.h>

/* The longest message WSJT-X shows, 37 characters, and the NUL: an EU VHF contest exchange between two portable
 * stations, "<DL1ABC/P> <PA3XYZ/P> R 590003 JO31AB", does not fit FTX_MAX_MESSAGE_LENGTH (35) */
#define CONTEST_TEXT_LENGTH 38

/* Into text (CONTEST_TEXT_LENGTH):
 * 0.3, 0.4  ARRL Field Day      "WA9XYZ KA1ABC R 16A EMA"
 * 0.6       WSPR-style          "K1ABC FN42 37", "PJ4/K1ABC 37", "<PJ4/K1ABC> FN42AX"
 * i3=3      ARRL RTTY Roundup   "TU; W9XYZ K1ABC R 579 MA", "W9XYZ G8ABC 559 0013"
 * i3=5      EU VHF contest      "<PA3XYZ> <G4ABC/P> R 590003 IO91NP"
 * Hashed calls are looked up in hash_if, the table the decoder gives ft8_lib, and "<...>" when not heard.
 * False for any other kind (0.2, the EU VHF contest's first form, is none now) and for one WSJT-X does not show;
 * then nothing goes into the table */
bool contest_unpack(const ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, char *text);
