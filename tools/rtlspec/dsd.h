/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - dsd-fme (lwvmobile, ISC; tools/dsdfme: built against libmbe-dvsi, its voice through the USB-3000 and
 *  nothing else) on the channel B listens to in narrow FM, for the digital voice the daemon has no decoder of its own
 *  for: P25 phases 1 and 2, D-STAR, YSF, X2-TDMA (its -fa; DMR left to dmr.c, DSD_NO_DMR). The channel's FM (the
 *  discriminator) into its stdin at 48 kHz; what it hears back on a pipe (DSD_STATUS_FD: "SY 0 <sync>" while it has a
 *  sync, "EV <slot> <call>" while someone talks); its speech (UDP, 8 kHz) out at 44.1 kHz in the FM's place while it
 *  hears a digital signal. Started at the first narrow FM sample, ended a minute after the last; /usr/bin/dsd-fme or
 *  RTLSPEC_DSD's path, none with RTLSPEC_NO_DSD.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What is heard changed (the protocol "" when none, the call "" when no one talks): said again each second while it
 * goes on. On dsd.c's own thread */
typedef void (*dsd_heard_fn)(const char *proto, int slot, const char *call);

/* Its thread started: false with no dsd-fme (or RTLSPEC_NO_DSD), and nothing fed to it then */
bool dsd_init(dsd_heard_fn heard);

/* One 44.1 kHz sample of the narrow FM channel (+-1 for +-5 kHz): from the stick's thread, never waits */
void dsd_feed(float d);

/* A digital signal heard in the last second: its speech (or silence) in place of the FM's buzz */
bool dsd_active(void);

/* Its speech at 44.1 kHz, a sample (about +-1), 0 when none */
float dsd_audio(void);
