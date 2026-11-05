#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/frame.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

using namespace hubproto;

TEST_CASE("Matter message types are known and sit at the documented codes") {
  CHECK(static_cast<std::uint8_t>(MsgType::MatterPair) == 0x60);
  CHECK(static_cast<std::uint8_t>(MsgType::MatterResult) == 0x61);
  CHECK(is_known_type(0x60));
  CHECK(is_known_type(0x61));
  CHECK_FALSE(is_known_type(0x6F));
}

TEST_CASE("MATTER_PAIR carries the kind and the bytes, and round-trips") {
  const std::array<std::uint8_t, 12> onboarding = {
      0x00, 0x01, 0xf1, 0xff, 0x00, 0x80, 0x00, 0x0f, 0xa1, 0xb2, 0xc3, 0xd4};
  std::array<std::uint8_t, 80> buf{};
  auto r = encode_matter_pair(kMatterKindManufacturer, onboarding.data(), onboarding.size(),
                               buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(r.len == kMatterPairHeader + onboarding.size());
  CHECK(buf[0] == kMatterKindManufacturer);
  CHECK(buf[1] == onboarding.size());

  MatterPairView view{};
  REQUIRE(decode_matter_pair(buf.data(), r.len, view));
  CHECK(view.kind == kMatterKindManufacturer);
  CHECK(view.len == onboarding.size());
  for (std::size_t i = 0; i < onboarding.size(); ++i) CHECK(view.bytes[i] == onboarding[i]);
}

TEST_CASE("MATTER_PAIR refuses what it cannot carry") {
  std::array<std::uint8_t, 80> buf{};
  const std::uint8_t one = 0x42;
  // No bytes at all: there is nothing to introduce.
  CHECK_FALSE(encode_matter_pair(kMatterKindQr, &one, 0, buf.data(), buf.size()).ok);
  // More than a frame can hold.
  std::array<std::uint8_t, kMatterMaxPayload + 1> big{};
  CHECK_FALSE(encode_matter_pair(kMatterKindQr, big.data(), big.size(), buf.data(), buf.size()).ok);
  // The room in the buffer.
  CHECK_FALSE(encode_matter_pair(kMatterKindQr, &one, 1, buf.data(), 2).ok);

  // A length that does not match the bytes.
  buf[0] = kMatterKindQr;
  buf[1] = 4;
  MatterPairView view{};
  CHECK_FALSE(decode_matter_pair(buf.data(), 5, view));  // too short
  CHECK_FALSE(decode_matter_pair(buf.data(), 7, view));  // too long
  buf[1] = 0;
  CHECK_FALSE(decode_matter_pair(buf.data(), 6, view));  // an empty payload
  // A kind nobody knows how to parse.
  buf[0] = kMatterKindQr + 1;
  buf[1] = 4;
  CHECK_FALSE(decode_matter_pair(buf.data(), 6, view));
}

TEST_CASE("MATTER_RESULT has the exact little-endian layout and round-trips") {
  MatterResult in{};
  in.state = MatterState::Done;
  in.code = MatterCode::Ok;
  in.entity_id = 0x0301;

  std::array<std::uint8_t, 8> buf{};
  auto r = encode_matter_result(in, buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(r.len == 4);
  CHECK(buf[0] == static_cast<std::uint8_t>(MatterState::Done));
  CHECK(buf[1] == static_cast<std::uint8_t>(MatterCode::Ok));
  CHECK(buf[2] == 0x01);
  CHECK(buf[3] == 0x03);

  MatterResult out{};
  REQUIRE(decode_matter_result(buf.data(), r.len, out));
  CHECK(out.state == MatterState::Done);
  CHECK(out.code == MatterCode::Ok);
  CHECK(out.entity_id == 0x0301);
  CHECK_FALSE(decode_matter_result(buf.data(), 3, out));
  CHECK_FALSE(decode_matter_result(buf.data(), 5, out));
  CHECK_FALSE(encode_matter_result(in, buf.data(), 3).ok);

  // A state or a code we do not know is not ours to interpret.
  buf[0] = 99;
  CHECK_FALSE(decode_matter_result(buf.data(), 4, out));
  buf[0] = static_cast<std::uint8_t>(MatterState::Done);
  buf[1] = 99;
  CHECK_FALSE(decode_matter_result(buf.data(), 4, out));
}

TEST_CASE("the Matter payload fits one frame") {
  CHECK(kMatterPairHeader + kMatterMaxPayload <= kMaxPayloadSize);
}
