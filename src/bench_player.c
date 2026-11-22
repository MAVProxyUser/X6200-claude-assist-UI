/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  GoldCorpHF - the bench's file player: a WAV the bench copied to /tmp played on this radio's own speaker, through
 *  the base's play mode as a voice message's preview plays (voice_messages.c), asked over the browser remote ("play
 *  PATH", "play stop"). For tools/bench/acoustic.py (the owner, 2026-11-22: stepping tones from one radio's speaker
 *  into the other's mic on G, "it should have been the internal RX", never the RTL stick): no SDR is needed for the
 *  speaker to sound. Only /tmp/bench_*.wav, mono at the play rate; never while this radio transmits, and it stops if
 *  the radio keys; the play mode it found is put back after.
 */

#include "bench_player.h"

#include <pthread.h>
#include <sndfile.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "radio.h"

static atomic_bool busy, stop_req;
static char        path[96];

static void *worker_cb(void *arg) {
    (void)arg;
    SF_INFO  info;
    SNDFILE *f;

    memset(&info, 0, sizeof(info));
    f = sf_open(path, SFM_READ, &info);
    if (f && info.channels == 1 && info.samplerate == AUDIO_PLAY_RATE) {
        audio_play_mode_t was = radio_play_mode();
        int16_t           buf[1024];

        radio_set_play_mode(AUDIO_PLAY_ON);
        while (!atomic_load(&stop_req) && radio_get_state() == RADIO_RX) {
            int n = (int)sf_read_short(f, buf, sizeof(buf) / sizeof(buf[0]));

            if (n <= 0) break;
            audio_play(buf, (size_t)n);
        }
        if (atomic_load(&stop_req) || radio_get_state() != RADIO_RX)
            audio_play_flush();
        else
            audio_play_wait();
        radio_set_play_mode(was);
    } else {
        fprintf(stderr, "bench player: %s: not a mono %d Hz WAV\n", path, AUDIO_PLAY_RATE);
    }
    if (f) sf_close(f);
    atomic_store(&busy, false);
    return NULL;
}

bool bench_player_start(const char *p) {
    size_t n = p ? strlen(p) : 0;

    if (n < 16 || n >= sizeof(path) || strncmp(p, "/tmp/bench_", 11) || strcmp(p + n - 4, ".wav") || strstr(p, "..") ||
        strchr(p + 5, '/'))
        return false;                       /* /tmp/bench_NAME.wav, nothing else (no folder under /tmp) */
    if (access(p, R_OK) != 0)
        return false;                       /* not there: refused now, not found later by the worker in silence */
    if (radio_get_state() != RADIO_RX || atomic_exchange(&busy, true))
        return false;
    snprintf(path, sizeof(path), "%s", p);
    atomic_store(&stop_req, false);

    pthread_t      t;
    pthread_attr_t at;

    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&t, &at, worker_cb, NULL) == 0;

    pthread_attr_destroy(&at);
    if (!ok) atomic_store(&busy, false);
    return ok;
}

void bench_player_stop(void) {
    atomic_store(&stop_req, true);
}

bool bench_player_busy(void) {
    return atomic_load(&busy);
}
