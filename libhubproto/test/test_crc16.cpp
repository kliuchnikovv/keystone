#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "hubproto/crc16.hpp"

namespace {

std::uint16_t crc_of(const char* s) {
  return hubproto::crc16_ccitt(reinterpret_cast<const std::uint8_t*>(s),
                               std::strlen(s));
}

}  // namespace

TEST_CASE("crc16 canonical check value") {
  // The defining check for CRC-16/CCITT-FALSE.
  CHECK(crc_of("123456789") == 0x29B1);
}

TEST_CASE("crc16 of empty input is the init value") {
  CHECK(hubproto::crc16_ccitt(nullptr, 0) == 0xFFFF);
}

TEST_CASE("crc16 of a single zero byte") {
  const std::uint8_t z = 0x00;
  CHECK(hubproto::crc16_ccitt(&z, 1) == 0xE1F0);
}

TEST_CASE("crc16 detects a single-bit change") {
  const std::uint8_t a[] = {0xDE, 0xAD, 0xBE, 0xEF};
  std::uint8_t b[] = {0xDE, 0xAD, 0xBE, 0xEE};  // one bit flipped
  CHECK(hubproto::crc16_ccitt(a, 4) != hubproto::crc16_ccitt(b, 4));
}
