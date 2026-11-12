/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver.
 */

#include "receiver.h"

#include <utility>

namespace cw {

/* A mark shorter than this many dots is a blip (noise keying the detector), not a dit */
constexpr float BLIP_K = 0.6f;

Receiver::Receiver(TextFn text, KeyFn key, StepFn step)
    : text_(std::move(text)), key_fn_(std::move(key)), step_fn_(std::move(step)) {
    set_floor_db(5.0f);
}

void Receiver::set_apf(bool on, float tone_hz, float q) {
    finder_.set_apf(on, tone_hz, q);
    step_noise_ = -1.0f;
    keyer_.restart();
}

void Receiver::feed(const float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        /* The noise floor is the gaps': not followed while the key is down, nor
         * while its mark is still in the FFT's 64 ms (ToneFinder::analyse) */
        finder_.hold_noise(keyer_.down() || keyer_.up_steps() * INTEG_STEP < FFT_LEN);
        if (finder_.push(x[i], lock_.hz(), peak_) && finder_.noise() > 0.0f) {
            step_noise_ = finder_.noise() * NOISE_TO_STEP;
            lock_.set_noise(step_noise_);
            if (lock_.follow(peak_)) {
                timer_.new_tone();
                keyer_.new_tone();
                moved_ = true;
            }
        }

        float power = lock_.push(x[i]);
        if (power >= 0.0f && step_noise_ > 0.0f)
            detector_step(power);
    }
}

void Receiver::detector_step(float power) {
    KeyEdge e = keyer_.step(power, step_noise_);

    /*
     * A key down goes on to the timer only once the mark has lasted BLIP_K dots,
     * with its crossing that much further back; one that ends sooner is dropped,
     * and the gaps either side of it are one. Noise peaks in the gaps of a station
     * 10 dB over the noise keyed the detector for 16-32 ms: each came out an E
     * and split a word gap into two, and before the timer's confirmations needed
     * elements of their own (ElementTimer::set_dot) it took them for a new speed.
     */
    if (e.dir > 0 && (text_.owning() || tx_)) {
        pend_      = false;   /* our own keying, or the radio's sidetone, is no blip: on at once */
    } else if (e.dir > 0) {
        pend_      = true;
        pend_ms_   = static_cast<float>(STEP_MS);
        pend_back_ = e.back_ms;
        e.dir      = 0;
    } else if (pend_) {
        if (e.dir < 0) {
            pend_ = false;   /* a blip: never there */
            e.dir = 0;
        } else {
            pend_ms_ += static_cast<float>(STEP_MS);
        }
    }
    if (pend_ && pend_ms_ + pend_back_ >= BLIP_K * timer_.dot_ms()) {
        pend_     = false;
        e.dir     = 1;
        e.back_ms = pend_back_ + pend_ms_ - static_cast<float>(STEP_MS);
    }

    /* Our own keying is not learned: its speed is known (expect), and its dah heard in pieces while the lock
     * finds the tone would only move it */
    if (text_.owning())
        timer_.ours();
    Sym s = timer_.step(e.dir, e.back_ms, e.strong);

    if (s != SYM_NONE) {
        text_.put(s);
        /* After our own last dit or dah the other sender's turn begins */
        if (text_.own_ended())
            timer_.turn();
    }
    if (e.dir != 0 && key_fn_)
        key_fn_(e.dir > 0);

    snr_db_ = 10.0f * std::log10(keyer_.power() / step_noise_ + 1e-12f);
    if (step_fn_)
        step_fn_();
    moved_ = false;
}

} // namespace cw
