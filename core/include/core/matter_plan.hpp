// Which Matter endpoint a hub entity becomes. From the device_type and the attributes a DECLARE
// lists, choose one primary Matter device type and the extra clusters (measurements, battery,
// power metering) to add to the same endpoint. Pure and host-tested; thread-c6 turns the result
// into esp-matter calls.
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
  kXHumidity = 1u << 1,
  kXIlluminance = 1u << 2,
  kXBattery = 1u << 3,
  kXPower = 1u << 4,
  kXEnergy = 1u << 5,
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

  if (a.setpoint || a.mode) {
    p.kind = kThermostat;  // its LocalTemperature is part of the thermostat cluster
    x(a.battery, kXBattery);
  } else if (a.level || a.color_temp || a.color_xy) {
    p.kind = a.color_xy ? kExtendedColorLight : a.color_temp ? kColorTemperatureLight : kDimmableLight;
    x(a.power, kXPower);
    x(a.energy, kXEnergy);
  } else if (dt == hubproto::DeviceType::Occupancy || a.occupied) {
    p.kind = kOccupancySensor;
    x(a.temperature, kXTemperature);
    x(a.humidity, kXHumidity);
    x(a.illuminance, kXIlluminance);
    x(a.battery, kXBattery);
  } else if (dt == hubproto::DeviceType::Contact || a.contact) {
    p.kind = kContactSensor;
    x(a.temperature, kXTemperature);
    x(a.battery, kXBattery);
  } else if (dt == hubproto::DeviceType::Button || a.event) {
    p.kind = kGenericSwitch;
    x(a.battery, kXBattery);
  } else if (a.temperature || a.humidity || a.illuminance) {
    // One endpoint per entity: the first reading present is the device type, the others ride
    // along as extra measurement clusters.
    if (a.temperature) {
      p.kind = kTemperatureSensor;
      x(a.humidity, kXHumidity);
      x(a.illuminance, kXIlluminance);
    } else if (a.humidity) {
      p.kind = kHumiditySensor;
      x(a.illuminance, kXIlluminance);
    } else {
      p.kind = kLightSensor;
    }
    x(a.battery, kXBattery);
  } else if (a.on_off) {
    p.kind = kOnOffLight;
    x(a.power, kXPower);
    x(a.energy, kXEnergy);
  }
  return p;
}

// Does an endpoint persisted as `stored` (packed) already serve a plan for `want`? A bare legacy
// on/off-light id (no extras recorded) counts as a match for any on/off-light plan: re-creating it
// would give the controller a new accessory and lose the user's rooms and scenes.
inline bool stored_matches(std::uint32_t stored, const Plan& want) noexcept {
  if (stored == want.packed()) return true;
  return extras_of(stored) == 0 && kind_of(stored) == kOnOffLight && want.kind == kOnOffLight;
}

}  // namespace core::matter
