#include <doctest/doctest.h>

#include <vector>

#include "core/zcl_profile.hpp"

using namespace core::zcl;
using hubproto::DeviceType;
namespace A = hubproto::attr;

namespace {
Profile classify_in(std::vector<std::uint16_t> in, std::vector<std::uint16_t> out = {}) {
  return classify(in.data(), in.size(), out.data(), out.size());
}
std::vector<std::uint8_t> ids(const Profile& p) {
  hubproto::AttrDecl a[16];
  const std::size_t n = attrs_for(p.caps, a, 16);
  std::vector<std::uint8_t> v;
  for (std::size_t i = 0; i < n; ++i) v.push_back(a[i].attr_id);
  return v;
}
}  // namespace

TEST_CASE("IKEA plug stays a switch with on_off, power_w, energy_wh (no battery)") {
  // Basic, PowerCfg, Identify, Groups, Scenes, OnOff, ElecMeas, Metering
  const auto p = classify_in({0x0000, 0x0001, 0x0003, 0x0004, 0x0005, 0x0006, 0x0B04, 0x0702});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Switch);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kOnOff, A::kPowerW, A::kEnergyWh});
}

TEST_CASE("a bare on/off endpoint is a switch with one attribute") {
  const auto p = classify_in({0x0000, 0x0006});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Switch);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kOnOff});
}

TEST_CASE("lights: dimmable and colour") {
  auto p = classify_in({0x0000, 0x0006, 0x0008});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Light);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kOnOff, A::kBrightness});
  CHECK_FALSE(p.color_caps_pending);

  p = classify_in({0x0000, 0x0006, 0x0008, 0x0300});
  CHECK(p.type == DeviceType::Light);
  CHECK(p.color_caps_pending);
  CHECK(ids(p) ==
        std::vector<std::uint8_t>{A::kOnOff, A::kBrightness, A::kColorTemp, A::kColorXy});

  // The lamp says it only does colour temperature: xy goes away.
  p.caps = refine_color_caps(p.caps, 0x0020);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kOnOff, A::kBrightness, A::kColorTemp});
  p.caps = refine_color_caps(p.caps, 0x0010);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kOnOff, A::kBrightness, A::kColorXy});
  p.caps = refine_color_caps(p.caps, 0x0000);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kOnOff, A::kBrightness});
}

TEST_CASE("a lamp's Power Configuration does not declare a battery") {
  const auto p = classify_in({0x0000, 0x0001, 0x0006, 0x0008});
  CHECK(p.type == DeviceType::Light);
  CHECK((p.caps & kCapBattery) == 0);
}

TEST_CASE("temperature / humidity / battery sensor") {
  const auto p = classify_in({0x0000, 0x0001, 0x0402, 0x0405});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Sensor);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kTemperature, A::kHumidity, A::kBattery});
}

TEST_CASE("illuminance-only endpoint is a sensor") {
  const auto p = classify_in({0x0400});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Sensor);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kIlluminance});
}

TEST_CASE("occupancy sensor keeps its extra readings") {
  const auto p = classify_in({0x0000, 0x0001, 0x0406, 0x0400, 0x0402});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Occupancy);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kTemperature, A::kIlluminance, A::kBattery,
                                            A::kOccupied});
}

TEST_CASE("thermostat: temperature, setpoint, mode") {
  const auto p = classify_in({0x0000, 0x0001, 0x0201});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Climate);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kTemperature, A::kBattery, A::kSetpoint, A::kMode});
  hubproto::AttrDecl a[8];
  const std::size_t n = attrs_for(p.caps, a, 8);
  for (std::size_t i = 0; i < n; ++i) {
    if (a[i].attr_id == A::kTemperature) CHECK((a[i].flags & hubproto::kFlagWritable) == 0);
    if (a[i].attr_id == A::kSetpoint) CHECK((a[i].flags & hubproto::kFlagWritable) != 0);
  }
}

TEST_CASE("IAS zone: unresolved until the zone type is known") {
  auto p = classify_in({0x0000, 0x0001, 0x0500});
  CHECK_FALSE(p.valid);
  CHECK(p.ias);

  auto contact = p;
  REQUIRE(resolve_ias(0x0015, contact));
  CHECK(contact.type == DeviceType::Contact);
  CHECK(ids(contact) == std::vector<std::uint8_t>{A::kBattery, A::kContact});

  auto motion = p;
  REQUIRE(resolve_ias(0x000D, motion));
  CHECK(motion.type == DeviceType::Occupancy);
  CHECK(ids(motion) == std::vector<std::uint8_t>{A::kBattery, A::kOccupied});

  auto smoke = p;
  CHECK_FALSE(resolve_ias(0x0028, smoke));  // fire sensor: not modelled
  CHECK_FALSE(smoke.valid);
}

TEST_CASE("remote controls become buttons; a plug that also sends is still a switch") {
  auto p = classify_in({0x0000, 0x0001, 0x0003}, {0x0006, 0x0008, 0x0005});
  REQUIRE(p.valid);
  CHECK(p.type == DeviceType::Button);
  CHECK(ids(p) == std::vector<std::uint8_t>{A::kBattery, A::kEvent});

  p = classify_in({0x0000, 0x0006}, {0x0006});
  CHECK(p.type == DeviceType::Switch);

  p = classify_in({0x0000, 0x0012});  // multistate input (Aqara)
  CHECK(p.type == DeviceType::Button);
}

TEST_CASE("an endpoint with nothing we model is not valid") {
  CHECK_FALSE(classify_in({0x0000, 0x0003}).valid);
  CHECK_FALSE(classify_in({0x0001}).valid);
  CHECK_FALSE(classify_in({}).valid);
}

TEST_CASE("attrs_for never writes past the buffer") {
  hubproto::AttrDecl a[3];
  CHECK(attrs_for(0xFFFFFFFFu, a, 3) == 3);
  CHECK(attrs_for(0, a, 3) == 0);
  // Everything at once still fits the registry's per-entity limit of 8 for real profiles:
  // a colour light with power metering is the widest we classify.
  const auto p = classify_in({0x0006, 0x0008, 0x0300, 0x0B04, 0x0702});
  CHECK(ids(p).size() <= 8);
}

TEST_CASE("button commands map to events") {
  using E = A::ButtonEvent;
  CHECK(button_event(cluster::kOnOff, 0x02, 0) == E::Press);
  CHECK(button_event(cluster::kOnOff, 0x01, 0) == E::On);
  CHECK(button_event(cluster::kOnOff, 0x00, 0) == E::Off);
  CHECK(button_event(cluster::kOnOff, 0x40, 0) == E::Idle);
  CHECK(button_event(cluster::kLevel, 0x05, 0) == E::Up);
  CHECK(button_event(cluster::kLevel, 0x01, 1) == E::Down);
  CHECK(button_event(cluster::kLevel, 0x02, 0) == E::Up);
  CHECK(button_event(cluster::kLevel, 0x07, 0) == E::Release);
  CHECK(button_event(cluster::kLevel, 0x00, 0) == E::Idle);
  CHECK(button_event(cluster::kColor, 0x01, 0) == E::Idle);
  CHECK(multistate_event(1) == E::Press);
  CHECK(multistate_event(2) == E::Double);
  CHECK(multistate_event(16) == E::Hold);
  CHECK(multistate_event(17) == E::Release);
  CHECK(multistate_event(99) == E::Idle);
}

TEST_CASE("attr_value_type resolves what a COMMAND for each attribute carries") {
  hubproto::ValueType vt{};
  REQUIRE(attr_value_type(A::kOnOff, vt));
  CHECK(vt == hubproto::ValueType::Bool);
  REQUIRE(attr_value_type(A::kBrightness, vt));
  CHECK(vt == hubproto::ValueType::U8);
  REQUIRE(attr_value_type(A::kColorTemp, vt));
  CHECK(vt == hubproto::ValueType::U16);
  REQUIRE(attr_value_type(A::kColorXy, vt));
  CHECK(vt == hubproto::ValueType::XyColor);
  REQUIRE(attr_value_type(A::kSetpoint, vt));
  CHECK(vt == hubproto::ValueType::I16);
  REQUIRE(attr_value_type(A::kMode, vt));
  CHECK(vt == hubproto::ValueType::Enum8);
  CHECK_FALSE(attr_value_type(0x77, vt));
}
