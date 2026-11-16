/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the network's rigctld (#178): Hamlib's own protocol for "Hamlib NET rigctl" (model 2), so WSJT-X,
 *  fldigi, JS8Call, GPredict and the loggers follow and tune the radio over the network with no cable. It shares
 *  winlink.c's server on port 4532: Pat (Winlink) on the radio itself keeps its own rules there; every other peer is
 *  answered here. Receive only: a PTT asked is refused (RPRT -9), as is anything that would transmit
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Answers one line (its CR LF cut) into reply, "" for none; false: the client says goodbye. Any thread: what it sets
 * is done on the LVGL thread and waited for */
bool rigctld_answer(const char *line, char *reply, size_t size);

/* The network may reach it: the TCI server is on (its switch is the network CAT's). Listen only (TCI's trust) leaves
 * it reading the radio, setting nothing */
bool rigctld_network_on(void);
