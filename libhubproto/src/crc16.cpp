#include "hubproto/crc16.hpp"

namespace hubproto {

std::uint16_t crc16_ccitt(const std::uint8_t* data, std::size_t len) noexcept {
  std::uint16_t crc = 0xFFFF;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[i]) << 8);
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 0x8000) {
        crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021);
      } else {
        crc = static_cast<std::uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

}  // namespace hubproto
