/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver.
 */

#include "receiver.h"

#include <algorithm>
#include <utility>

namespace cw {

/* A mark shorter than this many dots is a blip (noise keying the detector), not a dit */
constexpr float BLIP_K = 0.6f;

Receiver::Receiver(TextFn text, KeyFn key, StepFn step)
    : text_(std::move(text)), key_fn_(std::move(key)), step_fn_(std::move(step)) {
    set_floor_db(5.0f);
}

/*
 * 2026-11-25: the X6200 switched to CW 10 s before the X6100's "VVV TEST" read it as "TTTT TTTT TTTT", its decoder at
 * some 50 WPM: it had last measured the noise in other conditions (or seeded it in the switch's quiet first frame), the
 * band's noise came up over that floor and keyed the detector, and a key down holds the floor where it is (the gaps'
 * noise, ToneFinder::analyse): the noise kept itself keying and its short marks set the speed. Replayed from the over
 * alone the same audio copied whole
 */
void Receiver::start_over() {
    finder_.reseed();
    step_noise_ = -1.0f;
    keyer_.restart();
    held_n_ = 0;
}

void Receiver::set_apf(bool on, float tone_hz, float q) {
    finder_.set_apf(on, tone_hz, q);
    step_noise_ = -1.0f;
    keyer_.restart();
    held_n_ = 0;
}

void Receiver::set_base_peak(bool on, float tone_hz, float q) {
    if (finder_.set_base_peak(on, tone_hz, q))     /* the floor in other units */
        start_over();
}

void Receiver::feed(const float *x, size_t n) {
    for (size_t i = 0; i < n; i++) {
        /* The noise floor is the gaps': not followed while the key is down, nor
         * while its mark is still in the FFT's 64 ms (ToneFinder::analyse) */
        finder_.hold_noise(keyer_.down() || keyer_.up_steps() * INTEG_STEP < FFT_LEN);
        if (finder_.push(x[i], lock_.hz(), peak_) && finder_.noise() > 0.0f) {
            /* The floor is the band's with its colour divided out (the APF's, the X6200 base's resonance); the noise
             * at the lock's tone has it: there the X6200's CW noise stands 17-21 dB over the band's floor at the key
             * tone. Judged against the band's floor, that noise keyed the detector in every quiet and counted as a
             * station (2026-11-25, orch cw-quiet: 'ESI I I EI E T E I' with nothing on the air; the X6100, flat, nothing).
             * A colour taken out of the spectrum alone (the APF's till then) lowered the floor under it and not the
             * noise at the lock: the quiets no better, a station at the tone found later */
            step_noise_ = finder_.noise() * NOISE_TO_STEP * finder_.colour_at(lock_.hz());
            lock_.set_noise(step_noise_);
            if (pin_hz_ <= 0.0f && lock_.follow(peak_)) {
                /* The noise at the new tone: with a colour it can be 30 dB from the old one's, and the audio given
                 * again (rewind) is judged against it */
                step_noise_ = finder_.noise() * NOISE_TO_STEP * finder_.colour_at(lock_.hz());
                lock_.set_noise(step_noise_);
                timer_.new_tone();
                keyer_.new_tone();
                held_n_ = 0;   /* the carrier floor is measured again on this tone, from here (KeyDetector) */
                moved_  = true;
                if (peak_.snr >= from_db(PEAK_STATION_DB))
                    rewind();
                else
                    since_move_ = 0;
            }
        }
        lock_step(x[i]);
        past_[past_pos_] = x[i];
        past_pos_        = (past_pos_ + 1) % REWIND_LEN;
        since_move_      = std::min(since_move_ + 1, REWIND_LEN);
    }
}

void Receiver::lock_step(float x, bool rewound) {
    float power = lock_.push(x, rewound);
    if (power >= 0.0f && step_noise_ > 0.0f)
        detector_step(power, step_noise_);
}

/*
 * The lock just moved: the audio it skipped, up to REWIND_LEN, through it again on the new tone, so the new station's
 * first mark is there from its start. The tone finder moves the lock 40-70 ms into a station's first mark (its 64 ms
 * FFT, two frames in a row on the bin): a dit at 20 WPM is 60 ms, and a station's first dit was lost or came out a
 * blip (2026-11-25: the X6100's audio of the X6200's "VV TEST", the lock moved 37 ms into the first V). Only to a
 * station (PEAK_STATION_DB), not from one line of the noise to the next in a quiet. Never the
 * audio of another tone since (the lock moved twice: a station's elements given twice), nor that of a mark the
 * detector was still on (its elements given twice, from the leak of a strong station 50 Hz away that the lock left)
 */
void Receiver::rewind() {
    size_t up   = keyer_.down() || pend_ ? 0 : keyer_.up_steps() * INTEG_STEP;
    size_t n    = std::min({since_move_, up, REWIND_LEN});
    size_t warm = std::min(WARM_LEN, REWIND_LEN - n);
    size_t at   = (past_pos_ + 2 * REWIND_LEN - n - warm) % REWIND_LEN;

    /* The lock's low-pass and integrator, emptied by the move (ToneLock::tune), filled first from the audio before
     * what is given again, nothing keyed by it: else their first steps on the new tone read the noise or less. With
     * the detector on a mark (nothing given again: the lock 39 Hz off a station keyed on its first dit's leak, then
     * moved to it) that first step ended the mark and the dit was dropped as a blip: the X6200's 'VVV' came out 'UVV'
     * once its lock wandered the band in a quiet, its resonance taken out (2026-11-25) */
    since_move_ = 0;
    for (size_t k = 0; k < warm; k++)
        lock_.push(past_[(at + k) % REWIND_LEN], true);
    rewinding_ = true;
    for (size_t k = warm; k < warm + n; k++)
        lock_step(past_[(at + k) % REWIND_LEN], true);
    rewinding_ = false;
}

void Receiver::detector_step(float power, float noise) {
    keyer_.own(text_.owning() || tx_);

    /*
     * The key detector's first second (a cold start, or the APF switched: KeyDetector::muted): it measures the carrier
     * floor and keys nothing. The steps are kept and given to it again, in order, once it has the floor: before, a
     * decoder started as a station began lost all of its first second (2026-11-25: the X6100's of the X6200's "VV
     * TEST" printed "V TEST", the lock found it at 3.49 s and the detector keyed from 4.49 s on). The element timer
     * and the step callback (the squelch's frame counts) get them only then, once each. All against the noise as it is
     * at the end of that second: in it the noise is not held through the marks (no key down to hold it by), and as it
     * settles it can rise a lot: 15 dB in 0.4 s from the X6200's first mark on the X6100 (the X6200's transmitter
     * noise, 2026-11-25), its first dit 56 dB over the noise of before and the elements after it 41, under the
     * thresholds that dit had set: "VVV" came out "E T" */
    if (!replaying_ && keyer_.muted()) {
        if (held_n_ < held_.size())
            held_[held_n_++] = power;
        keyer_.step(power, noise);
        if (keyer_.muted())
            return;
        /* Only a station's: a line of the noise the lock sat on, given again, keyed the detector on its swings (E E
         * E on the X6100's 297 Hz line), where muted it did not, and the quiet before the next element was lost */
        replaying_ = lock_.station();
        for (size_t k = 0; replaying_ && k < held_n_; k++)
            detector_step(held_[k], noise);
        replaying_ = false;
        held_n_    = 0;
        return;
    }
    KeyEdge e = keyer_.step(power, noise);

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
     * finds the tone would only move it. Nor what a rewind gives again, nor anything while the lock holds no station
     * (ToneLock::station: a tone 10 dB over the noise): the noise keying the detector in a quiet is no speed to learn.
     * Learned, its short marks and gaps took the speed up a little at a time (each estimate within NEW_SPEED of the
     * last), and the shorter the dot the more blips passed BLIP_K: the X6200, a minute or two after a text, was at
     * some 50 WPM and printed the X6100's 15 WPM "VVV" as "TTT TTT TTT" (2026-11-25, cw-text) */
    if (text_.owning())
        timer_.ours();
    else if (rewinding_ || (!tx_ && !lock_.station()))
        timer_.unlearned();     /* a station's (or the noise's), not learned; a new text's held elements timed with it */
    Sym s = timer_.step(e.dir, e.back_ms, e.strong);

    if (s != SYM_NONE) {
        text_.put(s);
        /* After our own last dit or dah the other sender's turn begins */
        if (text_.own_ended())
            timer_.turn();
    }
    if (e.dir != 0 && key_fn_)
        key_fn_(e.dir > 0);

    snr_db_ = 10.0f * std::log10(keyer_.power() / noise + 1e-12f);
    if (step_fn_)
        step_fn_();
    moved_ = false;
}

} // namespace cw
