/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 audio / decode worker
 *
 *  Copyright (c) 2026
 */

#include "audio_worker.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <liquid/liquid.h>
#include "util.h"
#include <ft8lib/constants.h>

#include "worker.h"
#include "ft2.h"

/* Waterfall display width in pixels; used to size the FFT so each pixel maps
 * to one bin over the visible band. Matches dialog_ft8.c. */
#define WORKER_DISPLAY_WIDTH   771

struct audio_worker_s {
    /* Configuration (const after create). */
    int            sample_rate;       /* audio_sample_rate */
    ftx_protocol_t proto;
    int32_t        filter_low_hz;
    int32_t        filter_high_hz;
    uint16_t       nfft;
    uint16_t       fps_ms;            /* min interval between PSD emissions */

    audio_worker_cb_t cb;

    /* Thread lifecycle. joinable is the caller's (start/stop/destroy), not the thread's: set once
     * pthread_create() succeeds, the thread joined while it is, run yet or not (audit C04) */
    pthread_t       thread;
    bool            joinable;
    atomic_bool     stop_req;
    pthread_mutex_t sleep_mux;
    pthread_cond_t  sleep_cv;

    /* Audio buffer (written by audio_worker_feed, read by worker). */
    pthread_mutex_t audio_mux;
    cbufferf        audio_buf;
    /* When the newest sample in it came in, by CLOCK_REALTIME and CLOCK_MONOTONIC (the same moment): the oldest one
     * came buffered / sample_rate before it. Under audio_mux. The slots take their blocks by it, and Auto Time Sync
     * (time_sync.h) the moment the decoder's audio began. A feed that does not fit loses samples (cbufferf_write
     * drops it all): stamp_lost, the time of the slot's audio not known, until the next slot */
    int64_t         newest_ns, newest_mono_ns;
    bool            stamp_lost;
    long long       slot_index;       /* the slot the waterfall is filling: seconds since the epoch / slot time.
                                       * Worker thread only */

    /* DSP state (worker thread only). */
    spgramf         sg;
    float          *psd;
    float complex  *audio_frame_buf;
    size_t          audio_frame_buf_len;
    int             block_size;

    uint64_t        last_psd_time_ms;

    /* The decoder's audio (worker thread): when its first sample came, 0 not known; the blocks put into it since;
     * wf_first: the next block read is its first */
    int64_t         wf_ns, wf_mono_ns;
    uint32_t        wf_blocks;
    bool            wf_first;
};

/* ---------- small helpers ---------------------------------------------- */

static bool get_time_slot(ftx_protocol_t proto, struct timespec now, float *sec_since_start) {
    float sec = (now.tv_sec % 60) + now.tv_nsec / 1.0e9f;
    float slot_time = ftx_slot_time(proto);
    *sec_since_start = fmodf(sec, slot_time);
    return ((int)(sec / slot_time) % 2) != 0;
}

static void msleep_interruptible(audio_worker_t *w, long ms) {
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec  += ms / 1000;
    abstime.tv_nsec += (ms % 1000) * 1000000L;
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec  += 1;
        abstime.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&w->sleep_mux);
    if (!atomic_load(&w->stop_req)) {
        pthread_cond_timedwait(&w->sleep_cv, &w->sleep_mux, &abstime);
    }
    pthread_mutex_unlock(&w->sleep_mux);
}

static int64_t clock_ns(clockid_t id) {
    struct timespec t;

    clock_gettime(id, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static double now_s(void) {
    return clock_ns(CLOCK_REALTIME) / 1e9;
}

/* The slot a moment falls in, counted from the epoch: the slots of all three modes divide the minute */
static long long slot_of(const audio_worker_t *w, double t) {
    return (long long)floor(t / ftx_slot_time(w->proto));
}

/* A new slot: the decoder's audio starts again with its first block, whose time is taken as it is read */
static void slot_audio_new(audio_worker_t *w) {
    pthread_mutex_lock(&w->audio_mux);
    w->stamp_lost = false;
    pthread_mutex_unlock(&w->audio_mux);
    w->wf_ns = w->wf_mono_ns = 0;
    w->wf_blocks = 0;
    w->wf_first  = true;
}

/* ---------- PSD / waterfall ------------------------------------------- */

static void maybe_emit_psd(audio_worker_t *w, const slot_info_t *info,
                           float sec_since_slot_start) {
    uint64_t now = get_time();
    if ((now - w->last_psd_time_ms) < w->fps_ms) return;
    w->last_psd_time_ms = now;

    spgramf_get_psd(w->sg, w->psd);
    liquid_vectorf_addscalar(w->psd, w->nfft,
                             -10.f * log10f(sqrtf(w->nfft)),
                             w->psd);

    if (w->cb.on_psd) {
        w->cb.on_psd(w->psd, w->nfft, sec_since_slot_start, info, w->cb.ctx);
    }

    spgramf_reset(w->sg);
}

/* ---------- ftx_worker decode callback trampoline --------------------- */

typedef struct {
    audio_worker_t    *w;
    const slot_info_t *info;
} decode_ctx_t;

static void decode_cb(const char *text, int snr, float freq_hz, float time_sec, void *user) {
    decode_ctx_t *dc = (decode_ctx_t *)user;
    if (dc && dc->w && dc->w->cb.on_message) {
        slot_info_t info = *dc->info;

        info.rx_start_ns      = dc->w->wf_ns;
        info.rx_start_mono_ns = dc->w->wf_mono_ns;
        dc->w->cb.on_message(text, snr, freq_hz, time_sec, &info, dc->w->cb.ctx);
    }
}

/* ---------- RX frame pump --------------------------------------------- */

static void rx_consume_frames(audio_worker_t *w, const slot_info_t *info,
                              float sec_since_slot_start) {
    decode_ctx_t dc = {.w = w, .info = info};

    for (;;) {
        if (atomic_load(&w->stop_req)) return;

        unsigned int   got       = 0;
        float         *block       = NULL;
        float          block_data[w->block_size];

        bool           stale     = false;

        /*
         * Each block goes to the slot it came in during, by the time it came
         * in, not by when this loop gets to it: a decode in this thread can
         * take the better part of a second on the radio (an FT2 slot's 200
         * candidates), and the slot change was seen only that late. The old
         * slot's waterfall took the new one's start meanwhile and the new
         * one began up to a second late. FT2's candidate search reaches only
         * 4 symbols (0.1 s) before the waterfall's start, FT4's 0.2 s: their
         * signals start at the slot, so those decodes were lost.
         */
        pthread_mutex_lock(&w->audio_mux);
        unsigned int buffered = cbufferf_size(w->audio_buf);
        if (buffered >= (unsigned int)w->block_size) {
            int64_t   back = (int64_t)buffered * 1000000000LL / w->sample_rate;
            double    t0   = (w->newest_ns - back) / 1e9;
            long long s    = slot_of(w, t0 + 0.5 * w->block_size / w->sample_rate);

            if (s <= w->slot_index) {     /* a later slot's block waits for the slot change */
                stale = s < w->slot_index;   /* a slot already decoded: dropped */
                cbufferf_read(w->audio_buf, w->block_size, &block, &got);
                // Copy data to external buffer
                memcpy((void*)block_data, (void*)block, got * sizeof(float));
                block = block_data;
                cbufferf_release(w->audio_buf, got);
                /* The slot's first block: the decoder's audio begins with its first sample, which came in when the
                 * buffer's oldest did (Auto Time Sync) */
                if (!stale && w->wf_first) {
                    w->wf_first   = false;
                    w->wf_ns      = w->newest_ns ? w->newest_ns - back : 0;
                    w->wf_mono_ns = w->newest_ns ? w->newest_mono_ns - back : 0;
                }
                if (w->stamp_lost) w->wf_ns = w->wf_mono_ns = 0;
            }
        }
        pthread_mutex_unlock(&w->audio_mux);

        if (!block) {
            break;
        }
        if (stale) {
            continue;
        }

        spgramf_write(w->sg, block, w->block_size);
        maybe_emit_psd(w, info, sec_since_slot_start);

        ftx_worker_put_rx_samples(block, w->block_size);
        w->wf_blocks++;

        if (ftx_worker_is_full()) {
            ftx_worker_decode(decode_cb, true, (void *)&dc);
            ftx_worker_reset();
            /* The decoder's audio goes on from the next block, this many samples on */
            if (w->wf_ns) {
                int64_t on = (int64_t)w->wf_blocks * w->block_size * 1000000000LL / w->sample_rate;

                w->wf_ns      += on;
                w->wf_mono_ns += on;
            }
            w->wf_blocks = 0;
        } else {
            ftx_worker_decode(decode_cb, false, (void *)&dc);
        }
    }
}

/* ---------- thread body ---------------------------------------------- */

static void *worker_main(void *arg) {
    audio_worker_t *w = (audio_worker_t *)arg;
    slot_info_t     info = { .odd = false, .answer_generated = false };
    decode_ctx_t    dc   = { .w = w, .info = &info };

    w->slot_index = slot_of(w, now_s());

    while (!atomic_load(&w->stop_req)) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);

        float sec_since_slot_start = 0.0f;
        bool  new_odd  = get_time_slot(w->proto, now, &sec_since_slot_start);
        bool  new_slot = (new_odd != info.odd);
        bool  tick_done = false;

        /* FT2: a transmission must start within 0.8 s of its slot and lasts
         * 2.52 s, but the last decode of the slot just ended takes up to
         * 0.5 s on the radio. So on_tick (which starts a pending TX) runs
         * first, the last decode after it (SP2KMO's upstream 76d98ce2). */
        if (new_slot && (int)w->proto == (int)FTX_PROTOCOL_FT2 && w->cb.on_tick) {
            slot_info_t next = info;

            next.odd = new_odd;
            w->cb.on_tick(&next, true, sec_since_slot_start, w->cb.ctx);
            tick_done = true;
        }

        rx_consume_frames(w, &info, sec_since_slot_start);

        if (new_slot) {
            /* Slot boundary: flush a final decode and fire the slot_end
             * handler. The audio from before the new slot still to come
             * (stale audio accumulated during TX too) is dropped as it is
             * read; the new slot's own start stays for it. */
            ftx_worker_decode(decode_cb, true, (void *)&dc);
            ftx_worker_reset();
            w->slot_index = slot_of(w, now_s());
            slot_audio_new(w);          /* Auto Time Sync: the new slot's audio timed from its first block */

            if (w->cb.on_slot_end) {
                w->cb.on_slot_end(&info, w->cb.ctx);
            }
            info.odd = new_odd;
        }

        if (w->cb.on_tick && !tick_done) {
            w->cb.on_tick(&info, new_slot, sec_since_slot_start, w->cb.ctx);
        }

        if (!new_slot && !atomic_load(&w->stop_req)) {
            msleep_interruptible(w, 100);
        }
    }
    return NULL;
}

/* ---------- public API ------------------------------------------------- */

audio_worker_t *audio_worker_create(int audio_sample_rate,
                                    ftx_protocol_t proto,
                                    int32_t filter_low_hz,
                                    int32_t filter_high_hz,
                                    const audio_worker_cb_t *cb) {
    if (audio_sample_rate <= 0) return NULL;
    int span = filter_high_hz - filter_low_hz;
    if (span <= 0) return NULL;

    audio_worker_t *w = (audio_worker_t *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->sample_rate    = audio_sample_rate;
    w->proto          = proto;
    w->filter_low_hz  = filter_low_hz;
    w->filter_high_hz = filter_high_hz;
    w->nfft           = (uint16_t)(WORKER_DISPLAY_WIDTH * w->sample_rate / span);
    w->fps_ms         = (uint16_t)(1000 / 2.5);   /* 2.5 Hz: the 80 px above the log show two slots */
    if (cb) w->cb = *cb;

    pthread_mutex_init(&w->audio_mux, NULL);
    pthread_mutex_init(&w->sleep_mux, NULL);
    pthread_cond_init(&w->sleep_cv, NULL);

    w->audio_buf = cbufferf_create(audio_sample_rate * 3);
    if (!w->audio_buf) goto fail;

    w->sg = spgramf_create(w->nfft, LIQUID_WINDOW_HANN, w->nfft, w->nfft / 2);
    if (!w->sg) goto fail;

    w->psd = (float *)malloc(w->nfft * sizeof(float));
    if (!w->psd) goto fail;

    ftx_worker_init(w->sample_rate, proto);
    w->block_size          = ftx_worker_get_block_size();
    if (w->block_size <= 0) goto fail;

    atomic_store(&w->stop_req, false);
    w->wf_first   = true;   /* the first block read begins the decoder's audio */

    return w;

fail:
    audio_worker_destroy(w);
    return NULL;
}

int audio_worker_start(audio_worker_t *w) {
    if (!w || w->joinable) return -1;   /* a second thread would take the first one's handle */
    atomic_store(&w->stop_req, false);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
        return -1;
    }
    w->joinable = true;
    return 0;
}

void audio_worker_stop(audio_worker_t *w) {
    if (!w) return;
    atomic_store(&w->stop_req, true);
    pthread_mutex_lock(&w->sleep_mux);
    pthread_cond_broadcast(&w->sleep_cv);
    pthread_mutex_unlock(&w->sleep_mux);

    /* Not "has it run yet": a thread not scheduled so far starts on a worker freed meanwhile */
    if (w->joinable) {
        pthread_join(w->thread, NULL);
        w->joinable = false;
    }
}

void audio_worker_destroy(audio_worker_t *w) {
    if (!w) return;
    audio_worker_stop(w);

    if (w->audio_frame_buf) {
        free(w->audio_frame_buf);
        w->audio_frame_buf = NULL;
    }
    if (w->psd) {
        free(w->psd);
        w->psd = NULL;
    }

    if (w->sg) {
        spgramf_destroy(w->sg);
        w->sg = NULL;
    }
    if (w->audio_buf) {
        cbufferf_destroy(w->audio_buf);
        w->audio_buf = NULL;
    }

    ftx_worker_free();

    pthread_mutex_destroy(&w->audio_mux);
    pthread_mutex_destroy(&w->sleep_mux);
    pthread_cond_destroy(&w->sleep_cv);

    free(w);
}

void audio_worker_feed(audio_worker_t *w, unsigned int n, float *samples) {
    if (!w || !samples || (n == 0)) return;

    /* The fragment's last sample came now, the rest before it at the sample rate */
    int64_t now = clock_ns(CLOCK_REALTIME), mono = clock_ns(CLOCK_MONOTONIC);

    pthread_mutex_lock(&w->audio_mux);
    if (w->audio_buf) {
        if (cbufferf_space_available(w->audio_buf) < n) {
            w->stamp_lost = true;       /* cbufferf_write drops it all: the newest sample is still the one before */
        } else {
            cbufferf_write(w->audio_buf, samples, n);
            w->newest_ns      = now;
            w->newest_mono_ns = mono;
        }
    }
    pthread_mutex_unlock(&w->audio_mux);
}

uint16_t audio_worker_nfft(const audio_worker_t *w) {
    return w ? w->nfft : 0;
}
