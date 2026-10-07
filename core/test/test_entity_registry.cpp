#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>

#include "core/entity_registry.hpp"
#include "hubproto/payload.hpp"

using hubproto::AttrDecl;
using hubproto::DeclareView;
using hubproto::DeviceType;
using hubproto::ValueType;

namespace {

// Encode a DECLARE, decode it into a view, and keep the backing bytes alive.
struct DeclareBytes {
  std::array<std::uint8_t, 64> buf{};
  DeclareView view{};

  DeclareBytes(std::uint16_t entity_id, DeviceType dt,
               const AttrDecl* attrs, std::uint8_t count) {
    auto r = hubproto::encode_declare(entity_id, dt, attrs, count, buf.data(),
                                      buf.size());
    REQUIRE(r.ok);
    REQUIRE(hubproto::decode_declare(buf.data(), r.len, view));
  }
};

const std::array<AttrDecl, 2> kLightAttrs{{
    {0x01, ValueType::Bool, 0x07},
    {0x02, ValueType::U8, 0x07},
}};
const std::array<AttrDecl, 1> kSwitchAttrs{{{0x01, ValueType::Bool, 0x07}}};

}  // namespace

TEST_CASE("declare adds an entity that can be found") {
  core::EntityRegistry<> reg;
  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);

  REQUIRE(reg.declare(0x01, light.view));
  CHECK(reg.size() == 1);

  const auto* e = reg.find(0x0010);
  REQUIRE(e != nullptr);
  CHECK(e->chip_id == 0x01);
  CHECK(e->device_type == DeviceType::Light);
  REQUIRE(e->attr_count == 2);
  CHECK(e->attrs[0].attr_id == 0x01);
  CHECK(e->attrs[0].value_type == ValueType::Bool);
  CHECK(e->attrs[1].attr_id == 0x02);
  CHECK(e->attrs[1].has_value == false);
}

TEST_CASE("two entities from the same chip coexist") {
  core::EntityRegistry<> reg;
  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  DeclareBytes sw(0x0020, DeviceType::Switch, kSwitchAttrs.data(), 1);

  REQUIRE(reg.declare(0x01, light.view));
  REQUIRE(reg.declare(0x01, sw.view));
  CHECK(reg.size() == 2);
  CHECK(reg.find(0x0010)->device_type == DeviceType::Light);
  CHECK(reg.find(0x0020)->device_type == DeviceType::Switch);
}

TEST_CASE("re-declaring the same entity_id replaces it, not duplicates") {
  core::EntityRegistry<> reg;
  DeclareBytes as_light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  DeclareBytes as_switch(0x0010, DeviceType::Switch, kSwitchAttrs.data(), 1);

  REQUIRE(reg.declare(0x01, as_light.view));
  REQUIRE(reg.declare(0x01, as_switch.view));
  CHECK(reg.size() == 1);
  const auto* e = reg.find(0x0010);
  REQUIRE(e != nullptr);
  CHECK(e->device_type == DeviceType::Switch);
  CHECK(e->attr_count == 1);
}

TEST_CASE("entity_at enumerates live entities and bounds-checks") {
  core::EntityRegistry<> reg;
  CHECK(reg.entity_at(0) == nullptr);  // empty

  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  DeclareBytes sw(0x0020, DeviceType::Switch, kSwitchAttrs.data(), 1);
  DeclareBytes sw2(0x0030, DeviceType::Switch, kSwitchAttrs.data(), 1);
  REQUIRE(reg.declare(0x01, light.view));
  REQUIRE(reg.declare(0x01, sw.view));
  REQUIRE(reg.declare(0x02, sw2.view));

  // Every live entity is reachable exactly once across [0, size()).
  bool seen[3] = {false, false, false};
  for (std::size_t i = 0; i < reg.size(); ++i) {
    const auto* e = reg.entity_at(i);
    REQUIRE(e != nullptr);
    if (e->entity_id == 0x0010) seen[0] = true;
    else if (e->entity_id == 0x0020) seen[1] = true;
    else if (e->entity_id == 0x0030) seen[2] = true;
  }
  CHECK((seen[0] && seen[1] && seen[2]));
  CHECK(reg.entity_at(reg.size()) == nullptr);  // out of range

  // After clearing chip 0x01, only the chip 0x02 entity remains at index 0.
  reg.clear_chip(0x01);
  REQUIRE(reg.size() == 1);
  REQUIRE(reg.entity_at(0) != nullptr);
  CHECK(reg.entity_at(0)->entity_id == 0x0030);
  CHECK(reg.entity_at(1) == nullptr);
}

TEST_CASE("value_type_of resolves a declared attribute") {
  core::EntityRegistry<> reg;
  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  REQUIRE(reg.declare(0x01, light.view));

  ValueType vt{};
  REQUIRE(reg.value_type_of(0x0010, 0x02, vt));
  CHECK(vt == ValueType::U8);
  CHECK_FALSE(reg.value_type_of(0x0010, 0x99, vt));  // unknown attr
  CHECK_FALSE(reg.value_type_of(0x9999, 0x01, vt));  // unknown entity
}

TEST_CASE("set_value / get_value store and return the latest state") {
  core::EntityRegistry<> reg;
  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  REQUIRE(reg.declare(0x01, light.view));

  hubproto::Value out{};
  CHECK_FALSE(reg.get_value(0x0010, 0x02, out));  // nothing reported yet

  REQUIRE(reg.set_value(0x0010, 0x02, hubproto::Value::make_u8(200)));
  REQUIRE(reg.get_value(0x0010, 0x02, out));
  CHECK(out.as_u8() == 200);
  CHECK(reg.find(0x0010)->attrs[1].has_value);
}

TEST_CASE("set_value rejects a type mismatch and unknown targets") {
  core::EntityRegistry<> reg;
  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  REQUIRE(reg.declare(0x01, light.view));

  CHECK_FALSE(reg.set_value(0x0010, 0x02, hubproto::Value::make_bool(true)));  // declared u8
  CHECK_FALSE(reg.set_value(0x0010, 0x99, hubproto::Value::make_u8(1)));       // unknown attr
  CHECK_FALSE(reg.set_value(0x9999, 0x01, hubproto::Value::make_bool(true)));  // unknown entity
}

TEST_CASE("clear_chip removes only that chip's entities") {
  core::EntityRegistry<> reg;
  DeclareBytes a(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  DeclareBytes b(0x0020, DeviceType::Switch, kSwitchAttrs.data(), 1);
  DeclareBytes c(0x0030, DeviceType::Switch, kSwitchAttrs.data(), 1);
  REQUIRE(reg.declare(0x01, a.view));
  REQUIRE(reg.declare(0x01, b.view));
  REQUIRE(reg.declare(0x02, c.view));

  CHECK(reg.clear_chip(0x01) == 2);
  CHECK(reg.size() == 1);
  CHECK(reg.find(0x0010) == nullptr);
  CHECK(reg.find(0x0030) != nullptr);  // chip 0x02 untouched
}

TEST_CASE("declare fails when the table is full") {
  core::EntityRegistry<2, 8> reg;  // capacity 2
  DeclareBytes a(0x0001, DeviceType::Switch, kSwitchAttrs.data(), 1);
  DeclareBytes b(0x0002, DeviceType::Switch, kSwitchAttrs.data(), 1);
  DeclareBytes c(0x0003, DeviceType::Switch, kSwitchAttrs.data(), 1);
  REQUIRE(reg.declare(0x01, a.view));
  REQUIRE(reg.declare(0x01, b.view));
  CHECK_FALSE(reg.declare(0x01, c.view));
  CHECK(reg.size() == 2);
}

TEST_CASE("declare fails when an entity has more attrs than capacity") {
  core::EntityRegistry<8, 1> reg;  // one attr max
  DeclareBytes light(0x0010, DeviceType::Light, kLightAttrs.data(), 2);
  CHECK_FALSE(reg.declare(0x01, light.view));
  CHECK(reg.size() == 0);
}

TEST_CASE("EntityRegistry naming, resolution and persistence across clear_chip") {
  core::EntityRegistry<> reg;
  DeclareBytes plug(0x0100, DeviceType::Switch, kSwitchAttrs.data(), 1);
  REQUIRE(reg.declare(0x01, plug.view));

  CHECK(reg.name_of(0x0100) == nullptr);
  CHECK(reg.set_name(0x0100, "livingroom_plug"));
  CHECK(std::string(reg.name_of(0x0100)) == "livingroom_plug");

  // find_by_name
  const auto* found = reg.find_by_name("livingroom_plug");
  REQUIRE(found != nullptr);
  CHECK(found->entity_id == 0x0100);
  CHECK(reg.find_by_name("nonexistent") == nullptr);

  // resolve
  std::uint16_t resolved_id = 0;
  CHECK(reg.resolve("livingroom_plug", resolved_id));
  CHECK(resolved_id == 0x0100);
  CHECK(reg.resolve("0x0100", resolved_id));
  CHECK(resolved_id == 0x0100);

  // clear_chip should NOT delete the user-assigned name
  reg.clear_chip(0x01);
  CHECK(reg.size() == 0);
  CHECK(std::string(reg.name_of(0x0100)) == "livingroom_plug");

  // re-declaring the entity allows find_by_name immediately
  REQUIRE(reg.declare(0x01, plug.view));
  CHECK(reg.find_by_name("livingroom_plug") != nullptr);

  // clear_name removes it
  CHECK(reg.clear_name(0x0100));
  CHECK(reg.name_of(0x0100) == nullptr);
}
