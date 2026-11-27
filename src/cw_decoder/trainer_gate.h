/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  OpenX6X00 - The CW trainer's gate on what the decoder prints
 *
 *  The CW trainer hears nothing but the radio's own sidetone: its group's (the radio's keyer) and the user's keying
 *  (train mode, the same keyer and level). A character whose keyed frames came GATE_DB and more under the trainer's
 *  own characters is the band's noise keying the detector in the quiet before an answer: the X6100's quiet, at
 *  -78 dBFS against a sidetone at -28, keyed 'E E EE EEE' over 20 s after a group (the decoder pinned at the key tone)
 *  and '2' 2 s after one (free), and the trainer took that for the answer (2026-11-27, tools/cw_replay TRAINER=1).
 *  Dropped, and the space after it. Its own characters set the level; till the first, everything passes.
 */
#pragma once

namespace cw {

class TrainerGate {
public:
    static constexpr float GATE_DB = 20.0f;

    /* One detector step (8 ms) with the key down, its SNR on the locked tone (Receiver::snr_db) */
    void keyed_step(float snr_db) {
        keyed_++;
        snr_ += snr_db;
    }

    /* A piece of text from the decoder (own: the trainer's own keying, MorseText::own): true when it is let through */
    bool pass(const char *text, bool own) {
        if (text[0] == ' ')
            return !dropped_;
        float snr = keyed_ > 0 ? snr_ / keyed_ : -100.0f;

        keyed_ = 0;
        snr_   = 0.0f;
        if (own) {
            own_snr_ = own_snr_ < 0.0f ? snr : 0.7f * own_snr_ + 0.3f * snr;
            dropped_ = false;
            return true;
        }
        dropped_ = own_snr_ >= 0.0f && snr < own_snr_ - GATE_DB;
        return !dropped_;
    }

    /* The trainer opened or closed: its level is learned again */
    void reset() {
        own_snr_ = -1.0f;
        keyed_   = 0;
        snr_     = 0.0f;
        dropped_ = false;
    }

    float own_snr() const { return own_snr_; }

private:
    float own_snr_ = -1.0f;     /* the trainer's own characters' keyed SNR (dB), -1 none yet */
    int   keyed_   = 0;         /* keyed steps since the last character */
    float snr_     = 0.0f;      /* their SNR, summed */
    bool  dropped_ = false;     /* the last character was dropped: its space too */
};

}  // namespace cw
