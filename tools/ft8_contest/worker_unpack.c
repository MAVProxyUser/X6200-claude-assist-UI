/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - host test of the contest messages (src/ft8/contest_unpack.c); see the Makefile.
 *
 *  The unpacking of src/ft8/worker.c decode_messages(), for ft8_lib's demo decoder (worker_unpack.h).
 *  CONTEST_UNPACK=0 in the environment leaves ours out: the decoder as it was.
 */

#include "worker_unpack.h"

#include <stdlib.h>
#include <string.h>

#include "contest_unpack.h"
#include "hound.h"

#undef ftx_message_decode

ftx_message_rc_t worker_message_decode(const ftx_message_t *msg, ftx_callsign_hash_interface_t *hash_if, char *text) {
    const char      *off = getenv("CONTEST_UNPACK");
    ftx_message_rc_t rc  = ftx_message_decode(msg, hash_if, text);

    if (rc == FTX_MESSAGE_RC_ERROR_TYPE &&
        (hound_unpack_dxpedition(msg, text) || ((!off || strcmp(off, "0")) && contest_unpack(msg, hash_if, text))))
        rc = FTX_MESSAGE_RC_OK;
    return rc;
}
