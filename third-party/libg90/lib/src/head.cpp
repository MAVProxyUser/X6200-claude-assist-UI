/// Head frame encode/decode (96-byte command). See `docs/ENCODE-PADDING.md` and
/// `docs/DESIGN.md` for stock padding and TX policy.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <g90/crc.hpp>
#include <g90/head.hpp>
#include <span>
#include <string>

namespace g90 {
namespace {

constexpr uint8_t kSync0 = 0x55;
constexpr uint8_t kSync1 = 0xAA;

constexpr uint8_t kDefaultUnknown3[] = {0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kDefaultUnknown4[] = {
    0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x1F, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x32,
};
constexpr uint8_t kDefaultUnknown5[] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};

[[nodiscard]] std::string ascii_upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

void store_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

[[nodiscard]] uint32_t load_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void pack_vfo(uint8_t* buf,
              std::size_t base,
              uint32_t freq_hz,
              uint8_t mode,
              uint8_t att_mode,
              uint8_t agc,
              uint8_t filter_high_raw,
              uint8_t filter_low_raw,
              uint8_t fft_scale,
              uint8_t pad_byte = 0) {
  store_le32(buf + base, freq_hz);
  buf[base + 4] = att_mode;
  buf[base + 5] = mode;
  buf[base + 6] = agc;
  buf[base + 7] = pad_byte;
  buf[base + 8] = filter_high_raw;
  buf[base + 9] = filter_low_raw;
  buf[base + 15] = static_cast<uint8_t>(fft_scale & 0x0F);
}

// Overlay packing: each set HeadControls field patches specific byte offsets
// (36–91) on top of the stock template; unset optionals leave template bytes.
void apply_head_controls(uint8_t* buf, const HeadControls& c) {
  uint8_t c1 = buf[36];
  if (c.nb_en) {
    c1 = *c.nb_en ? static_cast<uint8_t>(c1 | kHeadCtrl1NbEn)
                  : static_cast<uint8_t>(c1 & ~kHeadCtrl1NbEn);
  }
  if (c.split_en) {
    c1 = *c.split_en ? static_cast<uint8_t>(c1 | kHeadCtrl1Split)
                     : static_cast<uint8_t>(c1 & ~kHeadCtrl1Split);
  }
  if (c.headphones) {
    c1 = *c.headphones ? static_cast<uint8_t>(c1 | kHeadCtrl1Headphones)
                       : static_cast<uint8_t>(c1 & ~kHeadCtrl1Headphones);
  }
  if (c.mic_compression) {
    c1 = *c.mic_compression ? static_cast<uint8_t>(c1 | kHeadCtrl1MicComp)
                            : static_cast<uint8_t>(c1 & ~kHeadCtrl1MicComp);
  }
  if (c.tuner_en) {
    c1 = *c.tuner_en ? static_cast<uint8_t>(c1 | kHeadCtrl1TunerEn)
                     : static_cast<uint8_t>(c1 & ~kHeadCtrl1TunerEn);
  }
  if (c.panel_lock) {
    c1 = *c.panel_lock ? static_cast<uint8_t>(c1 | kHeadCtrl1PanelLock)
                       : static_cast<uint8_t>(c1 & ~kHeadCtrl1PanelLock);
  }
  if (c.mem_en) {
    c1 = *c.mem_en ? static_cast<uint8_t>(c1 | kHeadCtrl1MemEn)
                   : static_cast<uint8_t>(c1 & ~kHeadCtrl1MemEn);
  }
  buf[36] = c1;

  if (c.shutdown_req) {
    if (*c.shutdown_req) {
      buf[37] = static_cast<uint8_t>(buf[37] | kHeadCtrl2ShutdownBit);
    } else {
      buf[37] = static_cast<uint8_t>(buf[37] & ~kHeadCtrl2ShutdownBit);
    }
  }

  uint8_t c3 = buf[38];
  if (c.audio_in_line_en) {
    c3 = *c.audio_in_line_en ? static_cast<uint8_t>(c3 | kHeadCtrl3AudioLine)
                             : static_cast<uint8_t>(c3 & ~kHeadCtrl3AudioLine);
  }
  if (c.cw_disp_en) {
    c3 = *c.cw_disp_en ? static_cast<uint8_t>(c3 | kHeadCtrl3CwDisp)
                       : static_cast<uint8_t>(c3 & ~kHeadCtrl3CwDisp);
  }
  if (c.cw_qsk) {
    c3 = *c.cw_qsk ? static_cast<uint8_t>(c3 | kHeadCtrl3CwQsk)
                   : static_cast<uint8_t>(c3 & ~kHeadCtrl3CwQsk);
  }
  if (c.rclk_hz) {
    const auto [low, high] = pack_rclk_hz(*c.rclk_hz);
    c3 = static_cast<uint8_t>((c3 & 0x1F) | ((low & 7) << 5));
    buf[39] = high;
  }
  buf[38] = c3;

  if (c.nb_level || c.nb_width) {
    const uint8_t level = c.nb_level ? static_cast<uint8_t>(*c.nb_level & 0xF)
                                     : static_cast<uint8_t>((buf[46] >> 4) & 0xF);
    const uint8_t width =
        c.nb_width ? static_cast<uint8_t>(*c.nb_width & 0xF) : static_cast<uint8_t>(buf[46] & 0xF);
    buf[46] = static_cast<uint8_t>((level << 4) | width);
  }

  if (c.sql_level) {
    buf[45] = *c.sql_level;
  }
  if (c.mic_gain) {
    buf[49] = *c.mic_gain;
  }
  if (c.rf_power) {
    buf[44] = *c.rf_power;
  }
  if (c.volume) {
    buf[48] = *c.volume;
  }
  if (c.cw_wpm) {
    buf[60] = *c.cw_wpm;
  }
  if (c.cw_qsk_time_raw) {
    buf[51] = *c.cw_qsk_time_raw;
  }
  if (c.mem_ch) {
    buf[53] = *c.mem_ch;
  }

  if (c.vfo_b_en) {
    if (*c.vfo_b_en) {
      buf[54] = static_cast<uint8_t>(buf[54] | kHeadCtrl6VfoB);
    } else {
      buf[54] = static_cast<uint8_t>(buf[54] & ~kHeadCtrl6VfoB);
    }
  }

  if (c.cw_ratio_raw || c.cw_mode_b || c.cw_mlr) {
    const uint8_t cur = buf[61];
    const uint8_t ratio = c.cw_ratio_raw ? static_cast<uint8_t>(*c.cw_ratio_raw & 0xF)
                                         : static_cast<uint8_t>((cur >> 4) & 0xF);
    const bool mode_b = c.cw_mode_b ? *c.cw_mode_b : ((cur & 0x04) != 0);
    const uint8_t mlr =
        c.cw_mlr ? static_cast<uint8_t>(*c.cw_mlr & 3) : static_cast<uint8_t>(cur & 3);
    buf[61] = pack_ctrl7(ratio, mode_b, mlr);
  }

  if (c.swr_threshold_raw || c.aux_out_vol || c.aux_in_vol) {
    const uint8_t swr = c.swr_threshold_raw ? static_cast<uint8_t>(*c.swr_threshold_raw & 0xF)
                                            : static_cast<uint8_t>(buf[82] & 0xF);
    const uint8_t aout = c.aux_out_vol ? static_cast<uint8_t>(*c.aux_out_vol & 0xF)
                                       : static_cast<uint8_t>((buf[83] >> 4) & 0xF);
    const uint8_t ain = c.aux_in_vol ? static_cast<uint8_t>(*c.aux_in_vol & 0xF)
                                     : static_cast<uint8_t>(buf[83] & 0xF);
    const auto packed = pack_ctrl8(swr, aout, ain);
    buf[82] = packed.first;
    buf[83] = packed.second;
  }

  if (c.rit_hz) {
    const auto [low, high] = pack_rit_hz(*c.rit_hz);
    buf[84] = low;
    buf[85] = static_cast<uint8_t>((buf[85] & 0xFC) | (high & 3));
  }

  if (c.af_gain || c.beep_en || c.band_stack_full) {
    const int cur_gain = ((buf[43] & 3) << 5) | ((buf[42] >> 3) & 0x1F);
    const bool cur_beep = (buf[43] & 0x08) != 0;
    const bool cur_stack = (buf[43] & 0x04) != 0;
    const int gain = c.af_gain ? static_cast<int>(*c.af_gain) : cur_gain;
    const bool beep = c.beep_en ? *c.beep_en : cur_beep;
    const bool stack = c.band_stack_full ? *c.band_stack_full : cur_stack;
    const auto packed = pack_af_gain_beep(gain, beep, stack);
    buf[42] = packed.first;
    buf[43] = packed.second;
  }
}

void arm_head_ptt(uint8_t* buf) {
  buf[kHeadPttByte] = static_cast<uint8_t>(buf[kHeadPttByte] | kHeadPttBit);
  buf[37] = static_cast<uint8_t>(buf[37] & ~kHeadCtrl2TuningBit);
  buf[38] = static_cast<uint8_t>((buf[38] | kHeadCtrl3Operate) & ~kHeadCtrl3VoxEn);
}

void arm_head_atu_tune(uint8_t* buf) {
  buf[kHeadPttByte] = static_cast<uint8_t>(buf[kHeadPttByte] & ~kHeadPttBit);
  buf[37] = static_cast<uint8_t>((buf[37] | kHeadCtrl2TuningBit) & ~kHeadCtrl2ShutdownBit);
  buf[38] = static_cast<uint8_t>((buf[38] | kHeadCtrl3Operate) & ~kHeadCtrl3VoxEn);
}

void arm_head_vox(uint8_t* buf) {
  buf[kHeadPttByte] = static_cast<uint8_t>(buf[kHeadPttByte] & ~kHeadPttBit);
  buf[37] = static_cast<uint8_t>(buf[37] & ~kHeadCtrl2TuningBit);
  buf[38] = static_cast<uint8_t>(buf[38] | kHeadCtrl3Operate | kHeadCtrl3VoxEn);
}

// Default operate path when LabExtras does not clear_operate and does not
// allow_tx: healthy RX stream (operate ON, PTT/tune/VOX OFF). Distinct from
// clear_operate (clear operate/PTT/tune/VOX before CRC) and from arm_* when
// allow_tx is true.
void apply_stock_operate(uint8_t* buf) {
  buf[kHeadPttByte] = static_cast<uint8_t>(buf[kHeadPttByte] & ~kHeadPttBit);
  buf[37] = static_cast<uint8_t>(buf[37] & ~kHeadCtrl2TuningBit);
  buf[38] = static_cast<uint8_t>((buf[38] | kHeadCtrl3Operate) & ~kHeadCtrl3VoxEn);
}

// Filter resolution: raw pair overrides Hz when both raws are set; partial raw
// pair is an error. Otherwise encode from Hz with profile stock defaults.
[[nodiscard]] Result<FilterRaw>
resolve_filter(Profile profile, const HeadVfo& vfo, uint16_t stock_high_hz, uint16_t stock_low_hz) {
  const bool has_hi = vfo.filter_high_raw.has_value();
  const bool has_lo = vfo.filter_low_raw.has_value();
  if (has_hi != has_lo) {
    return Result<FilterRaw>::Err(Error::InvalidFilter);
  }
  if (has_hi) {
    FilterRaw raw;
    raw.high = *vfo.filter_high_raw;
    raw.low = *vfo.filter_low_raw;
    return Result<FilterRaw>::Ok(raw);
  }
  const uint16_t high = vfo.filter_high_hz.value_or(stock_high_hz);
  const uint16_t low = vfo.filter_low_hz.value_or(stock_low_hz);
  return Result<FilterRaw>::Ok(encode_filter(profile, high, low));
}

} // namespace

Result<Profile> parse_profile(std::string_view s) {
  std::string key(s);
  for (char& c : key) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  // Trim
  while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) {
    key.erase(key.begin());
  }
  while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) {
    key.pop_back();
  }
  if (key == "1.78" || key == "178" || key == "v178" || key == "gsoc") {
    return Result<Profile>::Ok(Profile::V178);
  }
  if (key == "1.79" || key == "1.79+" || key == "179" || key == "v179" || key == "1.81") {
    return Result<Profile>::Ok(Profile::V179);
  }
  return Result<Profile>::Err(Error::InvalidProfile);
}

Profile profile_for_body_version(uint8_t major, uint8_t minor) noexcept {
  const unsigned ver = (static_cast<unsigned>(major) << 8) | minor;
  return ver < 0x179 ? Profile::V178 : Profile::V179;
}

Result<Mode> mode_from_string(std::string_view s) {
  std::string key = ascii_upper(s);
  if (key == "LSB-DIG" || key == "LSB_DIG" || key == "LD") {
    key = "L-D";
  } else if (key == "USB-DIG" || key == "USB_DIG" || key == "UD") {
    key = "U-D";
  } else if (key == "CW-R") {
    key = "CWR";
  }
  for (char& c : key) {
    if (c == '_') {
      c = '-';
    }
  }
  if (key == "LSB") {
    return Result<Mode>::Ok(Mode::Lsb);
  }
  if (key == "L-D") {
    return Result<Mode>::Ok(Mode::Ld);
  }
  if (key == "USB") {
    return Result<Mode>::Ok(Mode::Usb);
  }
  if (key == "U-D") {
    return Result<Mode>::Ok(Mode::Ud);
  }
  if (key == "CW") {
    return Result<Mode>::Ok(Mode::Cw);
  }
  if (key == "CWR") {
    return Result<Mode>::Ok(Mode::Cwr);
  }
  if (key == "AM") {
    return Result<Mode>::Ok(Mode::Am);
  }
  if (key == "NFM") {
    return Result<Mode>::Ok(Mode::Nfm);
  }
  return Result<Mode>::Err(Error::InvalidMode);
}

std::string_view mode_to_string(Mode m) noexcept {
  switch (m) {
  case Mode::Lsb:
    return "LSB";
  case Mode::Ld:
    return "L-D";
  case Mode::Usb:
    return "USB";
  case Mode::Ud:
    return "U-D";
  case Mode::Cw:
    return "CW";
  case Mode::Cwr:
    return "CWR";
  case Mode::Am:
    return "AM";
  case Mode::Nfm:
    return "NFM";
  }
  return "UNK";
}

Result<uint8_t> mode_to_raw(Profile profile, Mode mode) {
  if (profile == Profile::V178) {
    switch (mode) {
    case Mode::Lsb:
      return Result<uint8_t>::Ok(0);
    case Mode::Usb:
      return Result<uint8_t>::Ok(1);
    case Mode::Cw:
      return Result<uint8_t>::Ok(2);
    case Mode::Cwr:
      return Result<uint8_t>::Ok(3);
    case Mode::Am:
      return Result<uint8_t>::Ok(4);
    case Mode::Nfm:
      return Result<uint8_t>::Ok(5);
    case Mode::Ld:
    case Mode::Ud:
      return Result<uint8_t>::Err(Error::InvalidMode);
    }
  }
  // V179 / 1.81
  switch (mode) {
  case Mode::Lsb:
    return Result<uint8_t>::Ok(0);
  case Mode::Ld:
    return Result<uint8_t>::Ok(1);
  case Mode::Usb:
    return Result<uint8_t>::Ok(2);
  case Mode::Ud:
    return Result<uint8_t>::Ok(3);
  case Mode::Cw:
    return Result<uint8_t>::Ok(4);
  case Mode::Cwr:
    return Result<uint8_t>::Ok(5);
  case Mode::Am:
    return Result<uint8_t>::Ok(6);
  case Mode::Nfm:
    return Result<uint8_t>::Ok(7);
  }
  return Result<uint8_t>::Err(Error::InvalidMode);
}

Result<Mode> mode_from_raw(Profile profile, uint8_t raw) {
  if (profile == Profile::V178) {
    switch (raw) {
    case 0:
      return Result<Mode>::Ok(Mode::Lsb);
    case 1:
      return Result<Mode>::Ok(Mode::Usb);
    case 2:
      return Result<Mode>::Ok(Mode::Cw);
    case 3:
      return Result<Mode>::Ok(Mode::Cwr);
    case 4:
      return Result<Mode>::Ok(Mode::Am);
    case 5:
      return Result<Mode>::Ok(Mode::Nfm);
    default:
      return Result<Mode>::Err(Error::InvalidMode);
    }
  }
  switch (raw) {
  case 0:
    return Result<Mode>::Ok(Mode::Lsb);
  case 1:
    return Result<Mode>::Ok(Mode::Ld);
  case 2:
    return Result<Mode>::Ok(Mode::Usb);
  case 3:
    return Result<Mode>::Ok(Mode::Ud);
  case 4:
    return Result<Mode>::Ok(Mode::Cw);
  case 5:
    return Result<Mode>::Ok(Mode::Cwr);
  case 6:
    return Result<Mode>::Ok(Mode::Am);
  case 7:
    return Result<Mode>::Ok(Mode::Nfm);
  default:
    return Result<Mode>::Err(Error::InvalidMode);
  }
}

FilterRaw encode_filter(Profile profile, uint16_t high_hz, uint16_t low_hz) {
  int hi = 0;
  int lo = 0;
  if (profile == Profile::V178) {
    hi = (static_cast<int>(high_hz) - 125) / 25;
    lo = (static_cast<int>(low_hz) - 100) / 25;
  } else {
    hi = high_hz < 50 ? 0 : (static_cast<int>(high_hz) - 50) / 50;
    lo = low_hz < 50 ? 0 : (static_cast<int>(low_hz) - 50) / 50;
  }
  FilterRaw out;
  out.high = static_cast<uint8_t>(std::clamp(hi, 0, 255));
  out.low = static_cast<uint8_t>(std::clamp(lo, 0, 255));
  return out;
}

FilterHz decode_filter(Profile profile, uint8_t high_raw, uint8_t low_raw) {
  FilterHz out;
  if (profile == Profile::V178) {
    out.high = static_cast<uint16_t>(125 + high_raw * 25);
    out.low = static_cast<uint16_t>(100 + low_raw * 25);
  } else {
    out.high = static_cast<uint16_t>(50 + high_raw * 50);
    out.low = static_cast<uint16_t>(50 + low_raw * 50);
  }
  return out;
}

Result<uint8_t> validate_fft_scale(Profile profile, uint8_t value) {
  const uint8_t v = static_cast<uint8_t>(value & 0x0F);
  if (profile == Profile::V178) {
    if (v < 1 || v > 10) {
      return Result<uint8_t>::Err(Error::InvalidFftScale);
    }
    return Result<uint8_t>::Ok(v);
  }
  if (v != 1) {
    return Result<uint8_t>::Err(Error::InvalidFftScale);
  }
  return Result<uint8_t>::Ok(1);
}

std::pair<uint8_t, uint8_t> pack_rclk_hz(int hz) {
  const int raw = std::clamp(1000 + hz, 0, 2047);
  return {static_cast<uint8_t>(raw & 7), static_cast<uint8_t>((raw >> 3) & 0xFF)};
}

int unpack_rclk_hz(uint8_t low3, uint8_t high) {
  return ((static_cast<int>(high) << 3) + (static_cast<int>(low3) & 7)) - 1000;
}

std::pair<uint8_t, uint8_t> pack_rit_hz(int hz) {
  const int v = std::clamp(hz, -512, 511);
  const int wire = v & 0x3FF;
  return {static_cast<uint8_t>(wire & 0xFF), static_cast<uint8_t>((wire >> 8) & 3)};
}

int unpack_rit_hz(uint8_t low, uint8_t high2) {
  const int wire = ((static_cast<int>(high2) & 3) << 8) | (static_cast<int>(low) & 0xFF);
  return ((wire + 512) % 1024) - 512;
}

std::pair<uint8_t, uint8_t> pack_af_gain_beep(int gain, bool beep_en, bool band_stack_full) {
  const int g = std::clamp(gain, 0, 127);
  const uint8_t gain_low = static_cast<uint8_t>(g & 0x1F);
  const uint8_t gain_high = static_cast<uint8_t>((g >> 5) & 3);
  const uint8_t b42 = static_cast<uint8_t>((gain_low << 3) & 0xF8);
  const uint8_t b43 =
      static_cast<uint8_t>(((beep_en ? 1 : 0) << 3) | ((band_stack_full ? 1 : 0) << 2) | gain_high);
  return {b42, b43};
}

uint8_t pack_ctrl7(uint8_t cw_ratio_raw, bool cw_mode_b, uint8_t cw_mlr) {
  const uint8_t ratio = static_cast<uint8_t>(cw_ratio_raw & 0xF);
  const uint8_t mlr = static_cast<uint8_t>(cw_mlr & 3);
  return static_cast<uint8_t>(((ratio << 4) | ((cw_mode_b ? 1 : 0) << 2) | mlr) & 0xFF);
}

std::pair<uint8_t, uint8_t>
pack_ctrl8(uint8_t swr_threshold_raw, uint8_t aux_out_vol, uint8_t aux_in_vol) {
  return {static_cast<uint8_t>(swr_threshold_raw & 0xF),
          static_cast<uint8_t>(((aux_out_vol & 0xF) << 4) | (aux_in_vol & 0xF))};
}

Result<HeadFrame> encode_head(const HeadEncodeRequest& req) {
  const Profile p = req.profile;

  const auto ma = mode_to_raw(p, req.vfo_a.mode);
  if (!ma.ok()) {
    return Result<HeadFrame>::Err(ma.err());
  }
  const auto mb = mode_to_raw(p, req.vfo_b.mode);
  if (!mb.ok()) {
    return Result<HeadFrame>::Err(mb.err());
  }

  const uint16_t stock_high = (p == Profile::V178) ? 2725 : 2700;
  constexpr uint16_t stock_low = 200;

  const auto filt_a = resolve_filter(p, req.vfo_a, stock_high, stock_low);
  if (!filt_a.ok()) {
    return Result<HeadFrame>::Err(filt_a.err());
  }

  // VFO-B filter inheritance: when B specifies no Hz or raw pair, copy A's
  // resolved filter edges before packing B's VFO block.
  HeadVfo vfo_b = req.vfo_b;
  const bool b_filter_unspecified = !vfo_b.filter_high_raw && !vfo_b.filter_low_raw &&
                                    !vfo_b.filter_high_hz && !vfo_b.filter_low_hz;
  if (b_filter_unspecified) {
    const auto hz = decode_filter(p, filt_a.value->high, filt_a.value->low);
    vfo_b.filter_high_hz = hz.high;
    vfo_b.filter_low_hz = hz.low;
  }

  const auto filt_b = resolve_filter(p, vfo_b, stock_high, stock_low);
  if (!filt_b.ok()) {
    return Result<HeadFrame>::Err(filt_b.err());
  }

  const uint8_t fft_a_in = req.vfo_a.fft_scale.value_or(1);
  const auto fs_a = validate_fft_scale(p, fft_a_in);
  if (!fs_a.ok()) {
    return Result<HeadFrame>::Err(fs_a.err());
  }
  // Inherit A's scale only when B's fft_scale is unset.
  const uint8_t fft_b_in = req.vfo_b.fft_scale.value_or(*fs_a.value);
  const auto fs_b = validate_fft_scale(p, fft_b_in);
  if (!fs_b.ok()) {
    return Result<HeadFrame>::Err(fs_b.err());
  }

  HeadFrame frame{};
  frame[0] = kSync0;
  frame[1] = kSync1;

  uint8_t pad2b = 0x55;
  uint8_t unknown2 = 2;
  const LabExtras* lab = req.lab ? &*req.lab : nullptr;
  if (lab) {
    if (lab->pad2b) {
      pad2b = *lab->pad2b;
    }
    if (lab->unknown2) {
      unknown2 = *lab->unknown2;
    }
  }

  // AGC: unset → 0 (golden bit-identity); clamp to 0–3 when set.
  const auto resolve_agc = [](const std::optional<uint8_t>& v) -> uint8_t {
    if (!v) {
      return 0;
    }
    return *v > 3 ? uint8_t{3} : *v;
  };
  const uint8_t agc_a = resolve_agc(req.vfo_a.agc);
  const uint8_t agc_b = resolve_agc(vfo_b.agc);

  pack_vfo(frame.data(),
           4,
           req.vfo_a.freq_hz,
           *ma.value,
           0,
           agc_a,
           filt_a.value->high,
           filt_a.value->low,
           *fs_a.value);
  pack_vfo(frame.data(),
           20,
           vfo_b.freq_hz,
           *mb.value,
           0,
           agc_b,
           filt_b.value->high,
           filt_b.value->low,
           *fs_b.value,
           pad2b);

  // Stock encode padding defaults (bytes 36–91) before HeadControls /
  // LabExtras overlays. Full offset table: ENCODE-PADDING.md.
  frame[38] = kHeadCtrl3CwQsk;
  frame[39] = 125;
  frame[42] = 0x78;
  frame[43] = 0x08;
  frame[44] = 1; // rf_power
  frame[45] = 0; // sql
  frame[46] = 0x33;
  frame[48] = req.volume;
  frame[49] = 10; // mic_gain
  frame[50] = 0x50;
  frame[51] = 2;
  frame[52] = unknown2;
  frame[53] = 6;
  std::memcpy(frame.data() + 56, kDefaultUnknown3, sizeof(kDefaultUnknown3));
  frame[60] = 10;
  frame[61] = 0x01;
  std::memcpy(frame.data() + 62, kDefaultUnknown4, sizeof(kDefaultUnknown4));
  frame[82] = 0x01;
  frame[83] = 0x98;
  std::memcpy(frame.data() + 86, kDefaultUnknown5, sizeof(kDefaultUnknown5));

  if (req.controls) {
    apply_head_controls(frame.data(), *req.controls);
  }

  // Lab TX-arm / operate policy. `allow_tx` gates all TX arms (PTT/ATU/VOX).
  if (lab && lab->clear_operate) {
    frame[kHeadPttByte] = static_cast<uint8_t>(frame[kHeadPttByte] & ~kHeadPttBit);
    frame[37] = static_cast<uint8_t>(frame[37] & ~kHeadCtrl2TuningBit);
    frame[38] = static_cast<uint8_t>(frame[38] & ~kHeadCtrl3Operate & ~kHeadCtrl3VoxEn);
  } else if (lab && lab->allow_tx) {
    frame[38] = static_cast<uint8_t>((frame[38] | kHeadCtrl3Operate) & ~kHeadCtrl3VoxEn);
    if (lab->arm_ptt) {
      arm_head_ptt(frame.data());
    } else if (lab->arm_atu_tune) {
      arm_head_atu_tune(frame.data());
    } else if (lab->arm_vox) {
      arm_head_vox(frame.data());
    }
  } else {
    // Arms without allow_tx are ignored (stock healthy RX stream).
    apply_stock_operate(frame.data());
  }

  const auto crc = stm32_crc32(std::span<const uint8_t>(frame.data(), 92));
  if (!crc.ok()) {
    return Result<HeadFrame>::Err(crc.err());
  }
  store_le32(frame.data() + 92, *crc.value);
  return Result<HeadFrame>::Ok(frame);
}

Result<DecodedHead> decode_head(Profile profile, const HeadFrame& frame) {
  if (frame[0] != kSync0 || frame[1] != kSync1) {
    return Result<DecodedHead>::Err(Error::BadSync);
  }

  const auto expected = stm32_crc32(std::span<const uint8_t>(frame.data(), 92));
  if (!expected.ok()) {
    return Result<DecodedHead>::Err(expected.err());
  }
  const uint32_t got_crc = load_le32(frame.data() + 92);

  DecodedHead out;
  out.crc_ok = (got_crc == *expected.value);
  out.freq_a_hz = load_le32(frame.data() + 4);
  out.filter_high_raw = frame[4 + 8];
  out.filter_low_raw = frame[4 + 9];
  out.fft_scale = static_cast<uint8_t>(frame[4 + 15] & 0x0F);
  {
    const auto m = mode_from_raw(profile, frame[4 + 5]);
    if (!m.ok()) {
      return Result<DecodedHead>::Err(m.err());
    }
    out.mode_a = *m.value;
  }
  {
    const auto hz = decode_filter(profile, out.filter_high_raw, out.filter_low_raw);
    out.filter_high_hz = hz.high;
    out.filter_low_hz = hz.low;
  }

  out.freq_b_hz = load_le32(frame.data() + 20);
  out.filter_high_raw_b = frame[20 + 8];
  out.filter_low_raw_b = frame[20 + 9];
  out.fft_scale_b = static_cast<uint8_t>(frame[20 + 15] & 0x0F);
  {
    const auto m = mode_from_raw(profile, frame[20 + 5]);
    if (!m.ok()) {
      return Result<DecodedHead>::Err(m.err());
    }
    out.mode_b = *m.value;
  }
  out.volume = frame[48];
  return Result<DecodedHead>::Ok(out);
}

Result<bool> head_frame_requests_tx(const HeadFrame& frame) {
  if (frame[0] != kSync0 || frame[1] != kSync1) {
    return Result<bool>::Err(Error::BadSync);
  }
  const auto expected = stm32_crc32(std::span<const uint8_t>(frame.data(), 92));
  if (!expected.ok()) {
    return Result<bool>::Err(expected.err());
  }
  const uint32_t got_crc = load_le32(frame.data() + 92);
  if (got_crc != *expected.value) {
    return Result<bool>::Err(Error::BadCrc);
  }
  const bool ptt = (frame[kHeadPttByte] & kHeadPttBit) != 0;
  const bool tune = (frame[37] & kHeadCtrl2TuningBit) != 0;
  const bool vox = (frame[38] & kHeadCtrl3VoxEn) != 0;
  return Result<bool>::Ok(ptt || tune || vox);
}

} // namespace g90
