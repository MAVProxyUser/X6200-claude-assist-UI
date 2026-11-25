/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  CW receiver: finds the strongest tone in the filter band with a sliding
 *  FFT and keeps the per-bin noise floor.
 */

#pragma once

#include <array>
#include <complex>

#include "rx_defs.h"

#include <liquid/liquid.h>

namespace cw {

/* The radio's CW peak filter (APF) colours the noise before the decoder sees
 * it. It is a two-pole resonator run at 12.5 kHz on the receive audio; its
 * |H|^2 at each FFT bin lets the finder divide the colour back out. */
constexpr float APF_RATE = 12500.0f;
float apf_gain2(float tone_hz, float q, float hz);

/* The X6200's base colours CW's receive audio itself, the GUI's APF off: one resonance at the key tone, signal and
 * noise alike (2026-11-25, orch cw-noise-shape, nothing on the air: through 100-1300 Hz the noise 21 dB over the far
 * band at 700 Hz, -18 dB at 150 Hz, -6 dB 100-145 Hz wide; fitted within 0.5-1.7 dB rms by Q 9.4-10.9 at 709-714 Hz,
 * every CW filter alike; at a 600 Hz key tone at 600 Hz: it follows the key tone). Its |H|^2, 1 at the tone */
float base_peak_gain2(float tone_hz, float q, float hz);

class ToneFinder {
  public:
    ToneFinder();
    ~ToneFinder();
    ToneFinder(const ToneFinder &)            = delete;
    ToneFinder &operator=(const ToneFinder &) = delete;

    /* One sample in. Every FFT_STEP samples (once the FFT is full) a frame is
     * analysed: returns true and fills `out`. lock_hz is where the tone lock
     * sits now (for PeakReport::at_lock). */
    bool push(float x, float lock_hz, PeakReport &out);

    void  set_band(float lo_hz, float hi_hz);
    void  set_apf(bool on, float tone_hz, float q);
    bool  set_base_peak(bool on, float tone_hz, float q);   /* true when it changed */
    /* The colour (|H|^2 of the APF and the base's resonance) the finder divides out of the spectrum, as the tone
     * lock's step at hz takes it in: averaged over the step's 16 ms integrator (62 Hz wide). 1 with no colour */
    float colour_at(float hz) const;
    float noise() const { return noise_; }   /* per-bin noise power, < 0 until the first frame */
    /* The key is down, or its mark is still in the FFT window: the noise is not
     * followed then (Receiver::feed) */
    void  hold_noise(bool on) { hold_ = on; }
    /* The radio is transmitting: its own sidetone, no band (analyse). When the
     * band comes back the floor is seeded again from its first frame */
    /* The band's level changed (the decoder's audio back after other modes, another CW filter): the floor seeded
     * again from the next frame, as after a transmission */
    void  reseed() {
        noise_  = -1.0f;
        wait_n_ = 0;
    }
    void  transmitting(bool on) {
        if (tx_ && !on) {
            noise_  = -1.0f;
            wait_n_ = 0;
        }
        tx_ = on;
    }
    float peak_hz() const { return peak_hz_; }

    /* Last FFT frame, non-negative half (for the tests) */
    const std::array<std::complex<float>, FFT_BINS> &spectrum() const { return spec_; }

  private:
    void analyse(float lock_hz, PeakReport &out);

    /* Samples twice over, so the last FFT_LEN are always contiguous */
    std::array<float, 2 * FFT_LEN> ring_{};
    size_t ring_pos_ = 0;
    size_t filled_   = 0;

    std::array<float, FFT_LEN>               hann_;
    std::array<std::complex<float>, FFT_LEN> time_;
    std::array<std::complex<float>, FFT_LEN> freq_;
    std::array<std::complex<float>, FFT_BINS> spec_;
    fftplan plan_ = nullptr;

    float colour2(float hz, bool base) const;   /* |H|^2 at hz: the APF's, and the base's with base */
    void  colour();                             /* g2_ and g2_tx_ from them */

    bool                         apf_on_ = false;
    float                        apf_tone_ = 0.0f, apf_q_ = 1.0f;
    bool                         base_on_ = false;
    float                        base_tone_ = 0.0f, base_q_ = 1.0f;
    std::array<float, FFT_BINS>  g2_;       /* receiving: the APF's and the base's */
    std::array<float, FFT_BINS>  g2_tx_;    /* transmitting: the APF's alone (colour_at) */
    std::array<float, FFT_BINS>  pwr_{};
    std::array<float, FFT_BINS>  sorted_{};

    size_t first_ = 0, last_ = 0; /* band, in bins */
    float  noise_    = -1.0f;
    bool   hold_     = false;
    bool   tx_       = false;
    /* Frames' band noise waiting to be averaged in (analyse): a key down drops them */
    std::array<float, 3> wait_{};
    size_t wait_n_   = 0;
    float  peak_hz_  = 0.0f;
    int    last_bin_ = -1;        /* bin of the last strong frame */
    int    steady_   = 0;
};

} // namespace cw
