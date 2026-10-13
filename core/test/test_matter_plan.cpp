#include <doctest/doctest.h>

#include <initializer_list>

#include "core/matter_plan.hpp"

using namespace core::matter;
using hubproto::DeviceType;
namespace A = hubproto::attr;

namespace {
Attrs attrs(std::initializer_list<std::uint8_t> ids) {
  Attrs a;
  for (auto i : ids) a.add(i);
  return a;
}
}  // namespace

TEST_CASE("the plug stays an on/off light with its metering extras") {
  const auto p = plan_for(DeviceType::Switch, attrs({A::kOnOff, A::kPowerW, A::kEnergyWh}));
  CHECK(p.kind == kOnOffLight);
  CHECK(p.extras == (kXPower | kXEnergy));
  CHECK(kind_of(p.packed()) == kOnOffLight);
  CHECK(extras_of(p.packed()) == (kXPower | kXEnergy));
}

TEST_CASE("lights: dimmable, colour temperature, extended colour") {
  CHECK(plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness})).kind == kDimmableLight);
  CHECK(plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness, A::kColorTemp})).kind ==
        kColorTemperatureLight);
  CHECK(plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness, A::kColorTemp, A::kColorXy}))
            .kind == kExtendedColorLight);
  CHECK(plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness, A::kColorXy})).kind ==
        kExtendedColorLight);
  // Colour without level still needs the dimmable family (Matter colour lights include level).
  CHECK(plan_for(DeviceType::Light, attrs({A::kOnOff, A::kColorTemp})).kind == kColorTemperatureLight);
  CHECK(plan_for(DeviceType::Light, attrs({A::kOnOff})).kind == kOnOffLight);
}

TEST_CASE("sensors: first reading is the type, the rest ride along") {
  auto p = plan_for(DeviceType::Sensor,
                    attrs({A::kTemperature, A::kHumidity, A::kBattery}));
  CHECK(p.kind == kTemperatureSensor);
  CHECK(p.extras == (kXHumidity | kXBattery));

  p = plan_for(DeviceType::Sensor, attrs({A::kHumidity, A::kIlluminance}));
  CHECK(p.kind == kHumiditySensor);
  CHECK(p.extras == kXIlluminance);

  p = plan_for(DeviceType::Sensor, attrs({A::kIlluminance, A::kBattery}));
  CHECK(p.kind == kLightSensor);
  CHECK(p.extras == kXBattery);
}

TEST_CASE("occupancy, contact, button, thermostat") {
  auto p = plan_for(DeviceType::Occupancy, attrs({A::kOccupied, A::kTemperature, A::kBattery}));
  CHECK(p.kind == kOccupancySensor);
  CHECK(p.extras == (kXTemperature | kXBattery));

  p = plan_for(DeviceType::Contact, attrs({A::kContact, A::kBattery}));
  CHECK(p.kind == kContactSensor);
  CHECK(p.extras == kXBattery);

  p = plan_for(DeviceType::Button, attrs({A::kEvent, A::kBattery}));
  CHECK(p.kind == kGenericSwitch);
  CHECK(p.extras == kXBattery);

  p = plan_for(DeviceType::Climate, attrs({A::kTemperature, A::kSetpoint, A::kMode, A::kBattery}));
  CHECK(p.kind == kThermostat);
  CHECK(p.extras == kXBattery);  // temperature is the thermostat's own LocalTemperature
}

TEST_CASE("the co-processor and an empty entity are not bridged") {
  CHECK(plan_for(DeviceType::Chip, attrs({A::kOnOff})).kind == kNone);
  CHECK(plan_for(DeviceType::Switch, attrs({})).kind == kNone);
  CHECK(plan_for(DeviceType::Switch, attrs({A::kBattery})).kind == kNone);
}

TEST_CASE("a stored endpoint is kept when it already serves the plan") {
  const auto plug = plan_for(DeviceType::Switch, attrs({A::kOnOff, A::kPowerW, A::kEnergyWh}));
  CHECK(stored_matches(plug.packed(), plug));
  // Persisted by the older build as the bare on/off-light id: still the same accessory.
  CHECK(stored_matches(kOnOffLight, plug));
  // A lamp that gained brightness is a different endpoint.
  const auto dim = plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness}));
  CHECK_FALSE(stored_matches(kOnOffLight, dim));
  CHECK_FALSE(stored_matches(dim.packed(), plug));
}
