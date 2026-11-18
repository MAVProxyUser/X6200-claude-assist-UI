#pragma once

#include <cstdint>
#include <g90/types.hpp>
#include <span>

namespace g90 {

/// @brief STM32 hardware CRC-32 used on G90 wire frames.
///
/// Processes little-endian 32-bit words from the wire buffer using CRC-32/MPEG-2
/// polynomial `0x04C11DB7` with per-word byte reversal (STM32 peripheral semantics).
/// Head frames CRC bytes 0–91; body frames CRC bytes 0–367.
///
/// @param data Byte span whose length must be a multiple of 4 (empty → init value).
/// @return Computed CRC-32 value.
/// @return `Error::InvalidLength` when `data.size() % 4 != 0`.
[[nodiscard]] Result<uint32_t> stm32_crc32(std::span<const uint8_t> data);

} // namespace g90
