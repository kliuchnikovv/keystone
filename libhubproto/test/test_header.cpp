#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/message.hpp"
#include "hubproto/version.hpp"

using hubproto::Header;
using hubproto::MsgType;

TEST_CASE("header encodes to the exact little-endian byte layout (SPEC 4.2)") {
  Header h{};
  h.version = hubproto::kProtocolVersion;  // 0x01
  h.type = MsgType::Command;               // 0x20
  h.chip_id = 0x03;
  h.boot_id = 0x07;
  h.seq = 0x1234;
  h.entity_id = 0xBEEF;

  std::array<std::uint8_t, hubproto::kHeaderSize> buf{};
  hubproto::encode_header(h, buf.data());

  const std::array<std::uint8_t, 8> expected{
      0x01, 0x20, 0x03, 0x07, 0x34, 0x12, 0xEF, 0xBE};
  CHECK(buf == expected);
}

TEST_CASE("header round-trips through encode/decode") {
  Header in{};
  in.version = 0x01;
  in.type = MsgType::State;
  in.chip_id = 0x2A;
  in.boot_id = 0x99;
  in.seq = 0xABCD;
  in.entity_id = 0x0102;

  std::array<std::uint8_t, hubproto::kHeaderSize> buf{};
  hubproto::encode_header(in, buf.data());

  Header out{};
  REQUIRE(hubproto::decode_header(buf.data(), out));
  CHECK(out.version == in.version);
  CHECK(out.type == in.type);
  CHECK(out.chip_id == in.chip_id);
  CHECK(out.boot_id == in.boot_id);
  CHECK(out.seq == in.seq);
  CHECK(out.entity_id == in.entity_id);
}

TEST_CASE("decode_header rejects an unknown message type") {
  std::array<std::uint8_t, 8> buf{0x01, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  Header out{};
  CHECK_FALSE(hubproto::decode_header(buf.data(), out));
}

TEST_CASE("is_known_type accepts defined types and rejects others") {
  CHECK(hubproto::is_known_type(0x01));  // Hello
  CHECK(hubproto::is_known_type(0x40));  // Heartbeat
  CHECK(hubproto::is_known_type(0x04));  // Undeclare
  CHECK_FALSE(hubproto::is_known_type(0x05));
  CHECK_FALSE(hubproto::is_known_type(0x00));
  CHECK_FALSE(hubproto::is_known_type(0x11));
}
