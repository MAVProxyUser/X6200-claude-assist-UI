/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receive: RX audio -> cw::Receiver (src/cw_decoder/) -> squelch -> panel,
 *  trainer or remote clients; the locked tone drives the tuning indicator.
 */

// First: the decoder headers bring <complex>, which liquid.h must see before
// it is included anywhere else
#include "cw_decoder/audio_resampler.h"
#include "cw_decoder/trainer_gate.h"

#include "cw.h"
#include "dsp.h"

#include "cfg/settings_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

extern "C" {
    #include "audio.h"
    #include "cw_decoder.h"
    #include "cw_tune_ui.h"
    #include "radio.h"
}

/* Frames (8 ms) between updates of the tune indicator */
#define TUNE_UPDATE_FRAMES  12
/* The decoder's audio stopped this long: the band measured again when it is back (Receiver::start_over) */
#define RESUME_GAP_MS       500
/* Receive this long after our own transmission: the over ended, the band measured again */
#define OVER_END_S          0.4

static std::atomic<bool> started{false};

/* Settings: written by the LVGL thread (subscriptions), read by the audio thread */
static std::atomic<bool> decode_on{false};
static std::atomic<bool> tune_on{false};
static std::atomic<bool> trainer{false};
static std::atomic<int>  trainer_own{-1};      /* cw_trainer_own's count for the audio thread, -1 none */
static std::atomic<int>  trainer_wpm{0};       /* cw_trainer_speed's for the audio thread, 0 none */
static std::atomic<bool> tune_turned{false};   /* the bars' offset turned round: CW on the X6200 */
static std::atomic<int>  key_wpm{0};           /* the KEY Speed, for the audio thread */
static int32_t key_tone = 0;
static int32_t band_lo;
static int32_t band_hi;

/* Receiver settings: stored by the subscriptions (LVGL thread), applied by
 * the audio thread before the next block */
static std::atomic<float> rx_threshold;
static std::atomic<float> rx_hpf;
static std::atomic<float> rx_lpf;
static std::atomic<bool>  rx_peak_on;
static std::atomic<float> rx_peak_tone;
static std::atomic<float> rx_peak_q;
static std::atomic<bool>  rx_dirty{false};

/* Audio thread */
static cw::Receiver       *receiver;
static cw::AudioResampler *resampler;
static bool                keyed = false;
static float               tone_freq;
static std::array<float, 3> freq_hist;
static size_t              freq_hist_w = 0;
static size_t              freq_hist_n = 0;
static uint8_t             frame_counter = 0;
static std::atomic<float>  rx_shift{0.0f};   /* audio Hz the VFO moved, for the audio thread */
/* CW <-> CW-R where the tone changes sides of the key tone (the X6100: CW the upper sideband, CW-R the lower), the
 * dial where it was: the station's tone mirrored about the key tone (676 Hz in CW, 724 in CW-R at a 700 Hz key tone),
 * which the audio thread follows as it follows a dial move. Left to the tracker, it locked on the new place a few
 * frames in four and read 8 Hz for 24 for seconds (orch cw-tuner, 2026-11-27); the X6200's tone rises in both */
static std::atomic<bool>   rx_mirror{false};
static int                 last_dir;    /* radio_cw_tone_dir of the last CW mode, GUI thread */
static int32_t             last_freq;
static int                 wpm_sent = 0;

/* Our own keying's speed (#198): the decoder kept the band's, or what the noise between stations made of it (46 WPM
 * on a quiet band): sending at 15 WPM a dit then read as a dah and VVV as "TTT TTTT". An over's first key-down, after
 * OWN_OVER_GAP_S without the radio in TX, tells it the KEY Speed; not the breaks within an over (QSK: a straight key's
 * own speed, once learned, is kept), nor with the CW trainer open, which tells its own */
#define OWN_OVER_GAP_S  2
static bool                own_tx   = false;
static uint32_t            rx_count = UINT32_MAX;   /* samples since the radio left TX */

#ifdef BOARD_X6200
/* The X6200's base colours CW's receive audio itself, CW and CW-R alike: one resonance at the key tone, its noise
 * 17-21 dB over the band's floor there (orch cw-noise-shape, 2026-11-25, nothing on the air: fitted by Q 9.4-10.9
 * through 300-1100 to 100-1300 Hz; at a 600 Hz key tone it sat at 600, orch cw-noise-tone). Judged against the band's
 * floor, that noise keyed the decoder in every quiet (orch cw-quiet: 'ESI I I EI E T E I'). Taken out as the APF's
 * (Receiver::set_base_peak) */
#define BASE_PEAK_Q 10.0f
#endif

/* Squelch: noise keys the detector now and then and prints lone Es, Ts and Is. A character counts
 * when its keyed frames average SQ_MARGIN_DB over the Dec SNR threshold: on 40 m the noise's
 * characters averaged 6-11 dB, a real station's 14-28 dB (Dec SNR 8). The text opens after
 * SQ_OPEN such characters in a row, closes after SQ_CLOSE that are not, or after SQ_IDLE_FRAMES
 * with nothing keyed. The trainer (own keying) is not squelched.
 * ponytail: margin from one evening on 40 m; tune here if weak real signals stay closed */
#define SQ_MARGIN_DB    5.0f
#define SQ_OPEN         3
#define SQ_CLOSE        2
#define SQ_IDLE_FRAMES  375     /* 3 s of 8 ms frames */
static int         sq_keyed;              /* frames since the last character */
static cw::TrainerGate tgate;             /* the CW trainer's: its noise keys dropped (trainer_gate.h), audio thread */
static float       sq_snr;                /* sum of the keyed frames' SNR (dB) */
static int         sq_idle;
static bool        sq_open = false;
static int         sq_good, sq_bad;
static std::string sq_pending;

static void on_flag(Subject *subj, void *user_data);
static void on_key_speed(Subject *subj, void *user_data);
static void on_rx_param_change(Subject *subj, void *user_data);
static void on_freq_change(Subject *subj, void *user_data);
static void on_mode_change(Subject *subj, void *user_data);

static void on_text(const char *text, bool own);
static void on_keyed(bool on);
static void on_frame();

void cw_init(void) {
    cfg_sm.p_cw_decoder.subscribe_and_notify(on_flag, &decode_on);
    cfg_sm.p_cw_tune.subscribe_and_notify(on_flag, &tune_on);
    cfg_sm.p_key_speed.subscribe_and_notify(on_key_speed);

    cfg_sm.p_key_tone.subscribe(on_rx_param_change);
    cfg_sm.p_cw_decoder_snr.subscribe(on_rx_param_change);
    cfg_sm.p_cw_peak_on.subscribe(on_rx_param_change);
    cfg_sm.p_cw_peak_q.subscribe(on_rx_param_change);
    cfg_sm.cp_cur_filter_low.subscribe(on_rx_param_change);
    cfg_sm.cp_cur_filter_high.subscribe_and_notify(on_rx_param_change);
    last_freq = cfg_sm.cp_fg_freq.get();
    cfg_sm.cp_fg_freq.subscribe(on_freq_change);
    cfg_sm.cp_cur_mode.subscribe_and_notify(on_mode_change);
    tone_freq = key_tone;

    receiver  = new cw::Receiver(on_text, on_keyed, on_frame);
    resampler = new cw::AudioResampler((float)AUDIO_CAPTURE_RATE / AUDIO_DECIM);
    started   = true;
}

/* The tone moves in the audio as the VFO turns: CW on the X6100 is the upper sideband
 * (a higher VFO lowers the tone), CW-R the lower one, and so is CW on the X6200 (#115:
 * there the tone of a dial step of +150 Hz was followed 150 Hz down; it went 150 Hz up).
 * Big jumps (band, memory) are left to the tracker: the station is gone anyway. */
static void on_freq_change(Subject *subj, void *user_data) {
    int32_t freq = cfg_sm.cp_fg_freq.get();
    int32_t d    = freq - last_freq;
    int     dir  = radio_cw_tone_dir((radio_mode_t)cfg_sm.cp_cur_mode.get());

    last_freq = freq;
    if (dir && d != 0 && d >= -1500 && d <= 1500) {
        float s = rx_shift.load();
        while (!rx_shift.compare_exchange_weak(s, s + dir * d)) {}
    }
}

static void apply_rx_params() {
    static bool  peak_on;
    static float peak_tone, peak_q;

    float shift = rx_shift.exchange(0.0f);
    if (rx_mirror.exchange(false)) {
        shift += 2.0f * ((float)key_tone - (tone_freq + shift));
    }
    if (shift != 0.0f) {
        receiver->follow_vfo(shift);
        tone_freq += shift;
        for (auto &f : freq_hist) f += shift;
    }

    if (!rx_dirty.exchange(false)) {
        return;
    }
    receiver->set_floor_db(rx_threshold);
    receiver->set_band(rx_hpf, rx_lpf);

    /* Another CW filter: another band's level under the decoder (Receiver::start_over) */
    static float band_lo_set = -1.0f, band_hi_set = -1.0f;
    if (rx_hpf != band_lo_set || rx_lpf != band_hi_set) {
        if (band_lo_set >= 0.0f)
            receiver->start_over();
        band_lo_set = rx_hpf;
        band_hi_set = rx_lpf;
    }

    /* Changing the peak filter re-seeds the noise floor: only on a change */
    if (peak_on != rx_peak_on || peak_tone != rx_peak_tone || peak_q != rx_peak_q) {
        peak_on = rx_peak_on;
        peak_tone = rx_peak_tone;
        peak_q = rx_peak_q;
        /* Only a base that applies its peaking filter (HW_CAP_CW_PEAK: the patched X6100 base) colours the audio: the
         * X6200's has no such filter, and Xiegu's own X6100 base takes the setting and applies none (V1.1.9: no peak
         * at any Q, orch cw-peak-q 2026-11-25). Told of one, the decoder took it out of audio that did not have it
         * and judged the noise off the key tone up to 12 dB too low: with CW peak on most of the X6200's 29 bench
         * recordings of the X6100's CW copied worse than with it off ('VS', 'PARI'), at Q 8 their gaps printed
         * noise (2026-11-25) */
        receiver->set_apf(peak_on && (radio_caps() & HW_CAP_CW_PEAK), peak_tone, peak_q);
    }
#ifdef BOARD_X6200
    receiver->set_base_peak(true, rx_peak_tone, BASE_PEAK_Q);   /* a change only: the band measured again */
#endif
}

bool cw_audio_wanted(void) {
    return started && (decode_on || tune_on || trainer);
}

void cw_put_audio_samples(unsigned int n, float *samples) {
    static bool trainer_range = false;   /* the receiver has the trainer's speed range */
    bool        tr     = trainer;
    bool        wanted = decode_on || tune_on || tr;

    if (!started || !wanted)
        return;
    /* The decoder's audio back after a gap (the radio in another mode, the decoder or the tuning bars off): what it
     * measured of the band is from before (Receiver::start_over). Blocks come every 100 ms or so */
    static std::chrono::steady_clock::time_point last;
    auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0 && now - last > std::chrono::milliseconds(RESUME_GAP_MS))
        receiver->start_over();
    last = now;
    apply_rx_params();
    /* The trainer's speed range (down to 5 WPM) while it is open, the band's (10 WPM) again from the first block
     * after it closed; set before the trainer's speed, which the range clamps */
    if (tr != trainer_range) {
        receiver->trainer_range(tr);
        trainer_range = tr;
        tgate.reset();
#ifdef BOARD_X6200
        tgate.set_margin(0.0f);         /* told, not dropped: its decoder audio raised by state blocks (trainer_gate.h) */
#endif
    }
    /* The trainer's: its own sidetone at the key tone, nothing else (Receiver::pin) */
    static float pinned = 0.0f;
    float        pin_at = tr ? (float)key_tone : 0.0f;
    if (pin_at != pinned) {
        receiver->pin(pin_at);
        pinned = pin_at;
    }
    int wpm = trainer_wpm.exchange(0);
    if (wpm > 0)
        receiver->expect(wpm);
    int own = trainer_own.exchange(-1);
    if (own >= 0)
        receiver->own(own);

    bool tx = radio_get_state() == RADIO_TX;
    if (tx && !own_tx && !tr && rx_count >= OWN_OVER_GAP_S * (AUDIO_CAPTURE_RATE / AUDIO_DECIM))
        receiver->expect(key_wpm);
    /* Our own over ended (0.4 s of receive after it, longer than a QSK break between characters): the band measured
     * again (Receiver::start_over). The floor seeded from the first frame after the key (ToneFinder::transmitting)
     * could be the base's receive audio still coming back: too low, the band's noise then keyed the detector, which
     * held it there, and the X6200 went into the X6100's next text at some 50 WPM (cw-kbd, cw-carrier after its own
     * overs, 2026-11-25: 'EKTT TTT TTT TTT XV Q1TST' for VVV VVV Q1TST) */
    static bool over_end = false;
    if (tx)
        over_end = !tr;     /* not the CW Trainer's groups and answers: its timing is its own */
    else if (over_end && rx_count >= OVER_END_S * (AUDIO_CAPTURE_RATE / AUDIO_DECIM)) {
        over_end = false;
        receiver->start_over();
    }
    own_tx   = tx;
    rx_count = tx ? 0 : rx_count > UINT32_MAX - n ? UINT32_MAX : rx_count + n;
    receiver->transmitting(tx);

    resampler->process(n, samples, *receiver);
}

/* Decoded text: to the panel or the trainer, with the WPM when it changes */
static bool squelch(const char *text) {
    bool space = text[0] == ' ';
    bool good  = sq_keyed > 0 && sq_snr / sq_keyed >= rx_threshold.load() + SQ_MARGIN_DB;

    if (!space) {
        sq_keyed = 0;
        sq_snr   = 0.0f;
    }
    /* Open: a weak character waits in sq_pending; printed only if a good one follows (a weak letter
     * of the station), dropped with the squelch closing if another weak one comes (the gaps' noise) */
    if (sq_open) {
        if (space) {
            if (!sq_bad) {
                return true;
            }
            sq_pending += text;
            return false;
        }
        if (good) {
            sq_bad = 0;
            if (sq_pending.empty()) {
                return true;
            }
            sq_pending += text;
            cw_decoder_text(sq_pending.c_str(), false);
            sq_pending.clear();
            return false;
        }
        if (++sq_bad >= SQ_CLOSE) {
            sq_open = false;
            sq_good = 0;
            sq_pending.clear();
            return false;
        }
        sq_pending += text;
        return false;
    }
    if (space) {
        if (!sq_pending.empty()) {
            sq_pending += text;
        }
        return false;
    }
    if (!good) {
        sq_pending.clear();
        sq_good = 0;
        return false;
    }
    sq_pending += text;
    if (++sq_good >= SQ_OPEN) {
        sq_open = true;
        sq_bad  = 0;
        cw_decoder_text(sq_pending.c_str(), false);
        sq_pending.clear();
    }
    return false;
}

static void on_text(const char *text, bool own) {
    if (!decode_on && !trainer)
        return;
    if (!trainer && !squelch(text))
        return;
    if (trainer) {
        bool ok = tgate.pass(text, own);

        if (tgate.would_drop())
            fprintf(stderr, "cw trainer: '%s' at %.1f dB, the trainer's own at %.1f dB: %s\n", text, tgate.last_snr(),
                    tgate.own_snr(), ok ? "kept (told only)" : "dropped");
        if (!ok)
            return;
    }
    cw_decoder_text(text, own);

    int wpm = lroundf(receiver->wpm());
    if (wpm != wpm_sent) {
        wpm_sent = wpm;
        cw_decoder_wpm(wpm);
    }
}

static void on_keyed(bool on) {
    keyed = on;
}

/* Every 8 ms: follow the tone while keyed and locked (median of 3, then
 * smoothed). The tone the detector is locked on: the FFT peak of a frame jumps
 * to any other station or noise peak in the passband; and only a tone held
 * coherently: noise that keys the detector (the Es of an empty band) does not
 * light the indicator. */
static void on_frame() {
    bool tuned = keyed && receiver->locked();

    if (keyed) {
        sq_keyed++;
        sq_snr += receiver->snr_db();
        tgate.keyed_step(receiver->snr_db());
        sq_idle = 0;
    } else if (++sq_idle >= SQ_IDLE_FRAMES && (sq_open || !sq_pending.empty())) {
        sq_open = false;
        sq_good = 0;
        sq_pending.clear();
    }

    if (tuned) {
        float freq = receiver->lock_hz();
        if ((freq >= band_lo) && (freq <= band_hi)) {
            freq_hist[freq_hist_w] = freq;
            freq_hist_w = (freq_hist_w + 1) % freq_hist.size();
            if (freq_hist_n < freq_hist.size()) {
                freq_hist_n++;
            }
            if (freq_hist_n == freq_hist.size()) {
                std::array<float, 3> s = freq_hist;
                std::sort(s.begin(), s.end());
                tone_freq += 0.5f * (s[1] - tone_freq);
            }
        }
    } else {
        freq_hist_n = 0;
    }

    if (++frame_counter >= TUNE_UPDATE_FRAMES) {
        frame_counter = 0;
        if (tuned) {
            cw_tune_set_freq(tune_turned ? tone_freq - key_tone : key_tone - tone_freq);
        }
    }
}

void cw_trainer_set(bool on) {
    trainer = on;
    if (!on) {
        trainer_own = 0;
        trainer_wpm = 0;    /* not yet handed over: the trainer's speed, not the band's */
    }
}

void cw_trainer_own(int elements) {
    trainer_own = elements > 0 ? elements : 0;
}

void cw_trainer_speed(int wpm) {
    trainer_wpm = wpm;
}

bool cw_trainer_on(void) {
    return trainer;
}

float cw_get_tone_freq(void) {
    return tone_freq;
}

int cw_get_wpm(void) {
    return wpm_sent;
}

static void on_flag(Subject *subj, void *user_data) {
    auto *flag = static_cast<std::atomic<bool> *>(user_data);

    *flag = static_cast<SubjectT<int32_t> *>(subj)->get() != 0;
}

static void on_key_speed(Subject *subj, void *user_data) {
    key_wpm = static_cast<SubjectT<int32_t> *>(subj)->get();
}

/* The tune bars as the X6100 lights them: in CW on the station's side of the dial (the key tone minus the tone,
 * CW's tone falling as the dial goes up), in CW-R the other way round. The X6200's CW tone rises as the dial
 * goes up, as CW-R's does: there the offset is turned round in CW, the bars the same way up on both radios (#115) */
static void on_mode_change(Subject *subj, void *user_data) {
    radio_mode_t mode = (radio_mode_t)cfg_sm.cp_cur_mode.get();
    int          dir  = radio_cw_tone_dir(mode);

    tune_turned = mode == radio_mode_cw && dir > 0;
    if (dir) {
        if (last_dir && dir != last_dir) {
            rx_mirror = true;           /* the tone to the key tone's other side (rx_mirror) */
        }
        last_dir = dir;
    }
}

static void on_rx_param_change(Subject *subj, void *user_data) {
    key_tone = cfg_sm.p_key_tone.get();
    band_lo  = cfg_sm.cp_cur_filter_low.get();
    band_hi  = cfg_sm.cp_cur_filter_high.get();

    rx_threshold = cfg_sm.p_cw_decoder_snr.get();
    rx_hpf = band_lo;
    rx_lpf = band_hi;
    rx_peak_on = cfg_sm.p_cw_peak_on.get();
    rx_peak_tone = key_tone;
    rx_peak_q = cfg_sm.p_cw_peak_q.get();
    rx_dirty = true;
}
