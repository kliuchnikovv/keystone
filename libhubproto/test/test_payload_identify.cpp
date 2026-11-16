#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

using namespace hubproto;

TEST_CASE("IDENTIFY sits at its documented code") {
  CHECK(static_cast<std::uint8_t>(MsgType::Identify) == 0x64);
  CHECK(is_known_type(0x64));
  CHECK_FALSE(is_known_type(0x65));
}

TEST_CASE("IDENTIFY carries the IdentifyTime and round-trips") {
  std::array<std::uint8_t, 8> buf{};
  for (const std::uint16_t seconds : {std::uint16_t{1}, std::uint16_t{10}, std::uint16_t{60},
                                      kIdentifyMaxSeconds}) {
    const EncodeResult r = encode_identify(seconds, buf.data(), buf.size());
    REQUIRE(r.ok);
    CHECK(r.len == kIdentifyPayloadSize);
    // Little-endian, as everything else on this link.
    CHECK(buf[0] == static_cast<std::uint8_t>(seconds & 0xFF));
    CHECK(buf[1] == static_cast<std::uint8_t>((seconds >> 8) & 0xFF));

    std::uint16_t back = 0;
    REQUIRE(decode_identify(buf.data(), r.len, back));
    CHECK(back == seconds);
  }
}

TEST_CASE("IDENTIFY refuses what it cannot carry") {
  std::array<std::uint8_t, 8> buf{};
  // Zero seconds is not an identify: a controller stops it with a new command.
  CHECK_FALSE(encode_identify(0, buf.data(), buf.size()).ok);
  // A buffer smaller than the payload.
  CHECK_FALSE(encode_identify(10, buf.data(), kIdentifyPayloadSize - 1).ok);
  CHECK_FALSE(encode_identify(10, nullptr, buf.size()).ok);

  std::uint16_t seconds = 0;
  CHECK_FALSE(decode_identify(nullptr, kIdentifyPayloadSize, seconds));
  // A bare command with no payload, and a payload of the wrong size, are both
  // refused rather than guessed at.
  CHECK_FALSE(decode_identify(buf.data(), 0, seconds));
  CHECK_FALSE(decode_identify(buf.data(), 1, seconds));
  CHECK_FALSE(decode_identify(buf.data(), 3, seconds));

  std::array<std::uint8_t, 2> zero{0, 0};
  CHECK_FALSE(decode_identify(zero.data(), zero.size(), seconds));
}
