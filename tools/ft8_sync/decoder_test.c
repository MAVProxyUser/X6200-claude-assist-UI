/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Host test of the times Auto Sync goes by (src/ft8/time_sync.h), with the real decoder (src/ft8/worker.c and
 *  ft8_lib) and the real decoder thread (src/ft8/audio_worker.c).
 *
 *  1. The decoder's lag: an FT8 and an FT4 message made by the app's own encoder, put at 64 places from 0.3 to 1.1 s
 *     into the decoder's audio, over noise. The time it gives is late by FTX_SYNC_DECODER_LAG (1.2 symbols) on
 *     average (within 0.1 symbol), each within 0.6 symbol of it (its time step is a quarter symbol).
 *  2. The decoder thread, in real time: FT4 audio in fragments of 0.1 s as PulseAudio gives them, a signal 0.3 s late
 *     in every slot by the clock, and every other slot the thread kept 0.3 to 0.5 s past the slot's end (a long last
 *     decode, a busy radio). This fork files each block in the slot it came in during (066e465), so the slot after
 *     a held one still starts on time, nothing of it drained: the time the decoder gives stays put, and the DT from
 *     the audio's stamp (slot_info_t's rx_start_ns) is 0.3 s in both kinds of slot. (Upstream's worker started that
 *     slot late, by the hold, and its test asked for that.) Takes about 30 s.
 *
 *  3. A strong station on a noise skirt: the X6100's audio of the X6200's CQ at 0.1 W from TX Hz 2000 (orch's
 *     ft8-copies x6200, 2026-11-25; radio_captures/bench/ft8_x6200_skirt_cq_2000hz.wav, 11025 Hz). At that power
 *     the X6200's base puts a skirt around it, -25 dBc in each 55 Hz out to 170 Hz either side, holding weak copies
 *     of the message at any offset in it. The decoder must give the station once, at its own frequency (2006 Hz).
 *     ft8_lib's ftx_delete_candidates had the candidates out of their order after a pass: a copy 25 tones off, at
 *     1853 Hz, was tried first and given, the station then a duplicate (the X6100 listed the X6200's answers 156 Hz
 *     off, and FT8's Hold off moved its TX Hz there).
 *
 *    decoder_test
 */

#include "ft8/audio_worker.h"
#include "ft8/ft2.h"
#include "ft8/time_sync.h"
#include "ft8/utils.h"
#include "ft8/worker.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE    11025               /* the app's: 44.1 kHz / 4 */
#define NS      1000000000LL
#define FT8     ((int)FTX_PROTOCOL_FT8)
#define FT4     ((int)FTX_PROTOCOL_FT4)

static int bad, cases;

static void check(bool ok, const char *what) {
    cases++;
    if (!ok) {
        printf("WRONG %s\n", what);
        bad++;
    }
}

/* audio_worker.c's milliseconds (util.h) */
uint64_t get_time(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static float noise(void) {
    return 0.01f * ((float)rand() / (float)RAND_MAX - 0.5f);
}

/* ---- 3. a strong station on a noise skirt ---- */

#define SKIRT_WAV CAPTURES "/bench/ft8_x6200_skirt_cq_2000hz.wav"

static int   skirt_n;
static float skirt_f;

static void skirt_cb(const char *text, int snr, float freq_hz, float time_sec, void *user) {
    (void)text, (void)snr, (void)time_sec, (void)user;
    if (!skirt_n++) skirt_f = freq_hz;
}

/* A 16-bit mono WAV at RATE: its samples as float, the count in *n; NULL if it is not one */
static float *read_wav(const char *path, int *n) {
    FILE    *f = fopen(path, "rb");
    uint8_t  h[44];
    float   *x = NULL;

    if (!f) return NULL;
    if (fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4) && h[22] == 1 &&
        h[34] == 16 && (uint32_t)(h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24) == RATE) {
        uint32_t bytes = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
        int16_t *pcm   = malloc(bytes);

        *n = (int)(bytes / 2);
        x  = malloc(*n * sizeof(float));
        if (pcm && x && fread(pcm, 2, *n, f) == (size_t)*n) {
            for (int k = 0; k < *n; k++) x[k] = pcm[k] / 32768.0f;
        } else {
            free(x);
            x = NULL;
        }
        free(pcm);
    }
    fclose(f);
    return x;
}

static void decoder_skirt(void) {
    int    n  = 0;
    float *x  = read_wav(SKIRT_WAV, &n);
    char   what[160];

    if (!x) {
        check(false, "the skirt recording read (" SKIRT_WAV ")");
        return;
    }
    ftx_worker_init(RATE, FT8);
    ftx_worker_reset();
    skirt_n = 0;
    int bs = ftx_worker_get_block_size();
    for (int p = 0; p + bs <= n && !ftx_worker_is_full(); p += bs) {
        ftx_worker_put_rx_samples(x + p, bs);
        ftx_worker_decode(skirt_cb, false, NULL);
    }
    ftx_worker_decode(skirt_cb, true, NULL);
    snprintf(what, sizeof(what), "the X6200's 0.1 W CQ on its skirt given once at its own 2006 Hz (%d decodes, the "
             "first at %.1f Hz; a copy 25 tones off is 1850 or 2162)", skirt_n, skirt_n ? skirt_f : 0.0f);
    bool ok = skirt_n == 1 && fabsf(skirt_f - 2006.0f) <= 10.0f;

    check(ok, what);
    if (ok) printf("ok    %s\n", what);
    free(x);
    ftx_worker_free();
}

/* ---- 1. the decoder's lag ---- */

static float found_t;
static int   found;

static void lag_cb(const char *text, int snr, float freq_hz, float time_sec, void *user) {
    (void)text, (void)snr, (void)freq_hz, (void)user;
    if (!found++) found_t = time_sec;
}

static void decoder_lag(int proto) {
    float    symbol = proto == FT8 ? FT8_SYMBOL_PERIOD : FT4_SYMBOL_PERIOD, sum = 0.0f, worst = 0.0f;
    int16_t *tx;
    uint32_t ntx;
    int      decoded = 0;
    char     what[96];

    ftx_worker_init(RATE, proto);
    if (!ftx_worker_generate_tx_samples("CQ ON6URE JO21", 1500, RATE, &tx, &ntx)) {
        check(false, "the encoder makes the message");
        ftx_worker_free();
        return;
    }

    int    n   = (int)(ftx_slot_time(proto) * RATE);
    float *buf = malloc(n * sizeof(float));
    int    bs  = ftx_worker_get_block_size();

    for (int i = 0; i < 64; i++) {
        float start = 0.3f + i * 0.0123f;
        int   s0    = (int)lroundf(start * RATE);

        srand(i + 1);
        for (int k = 0; k < n; k++) buf[k] = noise();
        for (uint32_t k = 0; k < ntx && s0 + (int)k < n; k++) buf[s0 + k] += tx[k] / 32768.0f * 0.1f;
        ftx_worker_reset();
        found = 0;
        for (int p = 0; p + bs <= n && !ftx_worker_is_full(); p += bs) {
            ftx_worker_put_rx_samples(buf + p, bs);
            ftx_worker_decode(lag_cb, false, NULL);
        }
        ftx_worker_decode(lag_cb, true, NULL);
        if (!found) continue;
        decoded++;

        float late = (found_t - start) / symbol;    /* symbols */

        sum += late;
        if (fabsf(late - FTX_SYNC_DECODER_LAG) > worst) worst = fabsf(late - FTX_SYNC_DECODER_LAG);
    }
    free(buf);
    free(tx);
    ftx_worker_free();

    if (decoded)
        printf("  %s: the decoder's time late by %.3f symbols on average (%.3f s), at most %.3f symbols off that\n",
               ftx_protocol_name(proto), sum / decoded, sum / decoded * symbol, worst);
    snprintf(what, sizeof(what), "%s: all 64 decoded (%d)", ftx_protocol_name(proto), decoded);
    check(decoded == 64, what);
    if (!decoded) return;
    snprintf(what, sizeof(what), "%s: late by %.3f symbols on average (%.2f s)", ftx_protocol_name(proto),
             sum / decoded, sum / decoded * symbol);
    check(fabsf(sum / decoded - FTX_SYNC_DECODER_LAG) <= 0.1f, what);
    snprintf(what, sizeof(what), "%s: each within 0.6 symbol of the lag (%.3f)", ftx_protocol_name(proto), worst);
    check(worst <= 0.6f, what);
}

/* ---- 2. the decoder thread in real time ---- */

#define LATE_S      0.3f    /* the signal's DT, every slot */
#define FRAGMENT    1103    /* 0.1 s */
#define MAX_HEARD   16

static struct {
    pthread_mutex_t mux;
    int             n;
    float           dt[MAX_HEARD], into[MAX_HEARD], time_sec[MAX_HEARD];
    bool            after_hold[MAX_HEARD];  /* an even slot: the thread was held past the end of the one before */
} heard = {.mux = PTHREAD_MUTEX_INITIALIZER};

static int16_t        *ft4_tx;
static uint32_t        ft4_ntx;
static int64_t         base_s;      /* a minute's start: sample n is base_s + n / RATE */
static int64_t         start_ns;    /* when the audio began (the feeder's first sample) */
static atomic_bool     feeding;
static audio_worker_t *worker;

static void on_message(const char *text, int snr, float freq_hz, float time_sec, const slot_info_t *info,
                       void *ctx) {
    (void)text, (void)snr, (void)freq_hz, (void)ctx;
    /* The test's first slot is a part of one when it starts after the slot's start: the decoder's audio of it begins
     * there (+0.93 s into it on a CI runner) and can still hold the whole message. Not a slot the decoder took up on
     * its own time, so not counted (it failed the checks below on about one run in eight) */
    if (info->rx_start_ns < start_ns + NS / 2) return;
    pthread_mutex_lock(&heard.mux);
    if (heard.n < MAX_HEARD && info->rx_start_ns) {
        int64_t s   = info->rx_start_ns;
        int64_t off = ftx_slot_offset_ns(s / NS, (long)(s % NS), ftx_slot_time(FT4));

        heard.dt[heard.n]       = ftx_sync_dt(FT4, info->rx_start_ns, time_sec);
        heard.into[heard.n]     = (float)off / 1e9f;
        heard.time_sec[heard.n] = time_sec;
        heard.after_hold[heard.n] = !info->odd;
        heard.n++;
    }
    pthread_mutex_unlock(&heard.mux);
}

/* Every other slot the thread is kept past the slot's end */
static void on_tick(const slot_info_t *info, bool new_slot, float sec, void *ctx) {
    (void)ctx;
    if (!new_slot && info->odd && sec > ftx_slot_time(FT4) - 0.2f) usleep(500000);
}

/* PulseAudio's fragments, each fed as its last sample comes: the signal LATE_S after 0.5 s into each slot */
static void *feeder(void *arg) {
    (void)arg;
    struct timespec now;
    float           frag[FRAGMENT];

    clock_gettime(CLOCK_REALTIME, &now);

    int64_t n = (int64_t)(now.tv_sec - base_s) * RATE + (int64_t)now.tv_nsec * RATE / NS;

    while (atomic_load(&feeding)) {
        for (int i = 0; i < FRAGMENT; i++) {
            int64_t k    = n + i;
            int64_t slot = (int64_t)floor(k / (ftx_slot_time(FT4) * RATE));
            int64_t at   = llround((slot * ftx_slot_time(FT4) + FTX_SYNC_START_S + LATE_S) * RATE);

            frag[i] = noise();
            if (k >= at && k - at < (int64_t)ft4_ntx) frag[i] += ft4_tx[k - at] / 32768.0f * 0.1f;
        }
        n += FRAGMENT;

        int64_t         end = (int64_t)base_s * NS + n * NS / RATE;   /* the fragment's last sample comes */
        struct timespec t   = {.tv_sec = end / NS, .tv_nsec = end % NS};

        while (clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &t, NULL)) {
        }
        audio_worker_feed(worker, FRAGMENT, frag);
    }
    return NULL;
}

static void decoder_thread(void) {
    audio_worker_cb_t cb = {.on_message = on_message, .on_tick = on_tick};
    struct timespec   now;
    pthread_t         th;
    char              what[128];

    worker = audio_worker_create(RATE, FTX_PROTOCOL_FT4, 200, 3000, &cb);
    check(worker != NULL, "the decoder thread made");
    if (!worker) return;
    /* Made before the thread starts: the encoder and the decoder share ft8_lib's call table */
    if (!ftx_worker_generate_tx_samples("CQ ON6URE JO21", 1500, RATE, &ft4_tx, &ft4_ntx)) {
        check(false, "the encoder makes the FT4 message");
        audio_worker_destroy(worker);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    base_s   = now.tv_sec - now.tv_sec % 60;
    start_ns = (int64_t)now.tv_sec * NS + now.tv_nsec;
    atomic_store(&feeding, true);
    pthread_create(&th, NULL, feeder, NULL);
    audio_worker_start(worker);

    /* Until two slots of each kind decoded (after a held thread, and not), 45 s at most */
    int held = 0, free_run = 0;

    for (int i = 0; i < 450 && (held < 2 || free_run < 2); i++) {
        usleep(100000);
        pthread_mutex_lock(&heard.mux);
        held = free_run = 0;
        for (int k = 0; k < heard.n; k++) {
            if (heard.after_hold[k]) held++;
            else free_run++;
        }
        pthread_mutex_unlock(&heard.mux);
    }
    atomic_store(&feeding, false);
    pthread_join(th, NULL);
    audio_worker_destroy(worker);
    free(ft4_tx);

    float tmin = 99.0f, tmax = -99.0f;

    for (int k = 0; k < heard.n; k++) {
        printf("  audio from %+.3f s into its slot, the decoder's time %.3f s: DT %+.3f s\n", heard.into[k],
               heard.time_sec[k], heard.dt[k]);
        snprintf(what, sizeof(what), "audio from %+.2f s into its slot: DT %+.3f s, %.2f s within 0.05 s",
                 heard.into[k], heard.dt[k], LATE_S);
        check(fabsf(heard.dt[k] - LATE_S) <= 0.05f, what);
        snprintf(what, sizeof(what), "a slot %s: its audio from %+.2f s into it, on time (under 0.1 s)",
                 heard.after_hold[k] ? "after the thread was held" : "after a free one", heard.into[k]);
        check(fabsf(heard.into[k]) < 0.1f, what);
        if (heard.time_sec[k] < tmin) tmin = heard.time_sec[k];
        if (heard.time_sec[k] > tmax) tmax = heard.time_sec[k];
    }
    snprintf(what, sizeof(what), "slots after a held thread (%d) and after a free one (%d), two each", held, free_run);
    check(held >= 2 && free_run >= 2, what);
    snprintf(what, sizeof(what), "the decoder's own time stayed put: %.2f s apart at most, under 0.1 s", tmax - tmin);
    check(tmax - tmin < 0.1f, what);
}

int main(void) {
    decoder_skirt();
    decoder_lag(FT8);
    decoder_lag(FT4);
    decoder_thread();
    printf("%d of %d right\n", cases - bad, cases);
    return bad != 0;
}
