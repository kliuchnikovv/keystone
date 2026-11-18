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

TEST_CASE("the plug becomes an on/off plug-in unit with its metering extras") {
  const auto p = plan_for(DeviceType::Switch, attrs({A::kOnOff, A::kPowerW, A::kEnergyWh}));
  CHECK(p.kind == kOnOffPlugInUnit);
  CHECK(p.extras == (kXPower | kXEnergy));
  CHECK(kind_of(p.packed()) == kOnOffPlugInUnit);
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

TEST_CASE("sensors: the first reading is the main endpoint, humidity gets its own") {
  auto set = plans_for(DeviceType::Sensor,
                       attrs({A::kTemperature, A::kHumidity, A::kBattery}));
  CHECK(set.main.kind == kTemperatureSensor);
  CHECK(set.main.extras == kXBattery);
  CHECK(set.has_humidity());
  CHECK(set.humidity.kind == kHumiditySensor);
  CHECK(set.humidity.extras == 0);  // the battery stays with the main endpoint

  // Apple Home renders ambient light and humidity as two separate accessories.
  set = plans_for(DeviceType::Sensor, attrs({A::kHumidity, A::kIlluminance}));
  CHECK(set.main.kind == kLightSensor);
  CHECK(set.humidity.kind == kHumiditySensor);

  // Humidity alone stays a single endpoint — no empty second accessory.
  set = plans_for(DeviceType::Sensor, attrs({A::kHumidity, A::kBattery}));
  CHECK(set.main.kind == kHumiditySensor);
  CHECK(set.main.extras == kXBattery);
  CHECK_FALSE(set.has_humidity());

  set = plans_for(DeviceType::Sensor, attrs({A::kIlluminance, A::kBattery}));
  CHECK(set.main.kind == kLightSensor);
  CHECK(set.main.extras == kXBattery);
  CHECK_FALSE(set.has_humidity());
}

TEST_CASE("occupancy, contact, button, thermostat") {
  auto p = plan_for(DeviceType::Occupancy, attrs({A::kOccupied, A::kTemperature, A::kBattery}));
  CHECK(p.kind == kOccupancySensor);
  CHECK(p.extras == (kXTemperature | kXBattery));

  const auto occ = plans_for(DeviceType::Occupancy,
                             attrs({A::kOccupied, A::kTemperature, A::kHumidity, A::kBattery}));
  CHECK(occ.main.kind == kOccupancySensor);
  CHECK(occ.humidity.kind == kHumiditySensor);

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

TEST_CASE("a battery is a Power Source whatever the device is") {
  // The battery rides along with every kind: the hub knows the level, so the endpoint gets
  // the cluster that reports it. It is not only for the sensors.
  auto p = plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness, A::kBattery}));
  CHECK(p.kind == kDimmableLight);
  CHECK(p.extras == kXBattery);

  p = plan_for(DeviceType::Switch, attrs({A::kOnOff, A::kPowerW, A::kBattery}));
  CHECK(p.kind == kOnOffPlugInUnit);
  CHECK(p.extras == (kXPower | kXBattery));

  p = plan_for(DeviceType::Sensor, attrs({A::kTemperature, A::kBattery}));
  CHECK(p.kind == kTemperatureSensor);
  CHECK(p.extras == kXBattery);

  // The occupancy sensor keeps its measurement clusters next to the battery.
  p = plan_for(DeviceType::Occupancy, attrs({A::kOccupied, A::kTemperature, A::kBattery}));
  CHECK(p.kind == kOccupancySensor);
  CHECK(p.extras == (kXTemperature | kXBattery));

  // No battery attribute, no Power Source cluster.
  p = plan_for(DeviceType::Sensor, attrs({A::kTemperature}));
  CHECK(p.extras == 0);
}

TEST_CASE("a stored endpoint is kept when it already serves the plan") {
  const auto plug = plan_for(DeviceType::Switch, attrs({A::kOnOff, A::kPowerW, A::kEnergyWh}));
  CHECK(stored_matches(plug.packed(), plug));
  // Persisted by the older build as the bare on/off-light id. A plug now needs the
  // plug-in-unit type, so this endpoint must be re-created.
  CHECK_FALSE(stored_matches(kOnOffLight, plug));
  // A lamp that gained brightness is a different endpoint.
  const auto dim = plan_for(DeviceType::Light, attrs({A::kOnOff, A::kBrightness}));
  CHECK_FALSE(stored_matches(kOnOffLight, dim));
  CHECK_FALSE(stored_matches(dim.packed(), plug));

  // An endpoint persisted by the old build still carries kXHumidity on the main
  // sensor. That extra cluster is now redundant, but must not force a re-create:
  // its UniqueID would change and Apple Home would lose the room and automations.
  const auto sensor = plans_for(DeviceType::Sensor,
                                attrs({A::kTemperature, A::kHumidity, A::kBattery}));
  CHECK(stored_matches((kXHumidity | kXBattery) << 16 | kTemperatureSensor, sensor.main));
}
