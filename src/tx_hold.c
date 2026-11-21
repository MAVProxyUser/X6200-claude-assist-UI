/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - audio played as it comes through the drive loop on a base without ALC (tx_hold.h)
 */

#include "tx_hold.h"

#include <math.h>

#include "audio.h"
#include "radio.h"
#include "tx_meters.h"

static float clamp_level(float d) {
    return d > 0.0f ? 0.0f : d < TX_HOLD_FLOOR ? TX_HOLD_FLOOR : d;
}

void tx_hold_begin(tx_hold_t *h, float target_w, float level_1w) {
    *h = (tx_hold_t){.on = (radio_caps() & HW_CAP_TX_NO_ALC) != 0};
    if (!h->on)
        return;
    tx_level_start(&h->level, target_w);
    h->level.power_hold = true;
    h->scale_db   = 10.0f * log10f(target_w > TX_HOLD_MIN_W ? target_w : TX_HOLD_MIN_W);
    h->peak_dbfs  = clamp_level(level_1w + h->scale_db);
    h->start_dbfs = h->peak_dbfs;
    h->applied    = 1.0f;
    h->quiet      = true;
    tx_info_refresh(&h->seen, NULL, NULL, NULL);    /* what came before this transmission is not ours */
}

void tx_hold_audio(tx_hold_t *h, int16_t *s, size_t n) {
    float pwr, alc;

    if (!h->on)
        return;
    if (tx_info_refresh(&h->seen, &alc, &pwr, NULL)) {
        h->pwr  = pwr;
        h->alc  = alc;
        h->have = true;
    }
    /* as tx_player's loop: one tx_level block each time the audio has gone into a new TX_HOLD_BLOCK, whatever size
     * the client's frames are, a reading that came between two of them kept for the next */
    if ((uint64_t)h->level.blocks <= (h->played + TX_HOLD_BLOCK - 1) / TX_HOLD_BLOCK) {
        h->peak_dbfs = clamp_level(h->peak_dbfs + tx_level_block(&h->level, !h->quiet, h->have, h->pwr, h->alc));
        h->have  = false;
        h->quiet = true;
    }
    for (size_t i = 0; i < n; i++) {
        int a = s[i] < 0 ? -s[i] : s[i];

        if (a > h->client_pk)
            h->client_pk = a;
        h->quiet = h->quiet && s[i] == 0;
    }
    h->played += n;
    if (h->client_pk == 0)
        return;     /* silence so far: nothing to bring anywhere */

    /* The level climbs no higher than the client's own peak: over it the audio is not raised, so steps up there
     * bring nothing, and learned they would wait for the client's slider to go up, then let its audio out that much
     * hotter. Never pulled under where it started, though: a voice's first word can be 30 dB under its loudest */
    float client = 20.0f * log10f(h->client_pk / 32767.0f);
    float cap    = client > h->start_dbfs ? client : h->start_dbfs;

    if (h->peak_dbfs > cap)
        h->peak_dbfs = cap;

    /* the client's peak to the level. A louder frame than any before takes the gain down at once, for the whole frame
     * (a ramp from the old gain would play its start too loud); the level moving, a ramp */
    float g = h->peak_dbfs - client;

    if (g > 0.0f)
        g = 0.0f;
    if (h->applied > 0.0f || g < h->applied - TX_LEVEL_DOWN_MAX_DB) {
        if (g != 0.0f)
            audio_gain_db(s, n, g, s);
    } else if (g != h->applied) {
        audio_gain_db_transition(s, n, h->applied, g, s);   /* a ramp, no click */
    } else if (g != 0.0f) {
        audio_gain_db(s, n, g, s);
    }
    h->applied = g;
}

float tx_hold_end(tx_hold_t *h, float level_1w) {
    if (!h->on)
        return level_1w;
    h->on = false;
    /* as tx_player keeps the apps': a level found (held at the setting, or brought down from over it), or one still
     * climbing, kept; one that never climbed (silence, too short to judge) leaves the last as it was */
    float found = h->peak_dbfs - h->scale_db;

    if (h->level.held)
        return found;
    return found > level_1w ? found : level_1w;
}
