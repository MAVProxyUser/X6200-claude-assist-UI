/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: 4 kHz audio in, text out.
 *
 *  tone finder (FFT) -> tone lock (NCO, PLL) -> key detector -> element
 *  timer -> Morse text
 *
 *  The design (FFT tone search, coherent lock with a PLL, Schmitt detector
 *  following the mark peak, histogram of mark and gap lengths) is the one of
 *  Georgy Dyuldin's receiver in x6100_gui; this is a new implementation of it.
 */

#pragma once

#include <array>
#include <functional>

#include "element_timer.h"
#include "key_detector.h"
#include "morse_text.h"
#include "rx_defs.h"
#include "tone_finder.h"
#include "tone_lock.h"

namespace cw {

class Receiver {
  public:
    using TextFn = MorseText::Out;
    using KeyFn  = std::function<void(bool)>;
    using StepFn = std::function<void()>;

    static constexpr float RATE = RX_RATE;   /* feed() wants this rate */

    /* text: each decoded character, and whether it holds our own keying
     * (own()); key: key down/up; step: after every detector step (8 ms) */
    Receiver(TextFn text, KeyFn key, StepFn step = {});

    void feed(const float *x, size_t n);

    float wpm() const { return timer_.wpm(); }
    float peak_hz() const { return finder_.peak_hz(); }   /* FFT peak of the last frame */
    /* The last frame's peak over the per-bin noise (dB) and its frames in a row on the bin (PeakReport) */
    float peak_snr_db() const { return 10.0f * std::log10(peak_.snr + 1e-12f); }
    int   peak_steady() const { return peak_.steady; }
    float lock_hz() const { return lock_.hz(); }          /* the tone being decoded */
    bool  locked() const { return lock_.holding(); }
    float snr_db() const { return snr_db_; }              /* last step, on the locked tone */

    void  set_floor_db(float db) { keyer_.set_floor_db(db); }
    void  set_band(float lo_hz, float hi_hz) { finder_.set_band(lo_hz, hi_hz); }
    /* The band's level may have changed under the decoder: its audio back after a gap (the radio in another mode,
     * the decoder off), another CW filter. The noise measured again and the detector muted a second, as at start
     * (that second given again if the lock holds a station: detector_step); the speed and the station kept */
    void  start_over();
    /* The radio's peak filter (APF) is on: its colour is taken out of the noise */
    void  set_apf(bool on, float tone_hz, float q);
    /* The VFO moved: the station is df Hz further up in the audio */
    void  follow_vfo(float df) { lock_.shift(df); }
    /* The next n dits and dahs are our own keying (MorseText::own), not
     * learned (ElementTimer::ours) */
    void  own(int n) { text_.own(n); }
    /* The radio is transmitting: the audio is its own sidetone (ToneFinder::transmitting), with no
     * noise to key the detector, so no blips to hold back (detector_step) */
    void  transmitting(bool on) {
        tx_ = on;
        finder_.transmitting(on);
    }
    /* Our own keying follows at wpm (the CW trainer's, ElementTimer::expect) */
    void  expect(float wpm) { timer_.expect(wpm); }
    /* The CW trainer is open: down to 5 WPM (ElementTimer::trainer_range), its
     * long dahs no carrier (KeyDetector::trainer_range) */
    void  trainer_range(bool on) {
        timer_.trainer_range(on);
        keyer_.trainer_range(on);
    }

  private:
    friend struct ReceiverPeek;   /* tests only */

    void lock_step(float x, bool rewound = false);
    void rewind();
    void detector_step(float power, float noise);

    /* The audio the lock is given again when it moves (rewind): the tone finder sees a station's first mark 40-70 ms
     * after it began, and its first dit was lost or a blip */
    static constexpr size_t REWIND_LEN = 768;   /* 192 ms */

    ToneFinder   finder_;
    ToneLock     lock_;
    KeyDetector  keyer_;
    ElementTimer timer_;
    MorseText    text_;
    KeyFn        key_fn_;
    StepFn       step_fn_;

    float step_noise_ = -1.0f;   /* noise of one integrator step, < 0 = not known */
    bool  tx_         = false;   /* the radio is transmitting (transmitting) */
    /* A key down not passed on yet (detector_step): since this long, its crossing this long before its step */
    bool  pend_       = false;
    float pend_ms_    = 0.0f;
    float pend_back_  = 0.0f;
    float snr_db_     = 0.0f;
    bool  moved_      = false;   /* the lock moved since the last step (for the tests) */
    PeakReport peak_{};

    std::array<float, REWIND_LEN> past_{};   /* the last audio, a ring (rewind) */
    size_t past_pos_   = 0;
    size_t since_move_ = 0;      /* samples the lock has had on this tone, up to REWIND_LEN */
    /* The steps' powers the key detector was muted for (KeyDetector::muted): given to it again once it is not */
    std::array<float, KeyDetector::BATCH> held_{};
    size_t held_n_     = 0;
    bool   replaying_  = false;
    bool   rewinding_  = false;   /* in rewind() */
};

} // namespace cw
