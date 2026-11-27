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

    /* How far under the trainer's own a character may be, dB; 0: none dropped, only told (would_drop). The X6200's
     * decoder audio is raised only in the blocks its state reads TX (dsp.cpp, cw_own_tone.h), so its group and an
     * answer keyed another way can come to it 20 dB apart: there the gate tells, measured before it drops anything
     * (2026-11-27: cwtrainer-learn heard 'M' for an answer 'MMM' with it at 20) */
    void set_margin(float db) { margin_ = db; }

    /* A piece of text from the decoder (own: the trainer's own keying, MorseText::own): true when it is let through */
    bool pass(const char *text, bool own) {
        if (text[0] == ' ')
            return !dropped_;
        float snr = keyed_ > 0 ? snr_ / keyed_ : -100.0f;

        keyed_     = 0;
        snr_       = 0.0f;
        last_snr_  = snr;
        would_     = false;
        if (own) {
            own_snr_ = own_snr_ < 0.0f ? snr : 0.7f * own_snr_ + 0.3f * snr;
            dropped_ = false;
            return true;
        }
        would_   = own_snr_ >= 0.0f && snr < own_snr_ - GATE_DB;
        dropped_ = would_ && margin_ > 0.0f && snr < own_snr_ - margin_;
        return !dropped_;
    }

    /* The last character, judged: its keyed SNR, and whether it was under the trainer's own by GATE_DB */
    float last_snr() const { return last_snr_; }
    bool  would_drop() const { return would_; }

    /* The trainer opened or closed: its level is learned again */
    void reset() {
        own_snr_ = -1.0f;
        keyed_   = 0;
        snr_     = 0.0f;
        dropped_ = false;
        would_   = false;
    }

    float own_snr() const { return own_snr_; }

private:
    float own_snr_ = -1.0f;     /* the trainer's own characters' keyed SNR (dB), -1 none yet */
    int   keyed_   = 0;         /* keyed steps since the last character */
    float snr_     = 0.0f;      /* their SNR, summed */
    bool  dropped_ = false;     /* the last character was dropped: its space too */
    bool  would_   = false;     /* the last character was GATE_DB under the trainer's own */
    float last_snr_ = 0.0f;     /* the last character's keyed SNR */
    float margin_  = GATE_DB;   /* set_margin */
};

}  // namespace cw
