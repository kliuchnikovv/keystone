// What a Zigbee endpoint is, in the hub's terms. Given the clusters an endpoint advertises (its
// simple descriptor) decide which device_type it becomes and which attributes it declares; map
// button commands to events. Pure functions with plain cluster numbers — no esp-zigbee headers —
// so the whole mapping is host-tested and the firmware only does the radio work around it.
//
// Rules (SPEC §6.1): one entity per application endpoint. The *primary* cluster picks the type
// (thermostat > light > occupancy > contact > sensor > button > plug); every other cluster the
// endpoint has adds attributes (temperature, humidity, battery, power, ...), so a motion sensor
// that also reports temperature is still one `occupancy` entity with a `temperature` attribute.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/attrs.hpp"
#include "hubproto/payload.hpp"

namespace core::zcl {

namespace cluster {
inline constexpr std::uint16_t kBasic = 0x0000;
inline constexpr std::uint16_t kPowerCfg = 0x0001;
inline constexpr std::uint16_t kIdentify = 0x0003;
inline constexpr std::uint16_t kScenes = 0x0005;
inline constexpr std::uint16_t kOnOff = 0x0006;
inline constexpr std::uint16_t kLevel = 0x0008;
inline constexpr std::uint16_t kMultistateIn = 0x0012;
inline constexpr std::uint16_t kThermostat = 0x0201;
inline constexpr std::uint16_t kColor = 0x0300;
inline constexpr std::uint16_t kIlluminance = 0x0400;
inline constexpr std::uint16_t kTemperature = 0x0402;
inline constexpr std::uint16_t kHumidity = 0x0405;
inline constexpr std::uint16_t kOccupancy = 0x0406;
inline constexpr std::uint16_t kIasZone = 0x0500;
inline constexpr std::uint16_t kElecMeas = 0x0B04;
inline constexpr std::uint16_t kMetering = 0x0702;
}  // namespace cluster

// Capabilities of one endpoint = which hub attributes it carries. Stored with the entity.
enum Cap : std::uint32_t {
  kCapOnOff = 1u << 0,
  kCapLevel = 1u << 1,
  kCapColorTemp = 1u << 2,
  kCapColorXy = 1u << 3,
  kCapTemperature = 1u << 4,
  kCapHumidity = 1u << 5,
  kCapIlluminance = 1u << 6,
  kCapBattery = 1u << 7,
  kCapOccupancy = 1u << 8,
  kCapContact = 1u << 9,
  kCapThermostat = 1u << 10,  // setpoint + mode (+ temperature from LocalTemperature)
  kCapButton = 1u << 11,
  kCapPower = 1u << 12,
  kCapEnergy = 1u << 13,
  kCapIas = 1u << 14,  // occupancy/contact comes from IAS Zone notifications, not a measurement cluster
};

struct Profile {
  bool valid = false;  // false: nothing here the hub models
  hubproto::DeviceType type = hubproto::DeviceType::Switch;
  std::uint32_t caps = 0;
  // An IAS Zone endpoint: the zone *type* (motion vs contact) is an attribute that has to be read
  // before the entity can be created, so `valid` stays false until `resolve_ias` is called.
  bool ias = false;
  // The Color Control cluster is present: which of xy / colour temperature the lamp supports is
  // an attribute (ColorCapabilities) that refines `caps` once read; both are assumed until then.
  bool color_caps_pending = false;
};

namespace detail {
inline bool has(const std::uint16_t* list, std::size_t n, std::uint16_t id) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (list[i] == id) return true;
  }
  return false;
}
}  // namespace detail

// `in`: the endpoint's input (server) clusters; `out`: its output (client) clusters.
inline Profile classify(const std::uint16_t* in, std::size_t n_in, const std::uint16_t* out,
                        std::size_t n_out) noexcept {
  using detail::has;
  Profile p;
  std::uint32_t c = 0;

  const bool on_off = has(in, n_in, cluster::kOnOff);
  const bool level = has(in, n_in, cluster::kLevel);
  const bool color = has(in, n_in, cluster::kColor);
  const bool thermostat = has(in, n_in, cluster::kThermostat);
  const bool occupancy = has(in, n_in, cluster::kOccupancy);
  p.ias = has(in, n_in, cluster::kIasZone);

  if (on_off) c |= kCapOnOff;
  if (level) c |= kCapLevel;
  if (color) {
    c |= kCapColorTemp | kCapColorXy;
    p.color_caps_pending = true;
  }
  if (thermostat) c |= kCapThermostat | kCapTemperature;
  if (occupancy) c |= kCapOccupancy;
  if (has(in, n_in, cluster::kTemperature)) c |= kCapTemperature;
  if (has(in, n_in, cluster::kHumidity)) c |= kCapHumidity;
  if (has(in, n_in, cluster::kIlluminance)) c |= kCapIlluminance;
  if (has(in, n_in, cluster::kElecMeas)) c |= kCapPower;
  if (has(in, n_in, cluster::kMetering)) c |= kCapEnergy;

  // A button either sends commands (output clusters On/Off, Level, Scenes) without being a
  // light/plug itself, or reports a multistate input (Aqara-style).
  const bool sends = has(out, n_out, cluster::kOnOff) || has(out, n_out, cluster::kLevel) ||
                     has(out, n_out, cluster::kScenes);
  const bool multistate = has(in, n_in, cluster::kMultistateIn);
  const bool controllable = on_off || level || color;

  const bool sensor_attrs =
      (c & (kCapTemperature | kCapHumidity | kCapIlluminance)) != 0;

  if (thermostat) {
    p.type = hubproto::DeviceType::Climate;
    p.valid = true;
  } else if (controllable && (level || color)) {
    p.type = hubproto::DeviceType::Light;
    p.valid = true;
  } else if (occupancy) {
    p.type = hubproto::DeviceType::Occupancy;
    p.valid = true;
  } else if (p.ias) {
    p.valid = false;  // resolved from the zone type
  } else if (sensor_attrs && !on_off) {
    p.type = hubproto::DeviceType::Sensor;
    p.valid = true;
  } else if ((sends || multistate) && !on_off) {
    c |= kCapButton;
    p.type = hubproto::DeviceType::Button;
    p.valid = true;
  } else if (on_off) {
    p.type = hubproto::DeviceType::Switch;
    p.valid = true;
  }

  // Battery: only on devices that are plausibly battery powered. A plug or a lamp also carries
  // Power Configuration (mains voltage), which would otherwise declare a `battery` that never
  // reports.
  const bool battery_type = p.type != hubproto::DeviceType::Switch &&
                            p.type != hubproto::DeviceType::Light;
  if (has(in, n_in, cluster::kPowerCfg) && (p.ias || (p.valid && battery_type))) {
    c |= kCapBattery;
  }
  p.caps = c;
  return p;
}

// IAS Zone type (ZCL attribute ZoneType) -> what the entity becomes. False: a kind we do not
// model (smoke, water leak, ...): the endpoint is skipped.
inline bool resolve_ias(std::uint16_t zone_type, Profile& p) noexcept {
  switch (zone_type) {
    case 0x000D:  // motion sensor
      p.type = hubproto::DeviceType::Occupancy;
      p.caps |= kCapOccupancy | kCapIas;
      break;
    case 0x0015:  // contact switch
    case 0x0016:  // (ZCL "contact switch" family on some devices)
      p.type = hubproto::DeviceType::Contact;
      p.caps |= kCapContact | kCapIas;
      break;
    default:
      return false;
  }
  p.valid = true;
  return true;
}

// ColorCapabilities (ZCL attribute 0x400A) bit0 hue/sat, bit2 enhanced hue, bit3 loop, bit4 xy,
// bit5 colour temperature. Narrow the assumed caps to what the lamp says it has.
inline std::uint32_t refine_color_caps(std::uint32_t caps, std::uint16_t color_capabilities) noexcept {
  caps &= ~(kCapColorTemp | kCapColorXy);
  if ((color_capabilities & 0x0010u) != 0) caps |= kCapColorXy;
  if ((color_capabilities & 0x0020u) != 0) caps |= kCapColorTemp;
  return caps;
}

// The attributes an entity of `caps` declares, in a fixed order (on_off first; the plug keeps its
// original on_off, power_w, energy_wh order). Returns how many were written (at most `max`).
inline std::size_t attrs_for(std::uint32_t caps, hubproto::AttrDecl* out, std::size_t max) noexcept {
  using hubproto::ValueType;
  constexpr std::uint8_t R = hubproto::kFlagReadable;
  constexpr std::uint8_t W = hubproto::kFlagWritable;
  constexpr std::uint8_t P = hubproto::kFlagReportable;
  struct Row {
    std::uint32_t cap;
    std::uint8_t id;
    ValueType vt;
    std::uint8_t flags;
  };
  static constexpr Row kRows[] = {
      {kCapOnOff, hubproto::attr::kOnOff, ValueType::Bool, R | W | P},
      {kCapLevel, hubproto::attr::kBrightness, ValueType::U8, R | W | P},
      {kCapColorTemp, hubproto::attr::kColorTemp, ValueType::U16, R | W | P},
      {kCapColorXy, hubproto::attr::kColorXy, ValueType::XyColor, R | W | P},
      {kCapTemperature, hubproto::attr::kTemperature, ValueType::I16, R | P},
      {kCapHumidity, hubproto::attr::kHumidity, ValueType::U16, R | P},
      {kCapIlluminance, hubproto::attr::kIlluminance, ValueType::U16, R | P},
      {kCapPower, hubproto::attr::kPowerW, ValueType::U16, R | P},
      {kCapEnergy, hubproto::attr::kEnergyWh, ValueType::U32, R | P},
      {kCapBattery, hubproto::attr::kBattery, ValueType::U8, R | P},
      {kCapThermostat, hubproto::attr::kSetpoint, ValueType::I16, R | W | P},
      {kCapThermostat, hubproto::attr::kMode, ValueType::Enum8, R | W | P},
      {kCapOccupancy, hubproto::attr::kOccupied, ValueType::Bool, R | P},
      {kCapContact, hubproto::attr::kContact, ValueType::Bool, R | P},
      {kCapButton, hubproto::attr::kEvent, ValueType::Enum8, R | P},
  };
  std::size_t n = 0;
  for (const Row& r : kRows) {
    if ((caps & r.cap) == 0 || n >= max) continue;
    out[n++] = {r.id, r.vt, r.flags};
  }
  return n;
}

// The value type of a hub attribute this module declares (what a COMMAND for it carries).
inline bool attr_value_type(std::uint8_t id, hubproto::ValueType& vt) noexcept {
  hubproto::AttrDecl all[16];
  const std::size_t n = attrs_for(0xFFFFFFFFu, all, 16);
  for (std::size_t i = 0; i < n; ++i) {
    if (all[i].attr_id == id) {
      vt = all[i].value_type;
      return true;
    }
  }
  return false;
}

// A command a remote sent to the hub -> a button event (0 = not a button command we model).
// `payload0` is the first payload byte (move/step mode).
inline hubproto::attr::ButtonEvent button_event(std::uint16_t cluster_id, std::uint8_t cmd,
                                                std::uint8_t payload0) noexcept {
  using E = hubproto::attr::ButtonEvent;
  if (cluster_id == cluster::kOnOff) {
    switch (cmd) {
      case 0x00: return E::Off;
      case 0x01: return E::On;
      case 0x02: return E::Press;  // toggle
      default: return E::Idle;
    }
  }
  if (cluster_id == cluster::kLevel) {
    switch (cmd) {
      case 0x01:  // move
      case 0x05:  // move (with on/off)
      case 0x02:  // step
      case 0x06:  // step (with on/off)
        return payload0 == 0 ? E::Up : E::Down;  // mode 0 = up, 1 = down
      case 0x03:  // stop
      case 0x07:  // stop (with on/off)
        return E::Release;
      default: return E::Idle;
    }
  }
  return E::Idle;
}

// Aqara-style multistate input present value -> event.
inline hubproto::attr::ButtonEvent multistate_event(std::uint16_t v) noexcept {
  using E = hubproto::attr::ButtonEvent;
  switch (v) {
    case 1: return E::Press;
    case 2: return E::Double;
    case 0:
    case 16: return E::Hold;
    case 17: return E::Release;
    default: return E::Idle;
  }
}

// A read response describes the device as it was when the device handled the read. If a command went out
// after that read was sent, the answer may be older than the command: it can arrive after the command's
// own confirmation and put the old value back (a plug that is on, shown as off until the next report).
// The firmware keeps one of these per entity and drops such an answer, asking again instead.
// Times are the chip's millisecond clock; wraparound is handled.
struct ReadGuard {
  static constexpr std::uint32_t kWindowMs = 5000;  // a read answer later than this after a command is not coming

  // Plain members, zero means "never": the firmware keeps this inside structs it clears with memset.
  bool has_cmd;
  bool has_read;
  std::uint32_t cmd_ms;
  std::uint32_t read_ms;

  void note_command(std::uint32_t now_ms) noexcept { has_cmd = true; cmd_ms = now_ms; }
  void note_read(std::uint32_t now_ms) noexcept { has_read = true; read_ms = now_ms; }

  // True for an answer of a read the firmware sent before its last command, received soon after that command.
  // Only the clusters a command changes can be out of date this way.
  bool stale(std::uint16_t cl, std::uint32_t now_ms) const noexcept {
    if (!has_cmd || !has_read) return false;
    if (cl != cluster::kOnOff && cl != cluster::kLevel && cl != cluster::kColor && cl != cluster::kThermostat) return false;
    if (static_cast<std::uint32_t>(now_ms - cmd_ms) >= kWindowMs) return false;
    return static_cast<std::int32_t>(cmd_ms - read_ms) >= 0;  // the command is not older than the read
  }
};

}  // namespace core::zcl
