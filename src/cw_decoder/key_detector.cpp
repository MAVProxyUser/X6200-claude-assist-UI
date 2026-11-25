/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: key detector.
 */

#include "key_detector.h"

#include <algorithm>

namespace cw {

namespace {

constexpr size_t EDGE_STEPS  = 2;    /* steps over a threshold for an edge */
/* After 400 ms of key up (about a word gap) the first mark needs one step more:
 * clicks and noise peaks in a quiet band last two steps */
constexpr size_t QUIET_STEPS = 50;

/* Carrier floor: noise alone has its 5th percentile of power/noise at
 * -ln(0.95) = 0.0513. Twice that and the noise reference is raised with it. */
constexpr float  PCT         = 0.05f;
constexpr float  PCT_NOISE   = 0.05129f;
constexpr float  CARRIER_K   = 2.0f;
constexpr float  PCT_STEP    = 0.02f;
constexpr int    CARRIER_STEPS = 60;    /* 480 ms down, longer than a 10 WPM dash */
/* With the CW trainer open (down to 5 WPM): 1.92 s, 8 dots at 5 WPM, where the element timer takes a mark for a
 * carrier too. At 480 ms each dah of 5-9 WPM raised the floor (a 5 WPM dah is 720 ms, 1080 at KEY Ratio 4.5), the
 * dahs of a 0 more and more, until its last dahs and the dits after them were lost */
constexpr int    TRAINER_CARRIER_STEPS = 240;
constexpr float  CARRIER_UP  = 1.05f;   /* per step, once it is a carrier */

/* A mark alone must peak this much over the decode floor (see ElementTimer) */
constexpr float  LONE_MARK_DB = 3.0f;

/* A key down is a rise: this much over the lowest of the steps before it (rose()). A station's element comes up in a
 * few ms; what a strong station sends between its elements, lifted by the receiver's AGC climbing back through its
 * word gap (some 40 dB in 0.6 s: half a dB a step), creeps over the thresholds, which fell 20 dB with the last mark's
 * peak: the decoder keyed on it ~200 ms before the next element and the word spaces went ("VVV TEST DE Q1TST" from
 * the X6100, whose carrier between elements is 62 dB under its marks, printed "VVVTESTDEQ1TST" by the X6200,
 * 2026-11-24; tools/cw_replay's x6100_cw_on_x6200 now copies whole). 4 dB, not 6: at 6 a weak element in a 20 dB
 * fade, rising out of an element gap its integrator had not let all the way down, was lost (a synthetic QSO with
 * QSB: 13 % -> 18 % of its characters wrong); at 4 the same as before the rule there and in noise to 2 dB SNR */
constexpr float  RISE_DB = 4.0f;

} // namespace

KeyEdge KeyDetector::step(float power, float noise) {
    float r     = power / noise;
    bool  muted = muted_;

    if (batch_n_ < BATCH) {
        /* First second on a tone: the floor from a batch, tracked from there. Its powers against the noise as it is at
         * its end, as Receiver::detector_step gives a muted second's steps again: measured against each step's own
         * noise, the floor was not that of the steps given, and their tracking took it down 1-2 % a step until a line
         * of the X6100's noise (297 Hz) keyed E after E (2026-11-25) */
        batch_[batch_n_++] = power;
        if (batch_n_ == BATCH) {
            auto q = batch_.begin() + static_cast<long>(PCT * BATCH);
            std::nth_element(batch_.begin(), q, batch_.end());
            carrier_ = std::max({carrier_, *q / noise, 1e-3f * PCT_NOISE});
            muted_   = false;
        }
    }
    if (muted) {
        power = 0.0f;   /* the noise floors are still settling (and the step that ends the batch: muted()) */
    } else if (down_steps_ > (trainer_ ? TRAINER_CARRIER_STEPS : CARRIER_STEPS)) {
        carrier_ *= CARRIER_UP;
    } else {
        /* Quantile tracking: up a little when over, down when under */
        carrier_ *= r > carrier_ ? (1.0f + PCT_STEP * PCT) : (1.0f - PCT_STEP * (1.0f - PCT));
        carrier_ = std::max(carrier_, 1e-3f * PCT_NOISE);
    }
    last_power_ = power;

    float n  = noise * std::max(1.0f, carrier_ / (CARRIER_K * PCT_NOISE));
    float rn = power / n;

    KeyEdge e = schmitt(rn);

    if (e.dir > 0)
        mark_peak_ = rn;
    else if (down_ || e.dir < 0)
        mark_peak_ = std::max(mark_peak_, rn);
    e.strong = mark_peak_ >= from_db(floor_db_ + LONE_MARK_DB);

    down_steps_ = down_ ? down_steps_ + 1 : 0;
    return e;
}

KeyEdge KeyDetector::schmitt(float r) {
    float back    = 0.0f;   /* samples */
    int   dir     = 0;
    bool  leaving = false;

    if (!down_) {
        size_t need = up_steps_ >= QUIET_STEPS ? EDGE_STEPS + 1 : EDGE_STEPS;

        up_steps_++;
        if (r > on_lvl_) {
            leaving = true;
            if (run_ == 0)
                r_new_ = r;
            if (++run_ >= need && !own_ && !rose(r)) {
                /* a level creeping up, not a mark keyed: the edge is looked for again from here */
                run_   = 0;
                r_old_ = r;
            } else if (run_ >= need) {
                /* the extra step of a first mark is given back */
                back      = -crossing() + static_cast<float>((need - EDGE_STEPS) * INTEG_STEP);
                up_steps_ = 0;
                down_     = true;
                run_      = 0;
                dir       = 1;
            }
        } else {
            run_ = 0;
        }
    } else {
        if (r < off_lvl_) {
            leaving = true;
            if (run_ == 0)
                r_new_ = r;
            if (++run_ >= EDGE_STEPS) {
                back  = -crossing();
                down_ = false;
                run_  = 0;
                dir   = -1;
            }
        } else {
            run_ = 0;
        }
    }
    if (!leaving)
        r_old_ = r;

    follow_peak(r);
    std::copy_backward(past_.begin(), past_.end() - 1, past_.end());
    past_[0] = r;
    return {dir, 1000.0f * back / RX_RATE, false};
}

/* r a rise: RISE_DB over the lowest of the steps before the one before it (past_[1..]: the edge's own first step is
 * past_[0] or past_[1], the level it rose from further back). Before any steps, a rise */
bool KeyDetector::rose(float r) const {
    float low = *std::min_element(past_.begin() + 1, past_.end());

    return r >= low * from_db(RISE_DB);
}

/* Thresholds from the mark's peak, which decays slowly (faster with key up) */
void KeyDetector::follow_peak(float r) {
    if (down_ && r > peak_)
        peak_ = r;
    peak_ *= down_ ? 0.97f : 0.94f;
    if (peak_ < 1.0f)
        peak_ = 1.0f;

    on_lvl_  = std::max(peak_ * from_db(-3.0f), from_db(floor_db_));
    off_lvl_ = std::max(peak_ * from_db(-6.0f), from_db(floor_db_ - 3.0f));
    mid_lvl_ = 1.0f + (peak_ - 1.0f) * from_db(-6.0f);
}

/* Where between the last old-side step and the first new-side step the
 * envelope crossed mid_lvl_, in samples back from the newer one (<= 0). The
 * square roots turn power ratios into amplitudes, which ramp about linearly
 * on a keyed edge. */
float KeyDetector::crossing() const {
    const float step = static_cast<float>(INTEG_STEP);
    const float span = static_cast<float>(INTEG_LEN);

    float a_new = std::sqrt(std::max(r_new_ - 1.0f, 0.0f));
    float a_old = std::sqrt(std::max(r_old_ - 1.0f, 0.0f));
    float a_mid = std::sqrt(std::max(mid_lvl_ - 1.0f, 0.0f));
    float slope = a_new - a_old;

    if (std::abs(slope) < 1e-3f)
        return -step;

    float t = -step * (a_new - a_mid) / slope;
    return std::clamp(t, -(span + step), 0.0f);
}

} // namespace cw
