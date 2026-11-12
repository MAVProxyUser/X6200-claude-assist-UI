/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the contest messages (src/ft8/contest_unpack.c); see the Makefile.
 *
 *  Put in front of ft8_lib's demo/decode_ft8.c as it is compiled (cc -include): its ftx_message_decode() is the
 *  worker's (src/ft8/worker.c decode_messages), ft8_lib's then Hound's then ours, and its text buffer takes the
 *  37 characters as the worker's does. The demo itself stays as ft8_lib has it.
 */

#pragma once

#include <stdio.h>
#include <ft8/message.h>

ftx_message_rc_t worker_message_decode(const ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, char *text);

#undef FTX_MAX_MESSAGE_LENGTH
#define FTX_MAX_MESSAGE_LENGTH 38                   /* CONTEST_TEXT_LENGTH */
#define ftx_message_decode     worker_message_decode
