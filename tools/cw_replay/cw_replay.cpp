// Offline replay of a radio's RX audio through the GUI's CW receiver (src/cw_decoder), as src/cw.cpp feeds it.
// stdin: raw float32 mono at 4000 Hz (the receiver's rate; replay.py makes it from a WAV), or with S16=1 16-bit
// little-endian, full scale 2.0 (test.sh's recording: the radio's audio peaks over 1). Args: band_lo band_hi
// floor_db [expect_wpm] (the CW filter's edges, Dec SNR). KEYS=1: each key down and up on stderr, by time (and by the
// detector's own step clock: what the receiver gives the detector again after a lock move or its first second comes
// out at once); TEXTS=1: each piece of text, by time; BASE_PEAK=tone:q: the X6200 base's resonance taken out (cw.cpp); APF=tone:q: the peak filter's
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "receiver.h"
#include "trainer_gate.h"

int main(int argc, char **argv) {
    float lo = argc > 1 ? atof(argv[1]) : 0, hi = argc > 2 ? atof(argv[2]) : 3000;
    float floor_db = argc > 3 ? atof(argv[3]) : 10;
    std::string text;
    int keys = 0;
    double t = 0;
    long steps = 0;   /* detector steps (8 ms): the time the detector keys by, also through its replays */
    cw::Receiver *rx = nullptr;
    /* TRAINER=1: the trainer's gate on the text too (trainer_gate.h, as src/cw.cpp has it) */
    cw::TrainerGate gate;
    bool down_now = false, gated = getenv("TRAINER") != nullptr;
    rx = new cw::Receiver(
        [&](const char *s, bool own) {
            if (gated && !gate.pass(s, own)) {
                if (getenv("TEXTS")) fprintf(stderr, "%7.3f text '%s' dropped (the trainer's gate)\n", t, s);
                return;
            }
            text += s;
            if (getenv("TEXTS")) fprintf(stderr, "%7.3f text '%s'\n", t, s);   /* TEXTS=1: each piece, by time */
        },
        [&](bool down) {
            down_now = down;
            if (down) keys++;
            if (getenv("KEYS")) fprintf(stderr, "%7.3f %s (step %.3f s)\n", t, down ? "down" : "up", steps * 0.008);
        },
        [&]() {
            steps++;
            if (down_now) gate.keyed_step(rx->snr_db());
        });
    rx->set_floor_db(floor_db);
    rx->set_band(lo, hi);
    /* APF=tone:q: the radio's peak filter on (its colour in the audio: replay.py does not put it there) */
    if (const char *ap = getenv("APF")) {
        float tone = 0, q = 0;
        if (sscanf(ap, "%f:%f", &tone, &q) == 2 && tone > 0 && q > 0)
            rx->set_apf(true, tone, q);
    }
    /* BASE_PEAK=tone:q: the X6200 base's resonance at its key tone in CW, as src/cw.cpp gives it there */
    if (const char *bp = getenv("BASE_PEAK")) {
        float tone = 0, q = 0;
        if (sscanf(bp, "%f:%f", &tone, &q) == 2 && tone > 0 && q > 0)
            rx->set_base_peak(true, tone, q);
    }
    if (argc > 4) rx->expect(atof(argv[4]));
    /* TRAINER=1: the CW trainer open (its speed range, Receiver::trainer_range); OWN=n: its next n elements our own
     * (Receiver::own); TX=a-b,c-d: the radio transmitting over those seconds (Receiver::transmitting), as in the
     * trainer's key train at 0 W, where the audio is its own sidetone */
    if (getenv("TRAINER")) rx->trainer_range(true);
    if (getenv("TRAINER") && argc > 4) rx->expect(atof(argv[4]));      /* the range clamps the speed: told after it */
    if (const char *on = getenv("OWN")) rx->own(atoi(on));
    if (const char *pn = getenv("PIN")) rx->pin(atof(pn));      /* PIN=hz: the tone pinned (the trainer's key tone) */
    std::vector<std::pair<double, double>> tx_spans;
    if (const char *sp = getenv("TX")) {
        for (const char *q = sp; *q;) {
            double a = 0, b = 0;
            if (sscanf(q, "%lf-%lf", &a, &b) == 2) tx_spans.emplace_back(a, b);
            const char *c = strchr(q, ',');
            if (!c) break;
            q = c + 1;
        }
    }
    bool tx_now = false;
    std::vector<float> buf(32);
    std::vector<int16_t> s16(buf.size());
    bool   is_s16 = getenv("S16") != nullptr;
    size_t n;
    float last_wpm = -1, last_lock = 0;
    while ((n = is_s16 ? fread(s16.data(), sizeof(int16_t), s16.size(), stdin)
                       : fread(buf.data(), sizeof(float), buf.size(), stdin)) > 0) {
        if (is_s16)
            for (size_t i = 0; i < n; i++) buf[i] = s16[i] / 16384.0f;
        if (!tx_spans.empty()) {
            bool on = false;
            for (auto &sp : tx_spans) on = on || (t >= sp.first && t < sp.second);
            if (on != tx_now) {
                rx->transmitting(on);
                tx_now = on;
                if (getenv("KEYS")) fprintf(stderr, "%7.3f transmitting %d\n", t, on ? 1 : 0);
            }
        }
        rx->feed(buf.data(), n);
        t += n / 4000.0;
        if (getenv("LOCKS") && std::abs(rx->lock_hz() - last_lock) >= 5.0f) {
            /* LOCKS=1: each move of the tone lock, by time (a lock that wanders in a quiet and comes back resets the
             * element timer: the first characters after it lost) */
            fprintf(stderr, "%7.3f lock %.0f -> %.0f Hz (peak %.0f Hz %.1f dB over the bins' noise, snr %.1f)\n", t,
                    last_lock, rx->lock_hz(), rx->peak_hz(), rx->peak_snr_db(), rx->snr_db());
            last_lock = rx->lock_hz();
        }
        if (rx->wpm() != last_wpm) {
            fprintf(stderr, "%6.2f s: wpm %.1f lock %.0f Hz peak %.0f Hz snr %.1f\n", t, rx->wpm(), rx->lock_hz(),
                    rx->peak_hz(), rx->snr_db());
            last_wpm = rx->wpm();
        }
    }
    printf("text: '%s' (key downs %d, wpm %.1f, lock %.0f Hz)\n", text.c_str(), keys, rx->wpm(), rx->lock_hz());
    return 0;
}
