#pragma once

#include <cstdint>
#include <g90/body.hpp>
#include <g90/head.hpp>
#include <g90/types.hpp>

namespace g90::defaults {

/// @brief Convenience preset: USB on the V179 codec at a given frequency.
///
/// Maps product firmware label **1.81** to `Profile::V179` with stock filter
/// 2700/200 Hz and FFT scale 1 — the typical on-air defaults for modern G90
/// units.
///
/// @param freq_hz VFO-A frequency in hertz (default 14.225 MHz).
/// @return Encoded 96-byte head frame, or an encode error from `encode_head`.
[[nodiscard]] Result<HeadFrame> encode_usb_v181(uint32_t freq_hz = 14'225'000);

} // namespace g90::defaults
