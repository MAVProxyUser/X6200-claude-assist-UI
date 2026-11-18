/// STM32 CRC-32: CRC-32/MPEG-2 poly 0x04C11DB7, init 0xFFFFFFFF.
/// Each 4-byte little-endian wire word is byte-swapped before feeding the CRC.
#include <g90/crc.hpp>

namespace g90 {
namespace {

constexpr uint32_t kPoly = 0x04C11DB7;

void mpeg2_feed_byte(uint32_t& crc, uint8_t byte) {
  crc ^= static_cast<uint32_t>(byte) << 24;
  for (int i = 0; i < 8; ++i) {
    if (crc & 0x80000000u) {
      crc = ((crc << 1) & 0xFFFFFFFFu) ^ kPoly;
    } else {
      crc = (crc << 1) & 0xFFFFFFFFu;
    }
  }
}

} // namespace

Result<uint32_t> stm32_crc32(std::span<const uint8_t> data) {
  if (data.size() % 4 != 0) {
    return Result<uint32_t>::Err(Error::InvalidLength);
  }
  uint32_t crc = 0xFFFFFFFF;
  for (std::size_t i = 0; i < data.size(); i += 4) {
    mpeg2_feed_byte(crc, data[i + 3]);
    mpeg2_feed_byte(crc, data[i + 2]);
    mpeg2_feed_byte(crc, data[i + 1]);
    mpeg2_feed_byte(crc, data[i + 0]);
  }
  return Result<uint32_t>::Ok(crc);
}

} // namespace g90
