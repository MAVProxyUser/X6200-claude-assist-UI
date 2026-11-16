/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the AMBE+2 vocoder for DMR's voice (dmr.c's frames): a DVSI AMBE-3000R in a USB-3000 (an FTDI serial
 *  port, ttyUSB) on the HOST port, at rate 33 (3600 bit/s: 2450 speech, 1150 FEC, DMR's). Found by asking each FTDI
 *  port its product ID; each frame decoded on a thread of its own into 8 kHz speech, out at 44.1 kHz for the
 *  daemon's audio. No USB-3000: nothing heard (DMR's voice has no open decoder).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The thread that finds the USB-3000 and decodes: started once */
void ambe_start(void);

/* A USB-3000 found, set up and answering */
bool ambe_ready(void);

/* One DMR frame (72 bits as sent, MSB first) to decode; dropped with no USB-3000 */
void ambe_frame(const uint8_t frame[9]);

/* The decoded speech at 44.1 kHz, a sample (about +-1): 0 while none is there (a call not begun, or ended) */
float ambe_audio(void);

/* A new call: what is left of the last one's speech dropped */
void ambe_flush(void);
