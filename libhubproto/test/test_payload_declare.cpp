#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "hubproto/payload.hpp"

using hubproto::AttrDecl;
using hubproto::DeclareView;
using hubproto::DeviceType;
using hubproto::ValueType;

TEST_CASE("value_size matches SPEC 6.2") {
  CHECK(hubproto::value_size(ValueType::Bool) == 1);
  CHECK(hubproto::value_size(ValueType::U8) == 1);
  CHECK(hubproto::value_size(ValueType::I16) == 2);
  CHECK(hubproto::value_size(ValueType::U16) == 2);
  CHECK(hubproto::value_size(ValueType::F32) == 4);
  CHECK(hubproto::value_size(ValueType::XyColor) == 4);
  CHECK(hubproto::value_size(ValueType::Enum8) == 1);
  CHECK(hubproto::value_size(static_cast<ValueType>(0x00)) == 0);
  CHECK(hubproto::value_size(static_cast<ValueType>(0xFF)) == 0);
}

TEST_CASE("encode_declare produces the SPEC 5 byte layout") {
  // A dimmable light: on_off(bool) + brightness(u8), both R/W/reportable.
  const std::array<AttrDecl, 2> attrs{{
      {0x01, ValueType::Bool, 0x07},
      {0x02, ValueType::U8, 0x07},
  }};
  std::array<std::uint8_t, 32> out{};
  auto r = hubproto::encode_declare(0x1234, DeviceType::Light, attrs.data(),
                                    2, out.data(), out.size());
  REQUIRE(r.ok);
  REQUIRE(r.len == 4 + 3 * 2);

  const std::array<std::uint8_t, 10> expected{
      0x34, 0x12,        // entity_id LE
      0x01,              // device_type = Light
      0x02,              // attr_count
      0x01, 0x01, 0x07,  // attr on_off: id, bool, flags
      0x02, 0x02, 0x07,  // attr brightness: id, u8, flags
  };
  for (std::size_t i = 0; i < expected.size(); ++i) CHECK(out[i] == expected[i]);
}

TEST_CASE("decode_declare round-trips") {
  const std::array<AttrDecl, 3> attrs{{
      {0x10, ValueType::I16, hubproto::kFlagReadable | hubproto::kFlagReportable},
      {0x11, ValueType::U16, hubproto::kFlagReadable},
      {0x1F, ValueType::U8, hubproto::kFlagReadable},
  }};
  std::array<std::uint8_t, 32> buf{};
  auto r = hubproto::encode_declare(0x0007, DeviceType::Sensor, attrs.data(),
                                    3, buf.data(), buf.size());
  REQUIRE(r.ok);

  DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  CHECK(v.entity_id == 0x0007);
  CHECK(v.device_type == DeviceType::Sensor);
  REQUIRE(v.attr_count == 3);
  for (std::uint8_t i = 0; i < 3; ++i) {
    AttrDecl a = hubproto::declare_attr_at(v, i);
    CHECK(a.attr_id == attrs[i].attr_id);
    CHECK(a.value_type == attrs[i].value_type);
    CHECK(a.flags == attrs[i].flags);
  }
}

TEST_CASE("encode_declare reports overflow") {
  const std::array<AttrDecl, 1> attrs{{{0x01, ValueType::Bool, 0x01}}};
  std::array<std::uint8_t, 4> tiny{};  // needs 7
  auto r = hubproto::encode_declare(0x0001, DeviceType::Switch, attrs.data(),
                                    1, tiny.data(), tiny.size());
  CHECK_FALSE(r.ok);
}

TEST_CASE("decode_declare rejects a truncated payload") {
  std::array<std::uint8_t, 5> buf{0x01, 0x00, 0x01, 0x02, 0x01};  // says 2 attrs
  DeclareView v{};
  CHECK_FALSE(hubproto::decode_declare(buf.data(), buf.size(), v));
}

TEST_CASE("decode_declare rejects an unknown value_type") {
  // entity 1, Light, 1 attr with value_type 0x00 (invalid).
  std::array<std::uint8_t, 7> buf{0x01, 0x00, 0x01, 0x01, 0x01, 0x00, 0x07};
  DeclareView v{};
  CHECK_FALSE(hubproto::decode_declare(buf.data(), buf.size(), v));
}
