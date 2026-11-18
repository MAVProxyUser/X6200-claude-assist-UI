#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace g90 {

/// @brief Library-wide error codes for codec and transport operations.
enum class Error {
  /// Buffer or span length does not match the wire format.
  InvalidLength,
  /// Head frame sync bytes are not `0x55 0xAA`.
  BadSync,
  /// CRC mismatch on a frame that requires hard validation (e.g. TX policy).
  BadCrc,
  /// Mode string or raw mode byte is invalid for the active profile.
  InvalidMode,
  /// Filter Hz/raw pair is invalid or only one half of a raw pair was supplied.
  InvalidFilter,
  /// FFT scale is out of range for the profile (V179 allows only `1`).
  InvalidFftScale,
  /// Profile string could not be parsed (`parse_profile`).
  InvalidProfile,
  /// Read timed out waiting for wire bytes (serial I/O).
  Timeout,
  /// Fewer bytes read than requested (serial I/O).
  ShortRead,
  /// Underlying I/O failure (serial I/O).
  IoError,
  /// API stub or feature not yet implemented (serial transport).
  NotImplemented,
};

/// @brief Stable short name for an `Error` code (for logging and CLI output).
/// @param e Error to stringify.
/// @return Lowercase identifier, e.g. `"bad_crc"`.
[[nodiscard]] std::string_view to_string(Error e) noexcept;

} // namespace g90
