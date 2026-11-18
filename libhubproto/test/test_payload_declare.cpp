#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
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
  CHECK(hubproto::value_size(ValueType::U32) == 4);
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
                                    2, nullptr, out.data(), out.size());
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
                                    3, nullptr, buf.data(), buf.size());
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
                                    1, nullptr, tiny.data(), tiny.size());
  CHECK_FALSE(r.ok);
}

TEST_CASE("a DECLARE carries the entity's name") {
  const std::array<AttrDecl, 1> attrs{{{0x1F, ValueType::U8, hubproto::kFlagReadable}}};
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(), 1,
                                    "kitchen sensor", buf.data(), buf.size());
  REQUIRE(r.ok);
  REQUIRE(r.len == hubproto::declare_size_with_name(1, 14));

  DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  CHECK(v.entity_id == 0x0300);
  REQUIRE(v.name != nullptr);
  CHECK(v.name_len == 14);
  CHECK(std::string(v.name, v.name_len) == "kitchen sensor");

  // No name: the DECLARE is the old shape and the reader sees no name.
  auto r2 = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(), 1,
                                     nullptr, buf.data(), buf.size());
  REQUIRE(r2.ok);
  CHECK(r2.len == hubproto::declare_size(1));
  DeclareView v2{};
  REQUIRE(hubproto::decode_declare(buf.data(), r2.len, v2));
  CHECK(v2.name == nullptr);
  CHECK(v2.name_len == 0);

  // An empty name is the same as no name.
  auto r3 = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(), 1,
                                     "", buf.data(), buf.size());
  REQUIRE(r3.ok);
  CHECK(r3.len == hubproto::declare_size(1));
}

TEST_CASE("an old reader ignores the name, and a new one reads an old DECLARE") {
  // The name is a tail: an older peer decodes the DECLARE as before.
  const std::array<AttrDecl, 1> attrs{{{0x1F, ValueType::U8, hubproto::kFlagReadable}}};
  std::array<std::uint8_t, 64> buf{};
  auto r = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(), 1,
                                    "lamp", buf.data(), buf.size());
  REQUIRE(r.ok);

  // A reader that stops after the attributes sees the old layout.
  CHECK(r.len > hubproto::declare_size(1));
  CHECK(buf[hubproto::declare_size(1)] == 4);  // the name's length byte

  // A new reader of an old DECLARE (no tail) finds no name — and still works.
  std::array<std::uint8_t, 8> old{};
  auto r2 = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(), 1,
                                     nullptr, old.data(), old.size());
  REQUIRE(r2.ok);
  DeclareView v{};
  REQUIRE(hubproto::decode_declare(old.data(), r2.len, v));
  CHECK(v.name == nullptr);
  CHECK(v.attr_count == 1);
}

TEST_CASE("a name longer than the limit is cut") {
  const std::array<AttrDecl, 1> attrs{{{0x1F, ValueType::U8, hubproto::kFlagReadable}}};
  std::array<std::uint8_t, 128> buf{};
  const std::string long_name(60, 'x');
  auto r = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(), 1,
                                    long_name.c_str(), buf.data(), buf.size());
  REQUIRE(r.ok);
  DeclareView v{};
  REQUIRE(hubproto::decode_declare(buf.data(), r.len, v));
  REQUIRE(v.name != nullptr);
  CHECK(v.name_len == hubproto::kDeclareNameMax);
}

TEST_CASE("declare_size is the buffer a DECLARE needs") {
  // A caller that sizes its own buffer must use this number: one byte short
  // and the encoder refuses the buffer — and the DECLARE is lost without a
  // word (it happened in the firmware, with eight attributes).
  CHECK(hubproto::declare_size(0) == 4);
  CHECK(hubproto::declare_size(1) == 7);
  CHECK(hubproto::declare_size(8) == 28);

  std::array<AttrDecl, 8> attrs{};
  for (std::size_t i = 0; i < attrs.size(); ++i) {
    attrs[i] = AttrDecl{static_cast<std::uint8_t>(0x10 + i), ValueType::U8, 0x01};
  }
  std::array<std::uint8_t, hubproto::declare_size(8)> exact{};
  auto r = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(),
                                    8, nullptr, exact.data(), exact.size());
  REQUIRE(r.ok);
  REQUIRE(r.len == hubproto::declare_size(8));

  // One byte less: the encoder has to refuse it.
  std::array<std::uint8_t, hubproto::declare_size(8) - 1> shortbuf{};
  auto r2 = hubproto::encode_declare(0x0300, DeviceType::Sensor, attrs.data(),
                                     8, nullptr, shortbuf.data(), shortbuf.size());
  CHECK_FALSE(r2.ok);

  DeclareView v{};
  REQUIRE(hubproto::decode_declare(exact.data(), r.len, v));
  CHECK(v.entity_id == 0x0300);
  CHECK(v.attr_count == 8);
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
