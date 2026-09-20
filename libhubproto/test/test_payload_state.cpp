#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/payload.hpp"

using hubproto::Command;
using hubproto::CmdAckStatus;
using hubproto::StateEntry;
using hubproto::StateReader;
using hubproto::Value;
using hubproto::ValueType;

TEST_CASE("Value round-trips every value_type") {
  CHECK(Value::make_bool(true).as_bool() == true);
  CHECK(Value::make_bool(false).as_bool() == false);
  CHECK(Value::make_u8(0xC8).as_u8() == 0xC8);
  CHECK(Value::make_i16(-50).as_i16() == -50);
  CHECK(Value::make_i16(32000).as_i16() == 32000);
  CHECK(Value::make_u16(0xBEEF).as_u16() == 0xBEEF);
  CHECK(Value::make_f32(3.5f).as_f32() == doctest::Approx(3.5f));
  CHECK(Value::make_enum8(0x03).as_enum8() == 0x03);

  std::uint16_t x = 0, y = 0;
  Value::make_xy(0x1122, 0x3344).as_xy(x, y);
  CHECK(x == 0x1122);
  CHECK(y == 0x3344);
}

TEST_CASE("Value equality compares type, length and wire bytes") {
  CHECK(Value::make_u8(200) == Value::make_u8(200));
  CHECK(Value::make_u8(200) != Value::make_u8(201));
  CHECK(Value::make_bool(true) != Value::make_u8(1));  // different type
  CHECK(Value::make_i16(-50) == Value::make_i16(-50));

  // Bytes beyond `len` do not affect equality.
  Value a = Value::make_u8(7);
  Value b = Value::make_u8(7);
  b.raw[1] = 0xEE;  // junk past len (len==1)
  CHECK(a == b);
}

TEST_CASE("Value stores type and length") {
  Value v = Value::make_i16(-1);
  CHECK(v.type == ValueType::I16);
  CHECK(v.len == 2);
}

TEST_CASE("i16 value is stored little-endian on the wire") {
  Value v = Value::make_i16(-50);  // 0xFFCE
  CHECK(v.raw[0] == 0xCE);
  CHECK(v.raw[1] == 0xFF);
}

TEST_CASE("encode_command matches SPEC 5 layout") {
  Command c{0x1234, 0x02, Value::make_u8(0xC8)};
  std::array<std::uint8_t, 8> out{};
  auto r = hubproto::encode_command(c, out.data(), out.size());
  REQUIRE(r.ok);
  REQUIRE(r.len == 4);
  const std::array<std::uint8_t, 4> expected{0x34, 0x12, 0x02, 0xC8};
  for (std::size_t i = 0; i < 4; ++i) CHECK(out[i] == expected[i]);
}

TEST_CASE("command round-trips with caller-supplied value_type") {
  Command in{0x00AB, 0x40, Value::make_i16(2150)};  // setpoint 21.5C
  std::array<std::uint8_t, 8> buf{};
  auto r = hubproto::encode_command(in, buf.data(), buf.size());
  REQUIRE(r.ok);

  Command out{};
  REQUIRE(hubproto::decode_command(buf.data(), r.len, ValueType::I16, out));
  CHECK(out.entity_id == 0x00AB);
  CHECK(out.attr_id == 0x40);
  CHECK(out.value.as_i16() == 2150);
}

TEST_CASE("cmd_ack round-trips") {
  std::array<std::uint8_t, 2> buf{};
  auto r = hubproto::encode_cmd_ack(CmdAckStatus::Unreachable, buf.data(), buf.size());
  REQUIRE(r.ok);
  REQUIRE(r.len == 1);
  CHECK(buf[0] == 0x02);

  CmdAckStatus s{};
  REQUIRE(hubproto::decode_cmd_ack(buf.data(), r.len, s));
  CHECK(s == CmdAckStatus::Unreachable);
}

TEST_CASE("state encodes SPEC 5 layout and reads back via the pull cursor") {
  const std::array<StateEntry, 2> entries{{
      {0x10, Value::make_i16(-50)},  // temperature -5.0C
      {0x1F, Value::make_u8(95)},    // battery 95%
  }};
  std::array<std::uint8_t, 16> buf{};
  auto r = hubproto::encode_state(0x0007, entries.data(), 2, buf.data(), buf.size());
  REQUIRE(r.ok);
  const std::array<std::uint8_t, 8> expected{
      0x07, 0x00,        // entity_id LE
      0x02,              // attr_count
      0x10, 0xCE, 0xFF,  // temp: attr_id, i16(-50) LE
      0x1F, 0x5F,        // battery: attr_id, u8(95)
  };
  REQUIRE(r.len == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) CHECK(buf[i] == expected[i]);

  // Registry resolver stand-in.
  auto resolve = [](std::uint8_t attr_id) -> ValueType {
    return attr_id == 0x10 ? ValueType::I16 : ValueType::U8;
  };

  StateReader rd = hubproto::state_reader(buf.data(), r.len);
  REQUIRE(rd.valid);
  CHECK(rd.entity_id == 0x0007);

  std::uint8_t attr = 0;
  int seen = 0;
  while (hubproto::state_next_attr(rd, attr)) {
    Value v{};
    REQUIRE(hubproto::state_consume(rd, resolve(attr), v));
    if (seen == 0) {
      CHECK(attr == 0x10);
      CHECK(v.as_i16() == -50);
    } else {
      CHECK(attr == 0x1F);
      CHECK(v.as_u8() == 95);
    }
    ++seen;
  }
  CHECK(seen == 2);
  CHECK(rd.valid);
}

TEST_CASE("state_reader rejects a truncated header") {
  std::array<std::uint8_t, 2> buf{0x07, 0x00};  // missing attr_count
  StateReader rd = hubproto::state_reader(buf.data(), buf.size());
  CHECK_FALSE(rd.valid);
}

TEST_CASE("state consume flags truncation mid-value") {
  // count=1, attr 0x10 declared i16 but only 1 value byte present.
  std::array<std::uint8_t, 5> buf{0x07, 0x00, 0x01, 0x10, 0xCE};
  StateReader rd = hubproto::state_reader(buf.data(), buf.size());
  REQUIRE(rd.valid);
  std::uint8_t attr = 0;
  REQUIRE(hubproto::state_next_attr(rd, attr));
  Value v{};
  CHECK_FALSE(hubproto::state_consume(rd, ValueType::I16, v));
  CHECK_FALSE(rd.valid);
}
