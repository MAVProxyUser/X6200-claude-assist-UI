// Offline replay of a radio's RX audio through the GUI's CW receiver (src/cw_decoder), as src/cw.cpp feeds it.
// stdin: raw float32 mono at 4000 Hz (the receiver's rate; replay.py makes it from a WAV), or with S16=1 16-bit
// little-endian, full scale 2.0 (test.sh's recording: the radio's audio peaks over 1). Args: band_lo band_hi
// floor_db [expect_wpm] (the CW filter's edges, Dec SNR). KEYS=1: each key down and up on stderr, by time
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "receiver.h"

int main(int argc, char **argv) {
    float lo = argc > 1 ? atof(argv[1]) : 0, hi = argc > 2 ? atof(argv[2]) : 3000;
    float floor_db = argc > 3 ? atof(argv[3]) : 10;
    std::string text;
    int keys = 0;
    double t = 0;
    cw::Receiver *rx = nullptr;
    rx = new cw::Receiver(
        [&](const char *s, bool own) { text += s; (void)own; },
        [&](bool down) {
            if (down) keys++;
            if (getenv("KEYS")) fprintf(stderr, "%7.3f %s\n", t, down ? "down" : "up");
        },
        {});
    rx->set_floor_db(floor_db);
    rx->set_band(lo, hi);
    if (argc > 4) rx->expect(atof(argv[4]));
    std::vector<float> buf(32);
    std::vector<int16_t> s16(buf.size());
    bool   is_s16 = getenv("S16") != nullptr;
    size_t n;
    float last_wpm = -1;
    while ((n = is_s16 ? fread(s16.data(), sizeof(int16_t), s16.size(), stdin)
                       : fread(buf.data(), sizeof(float), buf.size(), stdin)) > 0) {
        if (is_s16)
            for (size_t i = 0; i < n; i++) buf[i] = s16[i] / 16384.0f;
        rx->feed(buf.data(), n);
        t += n / 4000.0;
        if (rx->wpm() != last_wpm) {
            fprintf(stderr, "%6.2f s: wpm %.1f lock %.0f Hz peak %.0f Hz snr %.1f\n", t, rx->wpm(), rx->lock_hz(),
                    rx->peak_hz(), rx->snr_db());
            last_wpm = rx->wpm();
        }
    }
    printf("text: '%s' (key downs %d, wpm %.1f, lock %.0f Hz)\n", text.c_str(), keys, rx->wpm(), rx->lock_hz());
    return 0;
}
