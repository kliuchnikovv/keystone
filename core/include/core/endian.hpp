// Endian serialization helpers for little-endian wire formats.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {
namespace detail {

inline void put_u16_le(std::uint8_t* p, std::uint16_t val) noexcept {
  p[0] = static_cast<std::uint8_t>(val & 0xFF);
  p[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
}

inline std::uint16_t get_u16_le(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(p[0]) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

inline void put_u32_le(std::uint8_t* p, std::uint32_t val) noexcept {
  p[0] = static_cast<std::uint8_t>(val & 0xFF);
  p[1] = static_cast<std::uint8_t>((val >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((val >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((val >> 24) & 0xFF);
}

inline std::uint32_t get_u32_le(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void put_u64_le(std::uint8_t* p, std::uint64_t val) noexcept {
  for (std::size_t i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>((val >> (i * 8)) & 0xFF);
  }
}

inline std::uint64_t get_u64_le(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    v |= (static_cast<std::uint64_t>(p[i]) << (i * 8));
  }
  return v;
}

}  // namespace detail
}  // namespace core
