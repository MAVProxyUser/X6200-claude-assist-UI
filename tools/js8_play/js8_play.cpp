/*
 *  SPDX-License-Identifier: GPL-3.0-only
 *
 *  OpenX6X00 - JS8 from a recording: plays a WAV through the radio's own JS8 receiver (src/js8 on js8core) as if it
 *  were coming in live, with JS8 time set to the moment the recording began.
 *
 *  The receiver lines the decoder's 60 s ring up with JS8 time (the system clock plus the Time Sync drift) on its first
 *  audio, and from then on decodes by counting samples. So the drift is set to "recording start - now" just before the
 *  first sample goes in, the clock check that would realign a fast feed is off (realign_threshold_ms 0, as the tests
 *  do), and the file is fed faster than real time; silence after it lets the last slot finish.
 *
 *    js8_play FILE.wav START [--speeds normal,fast,turbo,slow]
 *        START: when the recording's first sample was taken, UTC: seconds since 1970 (fractions allowed) or
 *        2026-11-08T01:02:45.3Z
 *    js8_play --selftest
 *        a made-up band from make_test_band() (test_signal.cpp), decoded the same way: the stations' texts come back
 */

#include "rx_engine.hpp"
#include "test_signal.hpp"
#include "wav_file.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace x6100::js8;

static std::int64_t system_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static double parse_start(const char *s) {
    int Y, M, D, h, m;
    double sec;
    if (std::sscanf(s, "%d-%d-%dT%d:%d:%lfZ", &Y, &M, &D, &h, &m, &sec) == 6) {
        std::tm t = {};
        t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m;
        return (double)timegm(&t) + sec;
    }
    return std::atof(s);
}

static int parse_speeds(const char *s) {
    int mask = 0;
    if (std::strstr(s, "normal")) mask |= SUBMODE_NORMAL;
    if (std::strstr(s, "fast")) mask |= SUBMODE_FAST;
    if (std::strstr(s, "turbo")) mask |= SUBMODE_TURBO;
    if (std::strstr(s, "slow")) mask |= SUBMODE_SLOW;
    return mask ? mask : SUBMODE_NORMAL;
}

static const char *speed_name(int mode) {
    switch (mode) { case 0: return "normal"; case 1: return "fast"; case 2: return "turbo"; case 4: return "slow"; }
    return "?";
}

/* Decodes `audio` (at `rate`) whose first sample was taken at `start_s`; prints each frame and each whole message.
 * Returns the number of frames */
static int play(const std::vector<float> &audio, int rate, double start_s, int speeds) {
    std::mutex              lock;
    std::condition_variable changed;
    int                     frames = 0, cycles = 0;
    std::atomic<double>     fed_s{0.0};   // seconds of the recording given to the receiver so far

    // The recording's time when a decode comes out: the slot it is from ended shortly before. (The engine's own
    // stamp is the live clock's, which a recording played later does not match.)
    auto heard_at = [&](char *buf, std::size_t n) {
        std::time_t t  = (std::time_t)(start_s + fed_s.load());
        std::tm     tm = *std::gmtime(&t);
        std::strftime(buf, n, "%H:%M:%S", &tm);
    };
    Receiver::Callbacks cb;
    cb.on_frame = [&](const RxFrame &f) {
        std::lock_guard<std::mutex> hold(lock);
        char at[16];
        heard_at(at, sizeof(at));
        std::printf("frame    ~%s  %+3d dB  dt %+5.1f  %6.1f Hz  %-6s  %s%s\n", at, f.snr, f.dt, f.freq_hz,
                    speed_name(f.mode), f.text.c_str(), f.low_confidence ? "  (low confidence)" : "");
        std::fflush(stdout);
        ++frames;
    };
    cb.on_message = [&](const RxFrame &f) {
        if (f.partial) return;
        std::lock_guard<std::mutex> hold(lock);
        char at[16];
        heard_at(at, sizeof(at));
        std::printf("message  ~%s  %+3d dB  %6.1f Hz  %s\n", at, f.snr, f.freq_hz, f.text.c_str());
        std::fflush(stdout);
    };
    cb.on_cycle_done = [&](std::size_t) {
        std::lock_guard<std::mutex> hold(lock);
        ++cycles;
        changed.notify_all();
    };

    Receiver::Config cfg;
    cfg.input_rate           = rate;
    cfg.submodes             = speeds;
    cfg.realign_threshold_ms = 0;   // fed faster than real time: no clock check after the first audio

    // Silence after the recording, so its last slot (and a slow one, 30 s) completes
    std::vector<float> all(audio);
    all.resize(all.size() + (std::size_t)rate * 32, 0.0f);

    {
        Receiver rx(cfg, cb);
        rx.set_decode_range(0, 5000);
        // JS8 time is the recording's from here on: the receiver lines its ring up with it on the first audio
        set_drift_ms((std::int64_t)std::llround(start_s * 1000.0) - system_ms());
        const std::size_t chunk = (std::size_t)rate / 10;   // 100 ms
        for (std::size_t i = 0; i < all.size(); i += chunk) {
            rx.feed(all.data() + i, std::min(chunk, all.size() - i));
            fed_s.store((double)(i + chunk) / rate);
            // Five times real time: quick, and each slot's decode is over before the next is due
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // The decodes of the last slots: wait until no new cycle has ended for 3 s
        std::unique_lock<std::mutex> hold(lock);
        for (int seen = -1; seen != cycles;) {
            seen = cycles;
            changed.wait_for(hold, std::chrono::seconds(3));
        }
    }
    return frames;
}

int main(int argc, char **argv) {
    // The made-up band as a recording would have it: 1.3 s of noise before the slot, written to FILE; play it back
    // with START = (the printed slot time) - 1.3
    if (argc >= 3 && !std::strcmp(argv[1], "--make-test")) {
        std::vector<TestStation> band = {{"K1ABC", "FN42", "CQ CQ CQ FN42", 1000.0, -5.0},
                                         {"W8XYZ", "EN80", "K1ABC SNR?", 1600.0, -10.0}};
        std::vector<float> audio = make_test_band(band, 12000);
        std::vector<float> lead((std::size_t)(1.3 * 12000));
        std::uint32_t      r = 7;
        for (float &v : lead) { r = r * 1664525u + 1013904223u; v = 0.02f * ((float)(r >> 8) / 8388608.0f - 1.0f); }
        audio.insert(audio.begin(), lead.begin(), lead.end());
        return write_wav(argv[2], 12000, audio) ? 0 : 1;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "--selftest")) {
        std::vector<TestStation> band = {
            {"K1ABC", "FN42", "CQ CQ CQ FN42", 1000.0, -5.0},
            {"W8XYZ", "EN80", "K1ABC SNR?", 1600.0, -10.0},
        };
        std::vector<float> audio = make_test_band(band, 12000);
        // make_test_band's sample 0 starts a slot at every speed, as a 30 s boundary does
        double start = std::floor((double)std::time(nullptr) / 30.0) * 30.0 - 60.0;
        std::printf("selftest: %zu s of a made-up band, sent: K1ABC \"CQ CQ CQ FN42\" at 1000 Hz, "
                    "W8XYZ \"K1ABC SNR?\" at 1600 Hz\n", audio.size() / 12000);
        int n = play(audio, 12000, start, SUBMODE_NORMAL);
        std::printf("selftest: %d frames\n", n);
        return n > 0 ? 0 : 1;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s FILE.wav START_UTC [--speeds normal,fast,turbo,slow] | --selftest\n", argv[0]);
        return 2;
    }
    WavAudio    wav;
    std::string err;
    if (!read_wav(argv[1], wav, err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    int speeds = SUBMODE_NORMAL;
    for (int i = 3; i + 1 < argc; i++)
        if (!std::strcmp(argv[i], "--speeds")) speeds = parse_speeds(argv[i + 1]);
    double start = parse_start(argv[2]);
    std::printf("%s: %.1f s at %d Hz from %.1f\n", argv[1], (double)wav.samples.size() / wav.rate, wav.rate, start);
    int n = play(wav.samples, wav.rate, start, speeds);
    std::printf("%d frames\n", n);
    return 0;
}
