#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <g90/error.hpp>
#include <g90/head.hpp>
#include <g90/types.hpp>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace g90 {

/// @brief Summary statistics over the 160-bin FFT block.
struct FftFingerprint {
  uint64_t sum = 0;       ///< Sum of all FFT bins.
  uint16_t max = 0;       ///< Peak bin magnitude.
  std::size_t argmax = 0; ///< Index of the peak bin (0…159).
};

/// @brief Parsed 48-byte status header from a body reply frame.
struct BodyStatus {
  /// S-meter from body byte 0. Stock GSOC/DispUnit force always-negative via
  /// `(byte | 0xffffff00)` / `~(~(byte<<24)>>24)` ≡ `byte - 256` (range
  /// `[-256, -1]`). Not a C `int8_t` cast — that only matches for `byte ≥ 0x80`.
  int s_meter = 0;
  uint8_t po = 0;                        ///< Power output indicator (byte 1).
  uint8_t swr = 0;                       ///< SWR raw value (byte 2).
  uint8_t voltage_raw = 0;               ///< Supply voltage ×0.1 V (byte 3).
  uint8_t alc = 0;                       ///< ALC level (byte 5).
  uint8_t byte4 = 0;                     ///< Raw status/flags byte (byte 4).
  uint8_t atu = 0;                       ///< ATU subfield (byte 4 bits 0–1).
  bool version_ready = false;            ///< Byte 4 bit 2.
  bool status_icon = false;              ///< Byte 4 bit 3.
  bool band_bypass = false;              ///< Byte 4 bit 4.
  bool cw_flag = false;                  ///< Byte 4 bit 5 — CW text may follow in header.
  uint8_t info6 = 0;                     ///< Byte 6.
  uint8_t info7 = 0;                     ///< Byte 7.
  bool link_flag = false;                ///< Word at byte 8, bit 30.
  bool rx_handshake = false;             ///< Word at byte 8, bit 31.
  uint8_t version = 0;                   ///< Firmware version byte (byte 12).
  std::array<uint8_t, 48> header{};      ///< Full 48-byte header copy.
  std::array<uint8_t, 35> header_tail{}; ///< Bytes 13–47 (raw status tail).

  /// @brief Supply voltage in volts.
  /// @return `voltage_raw / 10.0`.
  [[nodiscard]] double voltage_v() const noexcept { return voltage_raw / 10.0; }

  /// @brief Count non-zero bytes in the 13–47 tail region.
  /// @return Number of non-zero bytes in `header_tail`.
  [[nodiscard]] std::size_t header_nz_13_47() const;

  /// @brief NUL-terminated CW text from header bytes 20–47 when `cw_flag` is set.
  /// @return ASCII substring up to the first `0x00`.
  [[nodiscard]] std::string cw_text() const;
};

/// @brief Full decode of a 372-byte body reply frame.
struct DecodedBody {
  /// @brief STM32 CRC over bytes 0–367 matches bytes 368–371.
  /// @note Soft CRC: `decode_body` returns `Ok` even when @c false; callers may
  ///       still treat a mismatch as a hard error (e.g. `g90ctl decode body`).
  bool crc_ok = false;
  BodyStatus status;               ///< Parsed 48-byte header.
  std::array<uint16_t, 160> fft{}; ///< FFT bins (little-endian u16 @ offset 48).
  FftFingerprint fingerprint;      ///< Derived from `fft` via `compute_fft_fingerprint`.
};

/// @brief Parse the 48-byte status header without validating the full body CRC.
/// @param header48 Exactly 48 bytes from the start of a body frame.
/// @return Populated `BodyStatus` field map.
[[nodiscard]] BodyStatus parse_body_status(std::span<const uint8_t, 48> header48);

/// @brief Build a synthetic 372-byte body frame (test helper).
/// @param fft Optional FFT payload (max 320 bytes → 160 u16 bins); zero-filled if empty.
/// @param header Optional 48-byte header; zero-filled if empty. Must be exactly 48 bytes
///               when provided.
/// @return Body frame with STM32 CRC over bytes 0–367.
/// @return `Error::InvalidLength` when spans exceed wire limits.
[[nodiscard]] Result<BodyFrame> encode_body(std::span<const uint8_t> fft = {},
                                            std::span<const uint8_t> header = {});

/// @brief Decode a 372-byte body reply frame.
/// @param frame Full wire buffer.
/// @return Header, FFT bins, fingerprint, and `crc_ok` flag.
/// @note Soft CRC: returns `Ok` with `crc_ok=false` on mismatch.
[[nodiscard]] Result<DecodedBody> decode_body(const BodyFrame& frame);

/// @brief Compute sum / max / argmax over 160 FFT bins.
/// @param fft Exactly 160 little-endian magnitude values.
/// @return Fingerprint suitable for golden comparisons.
[[nodiscard]] FftFingerprint compute_fft_fingerprint(std::span<const uint16_t, 160> fft);

/// @brief Byte-wise diff of two body headers (up to 48 bytes).
/// @param a First header span.
/// @param b Second header span.
/// @return List of `(offset, old_byte, new_byte)` for each differing index.
[[nodiscard]] std::vector<std::tuple<std::size_t, uint8_t, uint8_t>>
header_diffs(std::span<const uint8_t> a, std::span<const uint8_t> b);

} // namespace g90
