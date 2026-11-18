#include <g90/defaults.hpp>

namespace g90::defaults {

// Product label 1.81 → V179 codec: USB, 2700/200 Hz filter, FFT scale 1.
Result<HeadFrame> encode_usb_v181(uint32_t freq_hz) {
  HeadEncodeRequest req;
  req.profile = Profile::V179;
  req.vfo_a.freq_hz = freq_hz;
  req.vfo_a.mode = Mode::Usb;
  req.vfo_a.filter_high_hz = 2700;
  req.vfo_a.filter_low_hz = 200;
  req.vfo_a.fft_scale = 1;
  return encode_head(req);
}

} // namespace g90::defaults
