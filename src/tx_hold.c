/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - audio played as it comes through the digital apps' drive loop (tx_hold.h)
 */

#include "tx_hold.h"

#include <math.h>

#include "audio.h"
#include "params/params.h"
#include "radio.h"
#include "tx_meters.h"
#include "tx_player.h"

/* The level d, the samples' peak in it (d less the codec's gain) between TX_HOLD_FLOOR and full scale */
static float clamp_level(const tx_hold_t *h, float d) {
    float s = d - h->codec_db;

    return s > 0.0f ? h->codec_db : s < TX_HOLD_FLOOR ? h->codec_db + TX_HOLD_FLOOR : d;
}

void tx_hold_begin(tx_hold_t *h, float target_w, float level_1w) {
    uint32_t caps   = radio_caps();
    bool     no_alc = (caps & HW_CAP_TX_NO_ALC) != 0;

    *h = (tx_hold_t){.on = no_alc || TX_HOLD_ALC_BASE};
    if (!h->on)
        return;
    h->alc = !no_alc;
    tx_level_start(&h->level, target_w);
    h->level.power_hold = no_alc;
    h->level.coarse     = no_alc && target_w < TX_LEVEL_COARSE_W;
    /* as the apps' base gain (tx_player_base_gain_offset): with the power, but on a base that sets it itself */
    h->scale_db = no_alc || !(caps & HW_CAP_TX_LEVEL)
                      ? 10.0f * log10f(target_w > TX_HOLD_MIN_W ? target_w : TX_HOLD_MIN_W) : 0.0f;
    if (h->alc) {
        if (level_1w > 0.0f)
            level_1w = tx_player_level_1w();
        h->codec_db = audio_set_play_vol(level_1w + h->scale_db + TX_HOLD_HEAD_DB);
        h->raise_db = TX_HOLD_RAISE_DB;
    }
    h->peak_dbfs  = clamp_level(h, level_1w + h->scale_db);
    h->start_dbfs = h->peak_dbfs;
    h->quiet      = true;
    tx_info_refresh(&h->seen, NULL, NULL, NULL);    /* what came before this transmission is not ours */
}

void tx_hold_audio(tx_hold_t *h, int16_t *s, size_t n) {
    float pwr, alc;

    if (!h->on)
        return;
    if (tx_info_refresh(&h->seen, &alc, &pwr, NULL)) {
        h->pwr      = pwr;
        h->alc_read = alc;
        h->have     = true;
    }
    /* as tx_player's loop: one tx_level block each time the audio has gone into a new TX_HOLD_BLOCK, whatever size
     * the client's frames are, a reading that came between two of them kept for the next */
    if ((uint64_t)h->level.blocks <= (h->played + TX_HOLD_BLOCK - 1) / TX_HOLD_BLOCK) {
        h->peak_dbfs = clamp_level(h, h->peak_dbfs + tx_level_block(&h->level, !h->quiet, h->have, h->pwr,
                                                                    h->alc_read));
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
    h->sounded += n;

    /* The level climbs no higher than the client's own peak, raised as far as it may be: over it steps up bring
     * nothing, and learned they would wait for the client's slider to go up, then let its audio out that much hotter.
     * Never pulled under where it started, though: a voice's first word can be 30 dB under its loudest. In the
     * samples' terms: the codec's gain out */
    float client = 20.0f * log10f(h->client_pk / 32767.0f);
    float start  = h->start_dbfs - h->codec_db;
    float cap    = client + h->raise_db > start ? client + h->raise_db : start;

    if (h->peak_dbfs - h->codec_db > cap)
        h->peak_dbfs = cap + h->codec_db;

    /* the client's peak to the level. A louder frame than any before takes the gain down at once, for the whole frame
     * (a ramp from the old gain would play its start too loud); the level moving, a ramp */
    float g = h->peak_dbfs - h->codec_db - client;

    if (g > h->raise_db)
        g = h->raise_db;
    /* a base with ALC: the fade in, at the end of this frame (its start ramped from the last frame's) */
    float fade_n = TX_HOLD_FADE_S * AUDIO_PLAY_RATE;

    if (h->alc && h->sounded < fade_n)
        g -= TX_HOLD_FADE_DB * (1.0f - h->sounded / fade_n);
    if (!h->started || g < h->applied - TX_LEVEL_DOWN_MAX_DB) {
        if (g != 0.0f)
            audio_gain_db(s, n, g, s);
    } else if (g != h->applied) {
        audio_gain_db_transition(s, n, h->applied, g, s);   /* a ramp, no click */
    } else if (g != 0.0f) {
        audio_gain_db(s, n, g, s);
    }
    h->applied = g;
    h->started = true;
}

float tx_hold_end(tx_hold_t *h, float level_1w) {
    if (!h->on)
        return level_1w;
    h->on = false;
    float found = h->peak_dbfs - h->scale_db;

    if (h->alc) {
        audio_set_play_vol(params.play_gain_db_f.x);   /* the playback volume of the settings, as tx_player */
        /* as tx_player keeps the apps' on a base with ALC: what it found; one that decided nothing (too short to
         * judge, or silence) leaves the last as it was, TX_HOLD_UNSET too: the next from the apps' level again */
        return h->level.windows > 0 ? found : level_1w;
    }
    /* Under 1 W the readings found nothing (tx_level.h, TX_LEVEL_COARSE_W): the level for 1 W kept, unless a clear
     * over brought it down. Kept as found, the meter's high readings after the key took it down a little each over, 4
     * dB on the bench at 0.1 W (2026-11-24) */
    if (h->level.coarse)
        return found < level_1w ? found : level_1w;
    /* as tx_player keeps the apps': a level found (held at the setting, or brought down from over it), or one still
     * climbing, kept; one that never climbed (silence, too short to judge) leaves the last as it was */
    if (h->level.held)
        return found;
    return found > level_1w ? found : level_1w;
}
