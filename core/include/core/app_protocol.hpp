// App protocol (docs/app-protocol.md) message layer: decode what a client sends,
// encode what the hub sends. Pure functions over core::JsonReader / JsonWriter —
// no sockets, no ESP-IDF — so the whole wire format is host-tested. The transport
// (esp_http_server WebSocket) lives in the brain app.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/json.hpp"
#include "core/rule_engine.hpp"
#include "core/scene_table.hpp"
#include "core/schedule_table.hpp"
#include "hubproto/payload.hpp"

namespace core::app {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kMaxFrame = 4096;  // both directions
inline constexpr std::size_t kMaxToken = 64;

// ---- names -------------------------------------------------------------------

// attr_id -> protocol name (SPEC §6.1 table; one name per id). nullptr if unknown.
inline const char* attr_name(std::uint8_t id) noexcept {
  switch (id) {
    case 0x01: return "on_off";
    case 0x02: return "brightness";
    case 0x03: return "color_xy";
    case 0x04: return "color_temp";
    case 0x10: return "temperature";
    case 0x11: return "humidity";
    case 0x12: return "illuminance";
    case 0x13: return "power_w";
    case 0x14: return "energy_wh";
    case 0x1F: return "battery";
    case 0x20: return "position";
    case 0x21: return "state";
    case 0x30: return "locked";
    case 0x40: return "setpoint";
    case 0x41: return "mode";
    case 0x50: return "occupied";
    case 0x51: return "contact";
    case 0x60: return "event";
    default: return nullptr;
  }
}

inline const char* device_name(hubproto::DeviceType t) noexcept {
  switch (t) {
    case hubproto::DeviceType::Light: return "light";
    case hubproto::DeviceType::Sensor: return "sensor";
    case hubproto::DeviceType::Switch: return "switch";
    case hubproto::DeviceType::Cover: return "cover";
    case hubproto::DeviceType::Lock: return "lock";
    case hubproto::DeviceType::Climate: return "climate";
    case hubproto::DeviceType::Occupancy: return "occupancy";
    case hubproto::DeviceType::Contact: return "contact";
    case hubproto::DeviceType::Button: return "button";
    case hubproto::DeviceType::Chip: return "chip";
  }
  return "unknown";
}

inline const char* vt_name(hubproto::ValueType t) noexcept {
  switch (t) {
    case hubproto::ValueType::Bool: return "bool";
    case hubproto::ValueType::U8: return "u8";
    case hubproto::ValueType::I16: return "i16";
    case hubproto::ValueType::U16: return "u16";
    case hubproto::ValueType::F32: return "f32";
    case hubproto::ValueType::XyColor: return "xy";
    case hubproto::ValueType::Enum8: return "enum8";
    case hubproto::ValueType::U32: return "u32";
  }
  return "unknown";
}

enum class Err : std::uint8_t {
  Unauthorized, Version, Busy, BadJson, TooLarge, Unsupported, BadArgs, NotFound,
  ReadOnly, TableFull, Internal
};

inline const char* err_name(Err e) noexcept {
  switch (e) {
    case Err::Unauthorized: return "unauthorized";
    case Err::Version: return "version";
    case Err::Busy: return "busy";
    case Err::BadJson: return "bad_json";
    case Err::TooLarge: return "too_large";
    case Err::Unsupported: return "unsupported";
    case Err::BadArgs: return "bad_args";
    case Err::NotFound: return "not_found";
    case Err::ReadOnly: return "read_only";
    case Err::TableFull: return "table_full";
    case Err::Internal: return "internal";
  }
  return "internal";
}

inline const char* ack_name(hubproto::CmdAckStatus s) noexcept {
  switch (s) {
    case hubproto::CmdAckStatus::Ok: return "ok";
    case hubproto::CmdAckStatus::Fail: return "fail";
    case hubproto::CmdAckStatus::Unreachable: return "unreachable";
    case hubproto::CmdAckStatus::Unsupported: return "unsupported";
  }
  return "fail";
}

inline const char* cmp_name(CmpOp op) noexcept {
  switch (op) {
    case CmpOp::Always: return "any";
    case CmpOp::Eq: return "==";
    case CmpOp::Ne: return "!=";
    case CmpOp::Lt: return "<";
    case CmpOp::Le: return "<=";
    case CmpOp::Gt: return ">";
    case CmpOp::Ge: return ">=";
  }
  return "any";
}

// ---- values ------------------------------------------------------------------

// JSON-native value: bool, number, [x, y] for xy_color.
inline void write_value(JsonWriter& w, const hubproto::Value& v) noexcept {
  switch (v.type) {
    case hubproto::ValueType::Bool: w.boolean(v.as_bool()); break;
    case hubproto::ValueType::U8: w.u64(v.as_u8()); break;
    case hubproto::ValueType::Enum8: w.u64(v.as_enum8()); break;
    case hubproto::ValueType::I16: w.i64(v.as_i16()); break;
    case hubproto::ValueType::U16: w.u64(v.as_u16()); break;
    case hubproto::ValueType::U32: w.u64(v.as_u32()); break;
    case hubproto::ValueType::F32: w.f64(static_cast<double>(v.as_f32())); break;
    case hubproto::ValueType::XyColor: {
      std::uint16_t x = 0, y = 0;
      v.as_xy(x, y);
      w.begin_array().u64(x).u64(y).end_array();
      break;
    }
  }
}

// A value as it arrived in JSON, before it is checked against the attribute type.
struct Scalar {
  enum class Kind : std::uint8_t { None, Bool, Int, Float, Pair } kind = Kind::None;
  bool b = false;
  std::int64_t i = 0;
  double f = 0;
  std::int64_t x = 0, y = 0;
};

template <typename Reader>
Scalar read_scalar(const Reader& r, int tok) noexcept {
  Scalar s;
  if (tok < 0) return s;
  if (r.get_bool(tok, s.b)) {
    s.kind = Scalar::Kind::Bool;
  } else if (r.get_i64(tok, s.i)) {
    s.kind = Scalar::Kind::Int;
  } else if (r.get_double(tok, s.f)) {
    s.kind = Scalar::Kind::Float;
  } else if (r.is(tok, Reader::Type::Array) && r.count(tok) == 2 &&
             r.get_i64(r.at(tok, 0), s.x) && r.get_i64(r.at(tok, 1), s.y)) {
    s.kind = Scalar::Kind::Pair;
  }
  return s;
}

// Convert to the attribute's wire value, with range checks. False = does not fit.
inline bool to_value(const Scalar& s, hubproto::ValueType vt, hubproto::Value& out) noexcept {
  using VT = hubproto::ValueType;
  auto in = [&](std::int64_t lo, std::int64_t hi) {
    return s.kind == Scalar::Kind::Int && s.i >= lo && s.i <= hi;
  };
  switch (vt) {
    case VT::Bool:
      if (s.kind != Scalar::Kind::Bool) return false;
      out = hubproto::Value::make_bool(s.b);
      return true;
    case VT::U8:
      if (!in(0, 255)) return false;
      out = hubproto::Value::make_u8(static_cast<std::uint8_t>(s.i));
      return true;
    case VT::Enum8:
      if (!in(0, 255)) return false;
      out = hubproto::Value::make_enum8(static_cast<std::uint8_t>(s.i));
      return true;
    case VT::I16:
      if (!in(-32768, 32767)) return false;
      out = hubproto::Value::make_i16(static_cast<std::int16_t>(s.i));
      return true;
    case VT::U16:
      if (!in(0, 65535)) return false;
      out = hubproto::Value::make_u16(static_cast<std::uint16_t>(s.i));
      return true;
    case VT::U32:
      if (!in(0, 0xFFFFFFFFLL)) return false;
      out = hubproto::Value::make_u32(static_cast<std::uint32_t>(s.i));
      return true;
    case VT::F32: {
      double d = 0;
      if (s.kind == Scalar::Kind::Int) d = static_cast<double>(s.i);
      else if (s.kind == Scalar::Kind::Float) d = s.f;
      else return false;
      out = hubproto::Value::make_f32(static_cast<float>(d));
      return true;
    }
    case VT::XyColor:
      if (s.kind != Scalar::Kind::Pair || s.x < 0 || s.x > 65535 || s.y < 0 || s.y > 65535) return false;
      out = hubproto::Value::make_xy(static_cast<std::uint16_t>(s.x), static_cast<std::uint16_t>(s.y));
      return true;
  }
  return false;
}

// ---- client -> hub -------------------------------------------------------------

struct ClientMsg {
  enum class Kind : std::uint8_t { Hello, Cmd, Req, Unknown, Invalid } kind = Kind::Invalid;
  bool has_id = false;
  std::int32_t id = 0;
  // hello
  std::int64_t v = 0;
  bool token_ok = false;  // present, a string, and short enough
  char token[kMaxToken + 1] = {};
  char client[33] = {};
  // cmd
  bool has_entity = false, has_attr = false;
  std::int64_t entity = 0, attr = 0;
  Scalar value;
  // req
  char op[24] = {};
  int args = -1;  // token index of "args" in the reader, or -1
};

// Parse one frame. `r` keeps the token tree alive for `args` access, and must
// outlive the returned ClientMsg's use of `args`. Anything that is not a JSON
// object with a string "type" is Kind::Invalid (answer: bad_json).
template <typename Reader>
ClientMsg decode_client(Reader& r, const char* text, std::size_t len) noexcept {
  ClientMsg m;
  if (len > kMaxFrame || !r.parse(text, len) || !r.is(r.root(), Reader::Type::Object)) return m;
  char type[16];
  if (!r.get_string(r.find(r.root(), "type"), type, sizeof(type))) {
    // A "type" that is missing, not a string, or longer than any real type.
    m.kind = r.find(r.root(), "type") >= 0 ? ClientMsg::Kind::Unknown : ClientMsg::Kind::Invalid;
    return m;
  }
  const int root = r.root();
  std::int64_t id = 0;
  if (r.get_i64(r.find(root, "id"), id) && id >= 0 && id <= 0x7FFFFFFF) {
    m.has_id = true;
    m.id = static_cast<std::int32_t>(id);
  }
  if (std::strcmp(type, "hello") == 0) {
    m.kind = ClientMsg::Kind::Hello;
    r.get_i64(r.find(root, "v"), m.v);
    m.token_ok = r.get_string(r.find(root, "token"), m.token, sizeof(m.token));
    if (!m.token_ok) m.token[0] = '\0';
    if (!r.get_string(r.find(root, "client"), m.client, sizeof(m.client))) m.client[0] = '\0';
  } else if (std::strcmp(type, "cmd") == 0) {
    m.kind = ClientMsg::Kind::Cmd;
    m.has_entity = r.get_i64(r.find(root, "entity"), m.entity);
    m.has_attr = r.get_i64(r.find(root, "attr"), m.attr);
    m.value = read_scalar(r, r.find(root, "value"));
  } else if (std::strcmp(type, "req") == 0) {
    m.kind = ClientMsg::Kind::Req;
    if (!r.get_string(r.find(root, "op"), m.op, sizeof(m.op))) m.op[0] = '\0';
    m.args = r.find(root, "args");
  } else {
    m.kind = ClientMsg::Kind::Unknown;
  }
  return m;
}

// ---- hub -> client: single frames ------------------------------------------------

struct HubInfo {
  const char* name;
  const char* fw;
  std::int64_t time;  // unix seconds, 0 if unknown
  const char* tz;
  bool synced;
};

inline void encode_welcome(JsonWriter& w, const HubInfo& h) noexcept {
  w.begin_object().kv("type", "welcome").kv("v", kProtocolVersion)
      .key("hub").begin_object()
      .kv("name", h.name).kv("fw", h.fw).kv("time", h.time).kv("tz", h.tz).kv("synced", h.synced)
      .end_object().end_object();
}

// `id` < 0 = not tied to a request.
inline void encode_error(JsonWriter& w, std::int64_t id, Err code, const char* message = nullptr) noexcept {
  w.begin_object().kv("type", "error");
  if (id >= 0) w.kv("id", id);
  w.kv("code", err_name(code));
  if (message != nullptr) w.kv("message", message);
  w.end_object();
}

inline void encode_accepted(JsonWriter& w, std::int64_t id, std::uint32_t seq) noexcept {
  w.begin_object().kv("type", "accepted").kv("id", id).kv("seq", seq).end_object();
}

inline void encode_ack(JsonWriter& w, std::int64_t id, hubproto::CmdAckStatus status) noexcept {
  w.begin_object().kv("type", "ack").kv("id", id).kv("status", ack_name(status)).end_object();
}

inline void encode_res_ok(JsonWriter& w, std::int64_t id) noexcept {
  w.begin_object().kv("type", "res").kv("id", id).kv("ok", true).end_object();
}

// A successful request that created something: result.index is where it landed.
inline void encode_res_ok_index(JsonWriter& w, std::int64_t id, std::int64_t index) noexcept {
  w.begin_object().kv("type", "res").kv("id", id).kv("ok", true)
      .key("result").begin_object().kv("index", index).end_object().end_object();
}

// A scene was applied: how many commands went out and how many actions were skipped (an entity
// that is gone, read-only now, or a full command queue).
inline void encode_res_ok_counts(JsonWriter& w, std::int64_t id, std::int64_t sent, std::int64_t skipped) noexcept {
  w.begin_object().kv("type", "res").kv("id", id).kv("ok", true)
      .key("result").begin_object().kv("sent", sent).kv("skipped", skipped).end_object().end_object();
}

inline void encode_res_error(JsonWriter& w, std::int64_t id, Err code, const char* message = nullptr) noexcept {
  w.begin_object().kv("type", "res").kv("id", id).kv("ok", false).kv("code", err_name(code));
  if (message != nullptr) w.kv("message", message);
  w.end_object();
}

inline void encode_state(JsonWriter& w, std::uint16_t entity, std::uint8_t attr,
                         const hubproto::Value& v) noexcept {
  w.begin_object().kv("type", "state").kv("entity", static_cast<std::int64_t>(entity))
      .kv("attr", static_cast<std::int64_t>(attr)).key("value");
  write_value(w, v);
  w.end_object();
}

inline void encode_time(JsonWriter& w, std::int64_t time, const char* tz, bool synced) noexcept {
  w.begin_object().kv("type", "time").kv("time", time).kv("tz", tz).kv("synced", synced).end_object();
}

// ---- hub -> client: objects shared by snapshots and events -------------------------

// Attribute and entity objects. `Entity` is core::EntityT<N>.
template <typename Entity>
void write_entity(JsonWriter& w, const Entity& e, const char* name) noexcept {
  w.begin_object().kv("id", static_cast<std::int64_t>(e.entity_id));
  w.key("name");
  if (name != nullptr) w.str(name);
  else w.null();
  w.kv("chip", static_cast<std::int64_t>(e.chip_id)).kv("device", device_name(e.device_type));
  w.key("attrs").begin_array();
  for (std::uint8_t i = 0; i < e.attr_count; ++i) {
    const auto& a = e.attrs[i];
    w.begin_object().kv("id", static_cast<std::int64_t>(a.attr_id));
    char fallback[8];
    const char* n = attr_name(a.attr_id);
    if (n == nullptr) {
      std::snprintf(fallback, sizeof(fallback), "0x%02x", static_cast<unsigned>(a.attr_id));
      n = fallback;
    }
    w.kv("name", n).kv("vt", vt_name(a.value_type));
    w.key("flags").begin_array();
    if (a.flags & hubproto::kFlagReadable) w.str("r");
    if (a.flags & hubproto::kFlagWritable) w.str("w");
    if (a.flags & hubproto::kFlagReportable) w.str("rep");
    w.end_array();
    w.key("value");
    if (a.has_value) write_value(w, a.value);
    else w.null();
    w.end_object();
  }
  w.end_array().end_object();
}

template <typename Entity>
void encode_entity_event(JsonWriter& w, const Entity& e, const char* name) noexcept {
  w.begin_object().kv("type", "entity").kv("op", "add").key("entity");
  write_entity(w, e, name);
  w.end_object();
}

inline void encode_entity_removed(JsonWriter& w, std::uint16_t id) noexcept {
  w.begin_object().kv("type", "entity").kv("op", "remove").kv("id", static_cast<std::int64_t>(id)).end_object();
}

// What the brain knows about a chip, flattened for the wire. Telemetry fields are
// optional (the self entity may not have reported yet).
struct ChipView {
  std::uint8_t id;
  const char* label;
  const char* status;  // "ready" | "silent" | "seen"
  std::uint8_t boot_id;
  std::uint32_t reboots;
  bool has_heap = false, has_uptime = false, has_tx = false, has_crc = false;
  std::uint32_t heap = 0, uptime_s = 0, tx_frames = 0;
  std::uint16_t crc_errors = 0;
};

// Members of a chip object, written into an already-open object.
inline void write_chip_members(JsonWriter& w, const ChipView& c) noexcept {
  w.kv("id", static_cast<std::int64_t>(c.id)).kv("label", c.label).kv("status", c.status)
      .kv("boot_id", static_cast<std::int64_t>(c.boot_id)).kv("reboots", static_cast<std::int64_t>(c.reboots));
  w.key("telemetry").begin_object();
  if (c.has_heap) w.kv("heap", c.heap);
  if (c.has_uptime) w.kv("uptime_s", c.uptime_s);
  if (c.has_tx) w.kv("tx_frames", c.tx_frames);
  if (c.has_crc) w.kv("crc_errors", static_cast<std::uint32_t>(c.crc_errors));
  w.end_object();
}
inline void write_chip(JsonWriter& w, const ChipView& c) noexcept {
  w.begin_object();
  write_chip_members(w, c);
  w.end_object();
}
inline void encode_chip_event(JsonWriter& w, const ChipView& c) noexcept {
  w.begin_object().kv("type", "chip");
  write_chip_members(w, c);
  w.end_object();
}

inline void write_hhmm(JsonWriter& w, unsigned minute_of_day) noexcept {
  char t[8];
  std::snprintf(t, sizeof(t), "%02u:%02u", (minute_of_day / 60) % 24, minute_of_day % 60);
  w.str(t);
}

inline const char* action_name(const hubproto::Value& v) noexcept {
  // "toggle" is stored as U8 2 (a Bool can never hold it) — see the brain's timers.
  if (v.type == hubproto::ValueType::U8 && v.raw[0] == 2) return "toggle";
  return v.as_bool() ? "on" : "off";
}

// `scene` is the name of the scene an action targets (kSceneTarget); the caller resolves the id. A
// scene action is written as {"scene": "name"} instead of entity/attr/value ("?" if it cannot be resolved).
inline void write_rule(JsonWriter& w, std::size_t index, const Rule& r, const char* scene = nullptr) noexcept {
  w.begin_object().kv("index", static_cast<std::int64_t>(index));
  w.key("when").begin_object().kv("entity", static_cast<std::int64_t>(r.trigger_entity))
      .kv("attr", static_cast<std::int64_t>(r.trigger_attr)).kv("op", r.match != nullptr ? "any" : cmp_name(r.op));
  if (r.match == nullptr && r.op != CmpOp::Always) {
    w.key("value");
    write_value(w, r.operand);
  }
  w.end_object();
  w.key("then").begin_object();
  if (r.action_entity == kSceneTarget) {
    w.kv("scene", scene != nullptr ? scene : "?");
  } else {
    w.kv("entity", static_cast<std::int64_t>(r.action_entity)).kv("attr", static_cast<std::int64_t>(r.action_attr)).key("value");
    write_value(w, r.action_value);
  }
  w.end_object();
  if (r.window_start != kNoWindow) {
    w.key("between").begin_object().key("from");
    write_hhmm(w, r.window_start);
    w.key("to");
    write_hhmm(w, r.window_end);
    w.end_object();
  }
  if (r.has_cond) {
    w.key("and").begin_object().kv("entity", static_cast<std::int64_t>(r.cond_entity))
        .kv("attr", static_cast<std::int64_t>(r.cond_attr)).kv("op", cmp_name(r.cond_op)).key("value");
    write_value(w, r.cond_operand);
    w.end_object();
  }
  if (r.cooldown_s != 0) w.kv("cooldown_s", static_cast<std::int64_t>(r.cooldown_s));
  w.end_object();
}

// `S` is ScheduleTable<N>::Schedule (N is irrelevant to the encoding).
template <typename S>
void write_schedule(JsonWriter& w, std::size_t index, const S& s, const char* scene = nullptr) noexcept {
  w.begin_object().kv("index", static_cast<std::int64_t>(index)).key("time");
  write_hhmm(w, s.minute_of_day);
  w.key("days");
  if (s.weekdays == kEveryDay) {
    w.str("daily");
  } else if (s.weekdays == kWeekdays) {
    w.str("weekdays");
  } else if (s.weekdays == kWeekend) {
    w.str("weekends");
  } else {
    static const char* const kDay[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    w.begin_array();
    for (int d = 0; d < 7; ++d) if (s.weekdays & (1u << d)) w.str(kDay[d]);
    w.end_array();
  }
  if (s.entity_id == kSceneTarget) {
    w.kv("scene", scene != nullptr ? scene : "?").end_object();
    return;
  }
  w.kv("entity", static_cast<std::int64_t>(s.entity_id)).kv("attr", static_cast<std::int64_t>(s.attr_id))
      .kv("action", action_name(s.value)).end_object();
}

// `S` is SceneTable<..>::Scene: {"name":"evening","actions":[{"entity":256,"attr":1,"value":true},…]}
template <typename S>
void write_scene(JsonWriter& w, const S& s) noexcept {
  w.begin_object().kv("name", s.name).key("actions").begin_array();
  for (std::size_t i = 0; i < s.count; ++i) {
    const auto& a = s.actions[i];
    w.begin_object().kv("entity", static_cast<std::int64_t>(a.entity_id))
        .kv("attr", static_cast<std::int64_t>(a.attr_id)).key("value");
    write_value(w, a.value);
    w.end_object();
  }
  w.end_array().end_object();
}

struct TimerView {
  std::size_t index;
  std::uint16_t entity;
  std::uint8_t attr;
  hubproto::Value action;
  std::uint32_t left_s;
  const char* scene = nullptr;  // name, when `entity` is kSceneTarget
};

inline void write_timer(JsonWriter& w, const TimerView& t) noexcept {
  w.begin_object().kv("index", static_cast<std::int64_t>(t.index));
  if (t.entity == kSceneTarget) {
    w.kv("scene", t.scene != nullptr ? t.scene : "?");
  } else {
    w.kv("entity", static_cast<std::int64_t>(t.entity)).kv("attr", static_cast<std::int64_t>(t.attr))
        .kv("action", action_name(t.action));
  }
  w.kv("left_s", t.left_s).end_object();
}

// ---- snapshot, chunked to fit a frame --------------------------------------------

// The snapshot can exceed one frame with many entities, so it is sent as parts:
//   {"type":"snapshot","part":N,"entities":[…],"chips":[…],…,"last":bool}
// Part 0 resets the client's lists; every part appends the items it carries; the
// part with "last":true completes it. A Source provides, per section, a count and
// a writer for item i (the brain adapts its registry, rules, … to this):
//   size_t count(Section) ; void write(Section, size_t i, JsonWriter&)
enum class Section : std::uint8_t { Entities, Chips, Rules, Schedules, Timers, Scenes };
inline constexpr int kSections = 6;

inline const char* section_key(Section s) noexcept {
  switch (s) {
    case Section::Entities: return "entities";
    case Section::Chips: return "chips";
    case Section::Rules: return "rules";
    case Section::Schedules: return "schedules";
    case Section::Timers: return "timers";
    case Section::Scenes: return "scenes";
  }
  return "";
}

struct SnapshotCursor {
  int section = 0;
  std::size_t item = 0;
  std::uint32_t part = 0;
  bool done() const noexcept { return section >= kSections; }
};

// Write the next part into `w` (a fresh writer over a kMaxFrame buffer). Items are
// whole, never split; the part is closed once the writer passes `soft_limit` bytes.
// Returns false only if even an empty part did not fit (cannot happen with sane sizes).
template <typename Source>
bool encode_snapshot_part(SnapshotCursor& c, Source& src, JsonWriter& w, std::size_t soft_limit) noexcept {
  w.begin_object().kv("type", "snapshot").kv("part", static_cast<std::int64_t>(c.part));
  bool full = false;
  std::size_t written = 0;  // items in this part: at least one, so every part makes progress
  while (!c.done() && !full) {
    const Section sec = static_cast<Section>(c.section);
    const std::size_t n = src.count(sec);
    if (c.item >= n && c.item != 0) {  // section finished earlier
      ++c.section;
      c.item = 0;
      continue;
    }
    w.key(section_key(sec)).begin_array();
    while (c.item < n) {
      if (written > 0 && w.size() > soft_limit) {
        full = true;
        break;
      }
      src.write(sec, c.item, w);
      ++c.item;
      ++written;
    }
    w.end_array();
    if (c.item >= n) {
      ++c.section;
      c.item = 0;
    }
  }
  w.kv("last", c.done());
  w.end_object();
  ++c.part;
  return w.ok();
}

}  // namespace core::app
