// Matter -> hub: what one endpoint of a commissioned Matter node becomes.
//
// This is the reverse of matter_plan.hpp (a hub entity -> a Matter endpoint). The hub's
// registry is Matter-cluster-shaped (SPEC Invariant 4), so the translation is a table:
// each entry says "the hub attribute `attr` is Matter cluster/attribute `path`, carries
// type `type`, and is read from there". How a *command* reaches the device is decided by
// `to_matter` (Matter refuses writes to OnOff/CurrentLevel, so those become cluster
// commands), and `to_hub` turns a reading into the hub's units.
//
// Two directions, one table:
//   * a read/subscription turns the Matter value into a hub STATE (`to_hub`),
//   * an app command turns a hub value into a Matter write or invoke (`to_matter`).
//
// Pure and host-tested: the firmware does the TLV and the CHIP calls on top of it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/attrs.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/units.hpp"

namespace core::matter {

// The Matter cluster ids the hub understands.
enum Cluster : std::uint32_t {
  kOnOffCluster       = 0x0006,
  kLevelControl       = 0x0008,
  kColorControl       = 0x0300,
  kThermostat         = 0x0201,
  kIlluminance        = 0x0400,
  kTemperature        = 0x0402,
  kHumidity           = 0x0405,
  kOccupancy          = 0x0406,
  kBooleanState       = 0x0045,
  kPowerSource        = 0x002F,
};

// One hub attribute and where it lives on a Matter endpoint.
//
// `path` is the attribute to read and subscribe. How a *command* reaches the device is
// decided by `to_matter` below, not here: Matter refuses writes to several attributes the
// hub treats as writable (OnOff and CurrentLevel are read-only; you drive them through the
// OnOff/LevelControl commands), so a table entry cannot say it on its own.
struct Binding {
  std::uint8_t attr;        // hubproto::attr id
  std::uint32_t cluster;    // Matter cluster id
  std::uint32_t path;       // Matter attribute id to read/subscribe
  hubproto::ValueType type; // the type of the hub-side value
  std::uint8_t flags;       // kFlagReadable | kFlagWritable | kFlagReportable
};

// The table, in the order attributes should appear in a DECLARE.
//
// kColorXy is deliberately absent: Matter keeps colour in two attributes (CurrentX and
// CurrentY) and a hub attribute is one value, so a colour light shows up with on/off,
// level and colour temperature until that pairing has its own shape.
inline constexpr Binding kBindings[] = {
    // level / colour: the app writes them, subscriptions report them back.
    {hubproto::attr::kOnOff, kOnOffCluster, 0x0000, hubproto::ValueType::Bool,
     hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable},
    {hubproto::attr::kBrightness, kLevelControl, 0x0000, hubproto::ValueType::U8,
     hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable},
    {hubproto::attr::kColorTemp, kColorControl, 0x0007, hubproto::ValueType::U16,
     hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable},
    // sensors: read-only, report-driven (units.hpp turns the device units into ours).
    {hubproto::attr::kTemperature, kTemperature, 0x0000, hubproto::ValueType::I16,
     hubproto::kFlagReadable | hubproto::kFlagReportable},
    {hubproto::attr::kHumidity, kHumidity, 0x0000, hubproto::ValueType::U16,
     hubproto::kFlagReadable | hubproto::kFlagReportable},
    {hubproto::attr::kIlluminance, kIlluminance, 0x0000, hubproto::ValueType::U16,
     hubproto::kFlagReadable | hubproto::kFlagReportable},
    {hubproto::attr::kBattery, kPowerSource, 0x000C /* BatPercentRemaining */,
     hubproto::ValueType::U8, hubproto::kFlagReadable | hubproto::kFlagReportable},
    {hubproto::attr::kOccupied, kOccupancy, 0x0000, hubproto::ValueType::Bool,
     hubproto::kFlagReadable | hubproto::kFlagReportable},
    {hubproto::attr::kContact, kBooleanState, 0x0000, hubproto::ValueType::Bool,
     hubproto::kFlagReadable | hubproto::kFlagReportable},
    // thermostat: LocalTemperature is part of the same cluster, the other two are writable.
    {hubproto::attr::kTemperature, kThermostat, 0x0000, hubproto::ValueType::I16,
     hubproto::kFlagReadable | hubproto::kFlagReportable},
    {hubproto::attr::kSetpoint, kThermostat, 0x0012 /* OccupiedHeatingSetpoint */,
     hubproto::ValueType::I16,
     hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable},
    {hubproto::attr::kMode, kThermostat, 0x001C /* SystemMode */, hubproto::ValueType::Enum8,
     hubproto::kFlagReadable | hubproto::kFlagWritable | hubproto::kFlagReportable},
};

inline constexpr std::size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

// The binding for a hub attribute, or nullptr.
inline const Binding* binding_for_attr(std::uint8_t attr) noexcept {
  for (const auto& b : kBindings) {
    if (b.attr == attr) return &b;
  }
  return nullptr;
}

// The binding for a Matter attribute path, or nullptr.
inline const Binding* binding_for_path(std::uint32_t cluster, std::uint32_t path) noexcept {
  for (const auto& b : kBindings) {
    if (b.cluster == cluster && b.path == path) return &b;
  }
  return nullptr;
}

// What an endpoint says about itself: the DeviceTypeList and the ServerList of its
// Descriptor. The pointers refer to storage that outlives the call.
struct EndpointView {
  std::uint16_t endpoint_id = 0;
  const std::uint16_t* device_types = nullptr;
  std::size_t device_type_count = 0;
  const std::uint32_t* clusters = nullptr;
  std::size_t cluster_count = 0;

  bool has_cluster(std::uint32_t id) const noexcept {
    for (std::size_t i = 0; i < cluster_count; ++i) {
      if (clusters[i] == id) return true;
    }
    return false;
  }
  bool has_device_type(std::uint16_t id) const noexcept {
    for (std::size_t i = 0; i < device_type_count; ++i) {
      if (device_types[i] == id) return true;
    }
    return false;
  }
};

// Matter device types that change the hub's DeviceType without changing the attributes:
// a plug on an OnOff cluster is a Switch, not a Light.
inline constexpr std::uint16_t kOnOffPlugInUnit = 0x010A;
inline constexpr std::uint16_t kDimmablePlugInUnit = 0x010B;

// One hub entity: what to DECLARE for an endpoint.
struct EntityPlan {
  bool known = false;              // false: nothing here the hub models, skip the endpoint
  hubproto::DeviceType device_type = hubproto::DeviceType::Sensor;
  std::uint8_t attr_count = 0;
  hubproto::AttrDecl attrs[kBindingCount] = {};

  bool has(std::uint8_t attr) const noexcept {
    for (std::uint8_t i = 0; i < attr_count; ++i) {
      if (attrs[i].attr_id == attr) return true;
    }
    return false;
  }
};

inline EntityPlan plan_from_endpoint(const EndpointView& ep) noexcept {
  EntityPlan plan;
  auto add = [&](const Binding& b) {
    if (plan.has(b.attr)) return;  // the thermostat repeats kTemperature from the sensor table
    plan.attrs[plan.attr_count++] = hubproto::AttrDecl{b.attr, b.type, b.flags};
  };

  for (const auto& b : kBindings) {
    if (ep.has_cluster(b.cluster)) add(b);
  }
  if (plan.attr_count == 0) return plan;

  plan.known = true;
  if (ep.has_cluster(kThermostat)) {
    plan.device_type = hubproto::DeviceType::Climate;
  } else if (ep.has_cluster(kOnOffCluster) || ep.has_cluster(kLevelControl) ||
             ep.has_cluster(kColorControl)) {
    plan.device_type = (ep.has_device_type(kOnOffPlugInUnit) ||
                        ep.has_device_type(kDimmablePlugInUnit))
                           ? hubproto::DeviceType::Switch
                           : hubproto::DeviceType::Light;
  } else if (ep.has_cluster(kOccupancy)) {
    plan.device_type = hubproto::DeviceType::Occupancy;
  } else if (ep.has_cluster(kBooleanState)) {
    plan.device_type = hubproto::DeviceType::Contact;
  } else {
    plan.device_type = hubproto::DeviceType::Sensor;
  }
  return plan;
}

// ---- a whole node -------------------------------------------------------------------------
// A commissioned device describes itself as several endpoints, and the hub's model has one
// entity per *thing a person can point at*. The two do not always line up: TIMMERFLOTTE is a
// temperature sensor endpoint plus a humidity sensor endpoint, but nobody wants two cards for
// one box on the shelf. So measurement endpoints of one node (Sensor/Occupancy/Contact) merge
// into a single entity — the app then shows one card with temperature and humidity — while
// actuators (a light, a switch, a thermostat) each keep their own entity, because a two-gang
// switch really is two things to control.
inline constexpr std::size_t kNodeEntities = 4;
inline constexpr std::size_t kNodeEntityAttrs = 8;  // == EntityRegistry's default MaxAttrs

// One hub attribute of a node entity, with the Matter path it comes from.
struct NodeAttr {
  std::uint8_t attr;
  hubproto::ValueType type;
  std::uint8_t flags;
  std::uint16_t endpoint;  // where to read/subscribe/write it
  std::uint32_t cluster;
  std::uint32_t path;
};

struct NodeEntity {
  hubproto::DeviceType device_type = hubproto::DeviceType::Sensor;
  std::uint8_t attr_count = 0;
  NodeAttr attrs[kNodeEntityAttrs] = {};

  bool has(std::uint8_t attr) const noexcept {
    for (std::uint8_t i = 0; i < attr_count; ++i) {
      if (attrs[i].attr == attr) return true;
    }
    return false;
  }
};

struct NodePlan {
  bool known = false;
  std::uint8_t entity_count = 0;
  NodeEntity entities[kNodeEntities] = {};
};

// True for the device types that describe a measurement rather than an actuator.
inline bool measurement_type(hubproto::DeviceType t) noexcept {
  return t == hubproto::DeviceType::Sensor || t == hubproto::DeviceType::Occupancy ||
         t == hubproto::DeviceType::Contact;
}

// The binding that applies on this endpoint: kTemperature lives in both the temperature
// cluster and the thermostat cluster, and the endpoint says which one it has. A thermostat
// exposes both (LocalTemperature is the one its setpoint belongs next to), so the thermostat
// row wins there.
inline const Binding* pick_binding(const EndpointView& ep, std::uint8_t attr) noexcept {
  const Binding* match = nullptr;
  const Binding* fallback = nullptr;
  for (const auto& b : kBindings) {
    if (b.attr != attr) continue;
    if (fallback == nullptr) fallback = &b;
    if (!ep.has_cluster(b.cluster)) continue;
    if (match == nullptr || b.cluster == kThermostat) match = &b;
  }
  return match != nullptr ? match : fallback;
}

// Turn every endpoint a node reports into the entities the hub will declare. Endpoints the
// hub does not model are skipped, so the result can have fewer entities than the device has
// endpoints (and none at all for a device made only of root-node clusters).
inline NodePlan plan_node(const EndpointView* endpoints, std::size_t count) noexcept {
  NodePlan node;
  for (std::size_t i = 0; i < count; ++i) {
    const EntityPlan ep = plan_from_endpoint(endpoints[i]);
    if (!ep.known) continue;

    NodeEntity* target = nullptr;
    if (measurement_type(ep.device_type)) {
      for (std::uint8_t e = 0; e < node.entity_count; ++e) {
        if (measurement_type(node.entities[e].device_type)) {
          target = &node.entities[e];
          break;
        }
      }
    }
    if (target == nullptr) {
      if (node.entity_count >= kNodeEntities) continue;
      target = &node.entities[node.entity_count++];
      target->device_type = ep.device_type;
    }

    for (std::uint8_t a = 0; a < ep.attr_count; ++a) {
      const std::uint8_t attr = ep.attrs[a].attr_id;
      if (target->has(attr)) continue;  // two endpoints reporting the same thing
      if (target->attr_count >= kNodeEntityAttrs) break;
      const Binding* b = pick_binding(endpoints[i], attr);
      if (b == nullptr) continue;
      NodeAttr& na = target->attrs[target->attr_count++];
      na.attr = attr;
      na.type = ep.attrs[a].value_type;
      na.flags = ep.attrs[a].flags;
      na.endpoint = endpoints[i].endpoint_id;
      na.cluster = b->cluster;
      na.path = b->path;
    }
  }
  node.known = node.entity_count > 0;
  return node;
}

// ---- values ------------------------------------------------------------------------------
// A Matter attribute value, as the TLV layer hands it over: the raw signed/unsigned number,
// and for xy colour a pair.
struct MatterValue {
  bool boolean = false;
  std::int64_t signed_int = 0;   // temperature, setpoint
  std::uint64_t unsigned_int = 0;  // level, mireds, humidity, illuminance, battery, mode
  std::uint16_t xy[2] = {0, 0};
};

// Turn a Matter value into the hub's attribute value. Returns false when the device marked
// the reading invalid (units.hpp rejects those) — the caller then skips the STATE.
inline bool to_hub(std::uint8_t attr, const MatterValue& mv, hubproto::Value& out) noexcept {
  namespace units = hubproto::units;
  switch (attr) {
    case hubproto::attr::kOnOff:
    case hubproto::attr::kOccupied:
    case hubproto::attr::kContact:
      // BooleanState's StateValue: true = closed, which is what the hub's kContact means.
      out = hubproto::Value::make_bool(mv.boolean);
      return true;
    case hubproto::attr::kBrightness: {
      const auto level = static_cast<std::uint8_t>(mv.unsigned_int & 0xFF);
      if (!units::level_valid(level)) return false;
      out = hubproto::Value::make_u8(level);
      return true;
    }
    case hubproto::attr::kTemperature: {
      std::int16_t deci = 0;
      if (!units::temp_from_centi(static_cast<std::int16_t>(mv.signed_int), deci)) return false;
      out = hubproto::Value::make_i16(deci);
      return true;
    }
    case hubproto::attr::kSetpoint: {
      std::int16_t deci = 0;
      if (!units::temp_from_centi(static_cast<std::int16_t>(mv.signed_int), deci)) return false;
      out = hubproto::Value::make_i16(deci);
      return true;
    }
    case hubproto::attr::kHumidity: {
      std::uint16_t deci = 0;
      if (!units::humidity_from_centi(static_cast<std::uint16_t>(mv.unsigned_int), deci)) return false;
      out = hubproto::Value::make_u16(deci);
      return true;
    }
    case hubproto::attr::kIlluminance: {
      std::uint16_t lux = 0;
      if (!units::lux_from_measured(static_cast<std::uint16_t>(mv.unsigned_int), lux)) return false;
      out = hubproto::Value::make_u16(lux);
      return true;
    }
    case hubproto::attr::kBattery: {
      std::uint8_t pct = 0;
      // Matter BatPercentRemaining is in half percent (0..200).
      if (!units::battery_from_half_percent(static_cast<std::uint8_t>(mv.unsigned_int), pct)) {
        return false;
      }
      out = hubproto::Value::make_u8(pct);
      return true;
    }
    case hubproto::attr::kColorTemp:
      out = hubproto::Value::make_u16(static_cast<std::uint16_t>(mv.unsigned_int));
      return true;
    case hubproto::attr::kColorXy:
      out = hubproto::Value::make_xy(mv.xy[0], mv.xy[1]);
      return true;
    case hubproto::attr::kMode: {
      const auto mode = static_cast<std::uint8_t>(mv.unsigned_int & 0xFF);
      if (!units::mode_valid(mode)) return false;
      out = hubproto::Value::make_enum8(mode);
      return true;
    }
    default:
      return false;
  }
}

// ---- hub command -> Matter ----------------------------------------------------------------
// Matter offers two ways to change something: write an attribute, or invoke a cluster command.
// Which one applies is a property of the attribute, not of the device — OnOff and CurrentLevel
// are read-only attributes on every light — so it lives here, next to the table, rather than in
// the Binding (a table row cannot say it on its own).

enum class SendKind : std::uint8_t {
  kNone = 0,  // nothing to send: a sensor reading, or a value out of range
  kWrite,     // write an attribute
  kInvoke,    // invoke a cluster command
};

// One field of what is being sent. `tag` is the context tag inside the command structure; for a
// write the value goes in a one-element structure, so its tag is 0 as well (that is what the
// ESP JSON layer expects: {"0:U8": 128} / {"0:BOOL": true}).
struct MatterField {
  std::uint8_t tag;
  hubproto::Value value;  // already in Matter units
};

inline constexpr std::size_t kMaxFields = 4;

// What to send, and where. `endpoint` is the endpoint the attribute was discovered on.
struct WritePlan {
  SendKind kind = SendKind::kNone;
  std::uint16_t endpoint = 0;
  std::uint32_t cluster = 0;
  std::uint32_t id = 0;  // attribute id for kWrite, command id for kInvoke
  std::uint8_t field_count = 0;
  MatterField fields[kMaxFields] = {};

  bool ok() const noexcept { return kind != SendKind::kNone; }
};

// Matter command ids the hub uses.
inline constexpr std::uint32_t kOnOffCommand = 0x00;          // OnOff(OnOff)
inline constexpr std::uint32_t kMoveToLevelWithOnOff = 0x04;  // LevelControl(Level, ...)

// Turn an app command into the write or invoke that changes the same thing on the device.
// Returns a plan with kind == kNone when the attribute is read-only or the value would be
// rejected by the device, so the caller can answer CMD_ACK Unsupported instead of sending it.
inline WritePlan to_matter(std::uint8_t attr, const hubproto::Value& v,
                           std::uint16_t endpoint) noexcept {
  namespace units = hubproto::units;
  WritePlan plan;
  plan.endpoint = endpoint;
  const Binding* b = binding_for_attr(attr);
  if (b == nullptr || (b->flags & hubproto::kFlagWritable) == 0) return plan;
  if (v.type != b->type) return plan;

  auto field = [&plan](std::uint8_t tag, hubproto::Value fv) {
    plan.fields[plan.field_count++] = MatterField{tag, fv};
  };

  switch (attr) {
    case hubproto::attr::kOnOff:
      // Switching a light is the OnOff command; the attribute itself is read-only.
      plan.kind = SendKind::kInvoke;
      plan.cluster = kOnOffCluster;
      plan.id = kOnOffCommand;
      field(0, hubproto::Value::make_bool(v.as_bool()));
      return plan;

    case hubproto::attr::kBrightness: {
      const std::uint8_t level = v.as_u8();
      if (!units::level_valid(level)) return WritePlan{};  // 255 is ZCL/Matter "invalid"
      // MoveToLevelWithOnOff(Level, TransitionTime, OptionsMask, OptionsOverride): the level is
      // what the app picked, the rest is "right now, and even if the light is off".
      plan.kind = SendKind::kInvoke;
      plan.cluster = kLevelControl;
      plan.id = kMoveToLevelWithOnOff;
      field(0, hubproto::Value::make_u8(level));
      field(1, hubproto::Value::make_u16(0));
      field(2, hubproto::Value::make_u8(0));
      field(3, hubproto::Value::make_u8(0));
      return plan;
    }

    case hubproto::attr::kColorTemp:
      plan.kind = SendKind::kWrite;
      plan.cluster = kColorControl;
      plan.id = 0x0007;  // ColorTemperatureMireds
      field(0, hubproto::Value::make_u16(v.as_u16()));
      return plan;

    case hubproto::attr::kSetpoint:
      plan.kind = SendKind::kWrite;
      plan.cluster = kThermostat;
      plan.id = 0x0012;  // OccupiedHeatingSetpoint
      field(0, hubproto::Value::make_i16(units::temp_to_centi(v.as_i16())));
      return plan;

    case hubproto::attr::kMode: {
      const std::uint8_t mode = v.as_enum8();
      if (!units::mode_valid(mode)) return WritePlan{};
      plan.kind = SendKind::kWrite;
      plan.cluster = kThermostat;
      plan.id = 0x001C;  // SystemMode (an enum8, which Matter encodes as a uint8)
      field(0, hubproto::Value::make_u8(mode));
      return plan;
    }

    default:
      return WritePlan{};
  }
}

}  // namespace core::matter
