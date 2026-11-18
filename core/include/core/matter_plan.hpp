// Which Matter endpoint(s) a hub entity becomes. From the device_type and the attributes a
// DECLARE lists, choose one primary Matter device type and the extra clusters (measurements,
// battery, power metering) to add to the same endpoint. Humidity is the exception: Apple Home
// renders it only on its own 0x0307 endpoint, so plans_for() returns a second endpoint for it.
// Pure and host-tested; thread-c6 turns the result into esp-matter calls.
//
// The result is packed into one u32 — `kind` in the low 16 bits (the Matter device type id, so a
// value persisted by an older build, which stored the bare on/off-light id, still means the same
// thing) and the extras bitmask above it. esp-matter persists that u32 with the bridged endpoint,
// so a restored endpoint gets exactly the clusters it had.
#pragma once

#include <cstdint>

#include "hubproto/attrs.hpp"
#include "hubproto/payload.hpp"

namespace core::matter {

// Matter device type ids (what `kind` holds).
enum Kind : std::uint16_t {
  kNone = 0,
  kGenericSwitch = 0x000F,
  kContactSensor = 0x0015,
  kOnOffLight = 0x0100,
  kDimmableLight = 0x0101,
  kOnOffPlugInUnit = 0x010A,
  kLightSensor = 0x0106,
  kOccupancySensor = 0x0107,
  kColorTemperatureLight = 0x010C,
  kExtendedColorLight = 0x010D,
  kThermostat = 0x0301,
  kTemperatureSensor = 0x0302,
  kHumiditySensor = 0x0307,
};

// Extra clusters on the same endpoint.
enum Extra : std::uint16_t {
  kXTemperature = 1u << 0,
  // Kept for endpoints persisted by older builds. New plans put humidity on its own
  // endpoint (Apple Home only renders 0x0405 when the endpoint's device type is 0x0307).
  kXHumidity = 1u << 1,
  kXIlluminance = 1u << 2,
  kXBattery = 1u << 3,
  kXPower = 1u << 4,
  kXEnergy = 1u << 5,
};

// One hub entity can need more than one Matter endpoint: a combined temperature/
// humidity sensor becomes a Temperature Sensor and a Humidity Sensor, because Apple
// Home does not show a Relative Humidity cluster that rides along on another device
// type. The channel is stored with the endpoint so the two never collapse into one.
enum Channel : std::uint8_t {
  kChannelMain = 0,
  kChannelHumidity = 1,
};

struct Plan {
  std::uint16_t kind = kNone;
  std::uint16_t extras = 0;
  std::uint32_t packed() const noexcept {
    return static_cast<std::uint32_t>(kind) | (static_cast<std::uint32_t>(extras) << 16);
  }
};

inline std::uint16_t kind_of(std::uint32_t packed) noexcept {
  return static_cast<std::uint16_t>(packed & 0xFFFFu);
}
inline std::uint16_t extras_of(std::uint32_t packed) noexcept {
  return static_cast<std::uint16_t>(packed >> 16);
}

// The attributes an entity declares, as a set.
struct Attrs {
  bool on_off = false, level = false, color_temp = false, color_xy = false;
  bool temperature = false, humidity = false, illuminance = false, battery = false;
  bool power = false, energy = false, setpoint = false, mode = false;
  bool occupied = false, contact = false, event = false;

  void add(std::uint8_t id) noexcept {
    namespace A = hubproto::attr;
    switch (id) {
      case A::kOnOff: on_off = true; break;
      case A::kBrightness: level = true; break;
      case A::kColorTemp: color_temp = true; break;
      case A::kColorXy: color_xy = true; break;
      case A::kTemperature: temperature = true; break;
      case A::kHumidity: humidity = true; break;
      case A::kIlluminance: illuminance = true; break;
      case A::kBattery: battery = true; break;
      case A::kPowerW: power = true; break;
      case A::kEnergyWh: energy = true; break;
      case A::kSetpoint: setpoint = true; break;
      case A::kMode: mode = true; break;
      case A::kOccupied: occupied = true; break;
      case A::kContact: contact = true; break;
      case A::kEvent: event = true; break;
      default: break;
    }
  }
};

inline Plan plan_for(hubproto::DeviceType dt, const Attrs& a) noexcept {
  Plan p;
  auto x = [&](bool have, Extra e) {
    if (have) p.extras = static_cast<std::uint16_t>(p.extras | e);
  };
  if (dt == hubproto::DeviceType::Chip) return p;  // the co-processor itself is not bridged
  if (dt == hubproto::DeviceType::Phone) return p;  // a phone is not a Matter accessory

  // The Power Source cluster says what feeds the device. A battery is a power source like any
  // other, so it rides along with whatever the device is — not only with the sensors. (A
  // battery-powered lamp or plug is rare, but the hub does not care what the device is: it
  // knows the battery level, so the endpoint gets the cluster that reports it.)
  if (a.battery) x(true, kXBattery);

  if (a.setpoint || a.mode) {
    p.kind = kThermostat;  // its LocalTemperature is part of the thermostat cluster
  } else if (a.level || a.color_temp || a.color_xy) {
    p.kind = a.color_xy ? kExtendedColorLight : a.color_temp ? kColorTemperatureLight : kDimmableLight;
    x(a.power, kXPower);
    x(a.energy, kXEnergy);
  } else if (dt == hubproto::DeviceType::Occupancy || a.occupied) {
    p.kind = kOccupancySensor;
    x(a.temperature, kXTemperature);
    x(a.illuminance, kXIlluminance);
  } else if (dt == hubproto::DeviceType::Contact || a.contact) {
    p.kind = kContactSensor;
    x(a.temperature, kXTemperature);
  } else if (dt == hubproto::DeviceType::Button || a.event) {
    p.kind = kGenericSwitch;
  } else if (a.temperature || a.illuminance) {
    // Humidity is handled by plans_for(): it gets its own endpoint. Temperature and
    // illuminance still ride along here, in the order below.
    if (a.temperature) {
      p.kind = kTemperatureSensor;
      x(a.illuminance, kXIlluminance);
    } else {
      p.kind = kLightSensor;
    }
  } else if (a.on_off) {
    // A switch is a plug-in unit, not a light. The Matter type makes Apple Home show
    // an outlet instead of a lamp; its OnOff cluster and command behaviour are the same.
    p.kind = dt == hubproto::DeviceType::Switch ? kOnOffPlugInUnit : kOnOffLight;
    x(a.power, kXPower);
    x(a.energy, kXEnergy);
  }
  return p;
}

// The endpoints one hub entity becomes. `main` is the device the hub already had
// (temperature, occupancy, light...); `humidity` is added only when the entity reports
// humidity and the main endpoint is something else.
struct PlanSet {
  Plan main;
  Plan humidity;

  constexpr bool has_humidity() const noexcept { return humidity.kind != kNone; }
};

// Turn a DECLARE's attributes into the Matter endpoint plan(s) for that entity.
//
// Apple Home creates an accessory from an endpoint's *device type*, not from every
// cluster on it. A RelativeHumidityMeasurement cluster added to a Temperature Sensor
// endpoint therefore stays invisible in Home. Expose humidity as its own 0x0307
// endpoint instead; when it is the only reading, it is the main endpoint.
inline PlanSet plans_for(hubproto::DeviceType dt, const Attrs& a) noexcept {
  PlanSet set;
  if (dt == hubproto::DeviceType::Chip) return set;
  if (dt == hubproto::DeviceType::Phone) return set;  // a phone is not a Matter accessory

  set.main = plan_for(dt, a);  // plan_for() deliberately ignores humidity
  if (!a.humidity) return set;

  if (set.main.kind == kNone) {
    // Nothing else to bridge: the entity is a humidity sensor (possibly with a battery).
    set.main.kind = kHumiditySensor;
    return set;
  }
  set.humidity.kind = kHumiditySensor;
  return set;
}

// Does an endpoint persisted as `stored` (packed) already serve a plan for `want`? A bare legacy
// on/off-light id (no extras recorded) counts as a match for any on/off-light plan: re-creating it
// would give the controller a new accessory and lose the user's rooms and scenes.
inline bool stored_matches(std::uint32_t stored, const Plan& want) noexcept {
  // Old builds put a RelativeHumidityMeasurement cluster on the sensor's main
  // endpoint. That cluster is harmless (Home ignored it), so its bit must not force
  // a perfectly good temperature/occupancy endpoint to be re-created.
  constexpr std::uint16_t kIgnoreExtras = kXHumidity;
  if (kind_of(stored) == want.kind &&
      (extras_of(stored) & ~kIgnoreExtras) == (want.extras & ~kIgnoreExtras)) {
    return true;
  }
  // A legacy on/off-light endpoint may serve an on/off-light plan. It cannot serve
  // the new plug-in-unit plan: the endpoint's device type is fixed after creation,
  // so a plug needs a new endpoint (and therefore a new accessory in the controller).
  return extras_of(stored) == 0 && kind_of(stored) == kOnOffLight && want.kind == kOnOffLight;
}

}  // namespace core::matter
