/// Body frame encode/decode (372-byte reply). Status header is 48 B; FFT is
/// 160×uint16 LE starting at offset 48 (320 B); CRC covers bytes 0–367.
#include <algorithm>
#include <cstring>
#include <g90/body.hpp>
#include <g90/crc.hpp>

namespace g90 {
namespace {

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

[[nodiscard]] uint16_t load_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

constexpr std::size_t kFftOff = 48;
constexpr std::size_t kFftBytes = 320;

} // namespace

std::size_t BodyStatus::header_nz_13_47() const {
  std::size_t n = 0;
  for (uint8_t b : header_tail) {
    if (b != 0) {
      ++n;
    }
  }
  return n;
}

std::string BodyStatus::cw_text() const {
  // NUL-terminated ASCII from header bytes 20–47.
  std::string out;
  out.reserve(28);
  for (std::size_t i = 20; i < 48; ++i) {
    const uint8_t b = header[i];
    if (b == 0) {
      break;
    }
    out.push_back(static_cast<char>(b));
  }
  return out;
}

// B3 status byte map: 0=S-meter, 1=Po, 2=SWR, 3=voltage×0.1V, 4=flags/ATU,
// 5=ALC, 6–7=info, 8–11=link/handshake word, 12=version, 20–47=CW text tail.
BodyStatus parse_body_status(std::span<const uint8_t, 48> header48) {
  BodyStatus st;
  std::memcpy(st.header.data(), header48.data(), 48);
  std::memcpy(st.header_tail.data(), header48.data() + 13, 35);

  // Stock: always-negative sign-extend (GSOC unpack / DispUnit | 0xffffff00).
  st.s_meter = static_cast<int>(header48[0]) - 256;
  st.po = header48[1];
  st.swr = header48[2];
  st.voltage_raw = header48[3];
  st.byte4 = header48[4];
  st.atu = static_cast<uint8_t>(header48[4] & 0x3);
  st.version_ready = (header48[4] & 0x4) != 0;
  st.status_icon = (header48[4] & 0x8) != 0;
  st.band_bypass = (header48[4] & 0x10) != 0;
  st.cw_flag = (header48[4] & 0x20) != 0;
  st.alc = header48[5];
  st.info6 = header48[6];
  st.info7 = header48[7];
  const uint32_t w8 = load_le32(header48.data() + 8);
  st.link_flag = (w8 & 0x40000000u) != 0;
  st.rx_handshake = (w8 & 0x80000000u) != 0;
  st.version = header48[12];
  return st;
}

Result<BodyFrame> encode_body(std::span<const uint8_t> fft, std::span<const uint8_t> header) {
  if (fft.size() > kFftBytes) {
    return Result<BodyFrame>::Err(Error::InvalidLength);
  }
  if (!header.empty() && header.size() != kFftOff) {
    return Result<BodyFrame>::Err(Error::InvalidLength);
  }

  BodyFrame frame{};
  if (!header.empty()) {
    std::memcpy(frame.data(), header.data(), kFftOff);
  }
  if (!fft.empty()) {
    std::memcpy(frame.data() + kFftOff, fft.data(), fft.size());
  }

  const auto crc = stm32_crc32(std::span<const uint8_t>(frame.data(), 368));
  if (!crc.ok()) {
    return Result<BodyFrame>::Err(crc.err());
  }
  store_le32(frame.data() + 368, *crc.value);
  return Result<BodyFrame>::Ok(frame);
}

Result<DecodedBody> decode_body(const BodyFrame& frame) {
  const auto expected = stm32_crc32(std::span<const uint8_t>(frame.data(), 368));
  if (!expected.ok()) {
    return Result<DecodedBody>::Err(expected.err());
  }
  const uint32_t got = load_le32(frame.data() + 368);

  DecodedBody out;
  out.crc_ok = (got == *expected.value);

  std::array<uint8_t, 48> hdr{};
  std::memcpy(hdr.data(), frame.data(), 48);
  out.status = parse_body_status(std::span<const uint8_t, 48>(hdr));

  for (std::size_t i = 0; i < 160; ++i) {
    out.fft[i] = load_le16(frame.data() + kFftOff + i * 2);
  }
  out.fingerprint = compute_fft_fingerprint(out.fft);
  return Result<DecodedBody>::Ok(out);
}

FftFingerprint compute_fft_fingerprint(std::span<const uint16_t, 160> fft) {
  FftFingerprint fp;
  for (std::size_t i = 0; i < fft.size(); ++i) {
    fp.sum += fft[i];
    if (fft[i] > fp.max) {
      fp.max = fft[i];
      fp.argmax = i;
    }
  }
  return fp;
}

std::vector<std::tuple<std::size_t, uint8_t, uint8_t>> header_diffs(std::span<const uint8_t> a,
                                                                    std::span<const uint8_t> b) {
  const std::size_t n = std::min({a.size(), b.size(), kFftOff});
  std::vector<std::tuple<std::size_t, uint8_t, uint8_t>> out;
  for (std::size_t i = 0; i < n; ++i) {
    if (a[i] != b[i]) {
      out.emplace_back(i, a[i], b[i]);
    }
  }
  return out;
}

} // namespace g90
