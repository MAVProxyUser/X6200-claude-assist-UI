/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: times the key edges into dits, dahs and gaps and learns the
 *  sender's speed.
 */

#pragma once

#include <array>

#include "rx_defs.h"

namespace cw {

/* Histogram of mark and gap lengths, 4 ms per slot (up to 1.2 s) */
constexpr size_t SLOTS   = 300;
constexpr float  SLOT_MS = STEP_MS / 2.0f;

/* Speed range: a dot of 24-120 ms (50-10 WPM), 20 WPM to start. With the CW
 * trainer open down to 240 ms, 5 WPM (trainer_range) */
constexpr float  DOT_START_MS = PARIS_MS / 20.0f;
constexpr float  DOT_MIN_MS   = PARIS_MS / 50.0f;
constexpr float  DOT_MAX_MS   = PARIS_MS / 10.0f;
constexpr float  TRAINER_DOT_MAX_MS = PARIS_MS / 5.0f;

/* Every histogram entry ages by this when a new one comes in */
constexpr float  AGE = 0.9f;

/* A text's first elements are held (as many as this) and timed together before any is classified: see hold() */
constexpr int    FRESH_MAX = 12;

/* Each step the timer gets the detector's edge (if any). The time of a step
 * that holds an edge is split at the crossing, so the closing mark or gap
 * gets its part and the new one the rest: marks and gaps of one unit then
 * fall on the same spot of the histogram. The dot length is the centre of
 * the lowest cluster there. */
class ElementTimer {
  public:
    /* dir +1 key down, -1 key up, 0 nothing; back_ms: the crossing lies that
     * long before this step; strong: (key up) the mark was well above the floor */
    Sym   step(int dir, float back_ms, bool strong = true);

    float wpm() const { return static_cast<float>(PARIS_MS) / dot_ms_; }
    float dot_ms() const { return dot_ms_; }
    bool  key_down() const { return down_; }

    /* The lock moved to another tone: the running mark or gap is not learned
     * (it may have been measured on the old tone), the speed is kept */
    void  new_tone() { unsure_ = true; }
    /* The running gap is the pause between two senders taking turns (the CW
     * trainer's group, then the user's answer): no gap of either, not learned.
     * Learned, the answer's delay went in as a Farnsworth character gap, and
     * after GAPS_MIN of them the word boundary moved up with it */
    void  turn() { unsure_ = true; }
    /* The running mark or gap is our own keying (the CW trainer's, at the
     * speed it gave: expect): not learned. Right after the lock has found the
     * tone the first dah can come in pieces, and in a histogram expect() had
     * just started over a few of them set the speed to 50 WPM */
    void  ours() { unsure_ = true; own_ = true; }
    /* The running mark or gap is not learned, though a station's: given again by a rewind (Receiver::rewind), or
     * while the lock holds no station yet. A text's held first elements are timed with it all the same (hold) */
    void  unlearned() { unsure_ = true; }
    /* Keying at a known speed follows (the CW trainer's own, at the key
     * speed): a dot more than NEW_SPEED off is taken at once, the histogram
     * started over, instead of after MIN_ENTRIES and CONFIRM estimates */
    void  expect(float wpm);
    /* The CW trainer is open: the speed goes down to 5 WPM, the lowest key
     * speed, as the trainer keys and a straight key answers it (at 10 WPM a
     * 5 WPM dit sat on the dit/dah boundary and every element gap was a
     * character gap); closed, 10 WPM again, a slower speed taken up to it.
     * Only for the trainer: on the band nothing slower was ever tried, and
     * the rejection of noise and carriers (lone marks, CARRIER_K, the key
     * detector's carrier floor after 480 ms down) was set for 10 WPM and up */
    void  trainer_range(bool on);

  private:
    friend struct ElementTimerPeek;   /* tests only */

    Sym   close(bool mark, float ms, bool learn);
    void  hold(bool mark, float ms, bool learn, bool own);
    bool  plain(bool mark, float ms) const;
    void  flush();
    void  put(Sym s);
    void  learn(float ms);
    float dot_estimate(float min_peak) const;
    void  set_dot(float ms, float entry_ms);   /* entry_ms: the element just learned */
    bool  backed(float ms) const;
    void  retime();

    std::array<float, SLOTS> hist_{};
    int   hist_n_  = 0;     /* entries since the histogram was last emptied */
    float age_     = AGE;

    bool  down_    = false;
    float run_ms_  = 0.0f;  /* length of the running mark or gap */
    bool  ended_   = true;  /* the end of the text was already sent for this gap */
    bool  unsure_  = false; /* the running mark or gap began on another tone */
    bool  own_     = false; /* the running mark or gap is our own keying (ours) */

    /* A mark with a word gap before it is held until we know whether more
     * follows: alone it is an E or a T, or a noise blip */
    bool  first_       = true;
    bool  held_        = false;
    float held_ms_     = 0.0f;
    bool  held_learn_  = false;
    bool  held_strong_ = false;
    bool  held_own_    = false;

    /* The symbols due, one sent each step, in order: a step can close two, and a flush (hold) as many as were held */
    std::array<Sym, 2 * FRESH_MAX + 8> q_{};
    int   q_head_ = 0, q_n_ = 0;

    /* A text's first elements, held while fresh_ (hold) */
    struct Raw {
        bool  mark;
        float ms;
        bool  learn;
        bool  own;
    };
    std::array<Raw, FRESH_MAX> buf_{};
    int   buf_n_  = 0;
    bool  fresh_  = true;
    int   plain_n_ = 0;     /* a fresh text's elements in a row so far that were plain at the speed held (plain) */

    /* The last gaps of a character gap or longer: their median is the
     * character gap, which slow (Farnsworth) sending stretches */
    std::array<float, 16> gaps_{};
    int   gaps_n_   = 0;
    int   gaps_pos_ = 0;

    float dot_ms_   = DOT_START_MS;
    float dot_max_  = DOT_MAX_MS;      /* the slowest speed's dot (trainer_range) */
    float new_dot_  = -1.0f;   /* a different speed, waiting to be confirmed */
    int   new_seen_ = 0;

    float dah_from_  = 2.0f * DOT_START_MS;  /* mark: dit under, dah over */
    float char_from_ = 1.7f * DOT_START_MS;  /* gap: character gap from here */
    float word_from_ = 5.0f * DOT_START_MS;  /* gap: word gap from here */
};

} // namespace cw
