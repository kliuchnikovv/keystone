// Management operations of the app protocol (`req` / `res`, docs/app-protocol.md §5):
// names, rules, schedules and timers, plus the firmware update, the access card
// (token, certificate, relay) and the hub's own actions. Each operation parses its
// `args` JSON, checks it against the registry the same way the console does (and a
// little stricter: writable targets, value types and ranges), applies it to the
// container and reports an Outcome. Persisting to NVS and re-broadcasting stay with
// the caller, so this is portable and host-tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "core/app_protocol.hpp"
#include "core/civil_time.hpp"
#include "core/entity_registry.hpp"  // AttrState
#include "core/scene_table.hpp"

namespace core::app {

struct Outcome {
  bool ok = true;
  Err err = Err::Internal;
  char msg[40] = {};
  bool has_index = false;
  std::int64_t index = 0;
  bool has_counts = false;  // a scene was applied: sent / skipped
  std::int64_t sent = 0;
  std::int64_t skipped = 0;

  static Outcome fail(Err e, const char* m) {
    Outcome o;
    o.ok = false;
    o.err = e;
    std::strncpy(o.msg, m, sizeof(o.msg) - 1);
    return o;
  }
  static Outcome done() { return Outcome{}; }
  static Outcome counts(std::int64_t sent, std::int64_t skipped) {
    Outcome o;
    o.has_counts = true;
    o.sent = sent;
    o.skipped = skipped;
    return o;
  }
  static Outcome at(std::int64_t i) {
    Outcome o;
    o.has_index = true;
    o.index = i;
    return o;
  }
};

inline constexpr std::uint8_t kOnOffAttr = 0x01;
inline constexpr std::size_t kMaxNameChars = 23;  // EntityNames::MaxNameLen (24) minus the NUL
// A device name: 15 characters (a person can read it at a glance) and 30 bytes
// (the storage keeps 32, the NUL included).
inline constexpr std::size_t kMaxEntityNameChars = 15;
inline constexpr std::size_t kMaxEntityNameBytes = 30;

namespace detail {

template <typename Reader>
bool get_int(const Reader& r, int obj, const char* key, std::int64_t& out) {
  return r.get_i64(r.find(obj, key), out);
}

inline bool valid_entity_name(const char* s) {
  if (!s || s[0] == '\0') return false;
  // A name a person reads out loud: letters (any script), digits, spaces and a
  // little punctuation. At most 15 characters — a label, not a sentence — and at
  // most 30 bytes, so it still fits the 32-byte field it is stored in.
  std::size_t chars = 0;
  std::size_t bytes = 0;
  bool has_letter = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p) {
    const unsigned char c = *p;
    if (++bytes > kMaxEntityNameBytes) return false;
    if ((c & 0xC0) == 0x80) continue;  // a continuation byte: part of the character above
    // A new character starts here.
    if (++chars > kMaxEntityNameChars) return false;
    if (c < 0x20 || c == 0x7F) return false;  // control characters
    if (c < 0x80) {
      const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      if (letter) has_letter = true;
      const bool ok = letter || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
                       c == '.' || c == ',' || c == '&' || c == '\'' || c == '(' || c == ')';
      if (!ok) return false;
    } else {
      has_letter = true;  // any non-ASCII letter (Cyrillic, for one)
    }
  }
  if (!has_letter) return false;  // a name of digits would be read as an id
  // A name that reads as a number would be taken for an id at the console.
  char* end = nullptr;
  const unsigned long as_number = std::strtoul(s, &end, 0);
  (void)as_number;
  if (end != s && *end == '\0') return false;
  const std::size_t n = std::strlen(s);
  if (s[0] == ' ' || s[n - 1] == ' ') return false;  // no stray spaces at the edges
  return true;
}

// "HH:MM", exactly five characters.
inline bool parse_hhmm(const char* s, int& hour, int& minute) {
  if (std::strlen(s) != 5 || s[2] != ':') return false;
  for (int i : {0, 1, 3, 4}) if (s[i] < '0' || s[i] > '9') return false;
  hour = (s[0] - '0') * 10 + (s[1] - '0');
  minute = (s[3] - '0') * 10 + (s[4] - '0');
  return hour <= 23 && minute <= 59;
}

inline bool parse_cmp(const char* s, CmpOp& op) {
  if (!std::strcmp(s, "any")) op = CmpOp::Always;
  else if (!std::strcmp(s, "==")) op = CmpOp::Eq;
  else if (!std::strcmp(s, "!=")) op = CmpOp::Ne;
  else if (!std::strcmp(s, "<")) op = CmpOp::Lt;
  else if (!std::strcmp(s, "<=")) op = CmpOp::Le;
  else if (!std::strcmp(s, ">")) op = CmpOp::Gt;
  else if (!std::strcmp(s, ">=")) op = CmpOp::Ge;
  else return false;
  return true;
}

// "daily" | "weekdays" | "weekends" | ["mon", …]; a missing field means daily.
template <typename Reader>
bool parse_days(const Reader& r, int tok, std::uint8_t& mask) {
  if (tok < 0) {
    mask = kEveryDay;
    return true;
  }
  char s[12];
  if (r.is(tok, Reader::Type::String)) {
    if (!r.get_string(tok, s, sizeof(s))) return false;
    if (!std::strcmp(s, "daily")) mask = kEveryDay;
    else if (!std::strcmp(s, "weekdays")) mask = kWeekdays;
    else if (!std::strcmp(s, "weekends")) mask = kWeekend;
    else return false;
    return true;
  }
  if (!r.is(tok, Reader::Type::Array) || r.count(tok) == 0) return false;
  static const char* const kDay[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
  std::uint8_t m = 0;
  for (int i = 0; i < r.count(tok); ++i) {
    if (!r.get_string(r.at(tok, i), s, sizeof(s))) return false;
    int found = -1;
    for (int d = 0; d < 7; ++d) if (!std::strcmp(s, kDay[d])) found = d;
    if (found < 0) return false;
    m = static_cast<std::uint8_t>(m | (1u << found));
  }
  mask = m;
  return true;
}

// The on/off action of schedules and timers: on, off, or toggle (U8 2, as on the console).
template <typename Reader>
bool parse_action(const Reader& r, int obj, hubproto::Value& out) {
  char s[10];
  if (!r.get_string(r.find(obj, "action"), s, sizeof(s))) return false;
  if (!std::strcmp(s, "on")) out = hubproto::Value::make_bool(true);
  else if (!std::strcmp(s, "off")) out = hubproto::Value::make_bool(false);
  else if (!std::strcmp(s, "toggle")) out = hubproto::Value::make_u8(2);
  else return false;
  return true;
}

// Resolve {"entity": n, "attr": m} inside `obj` against the registry.
template <typename Reg, typename Reader>
bool lookup(const Reg& reg, const Reader& r, int obj, bool allow_chip, std::uint16_t& entity,
            std::uint8_t& attr, const AttrState*& state, Outcome& fail, const char* where) {
  char what[40];
  auto bad = [&](Err e, const char* detail) {
    std::snprintf(what, sizeof(what), "%s: %s", where, detail);
    fail = Outcome::fail(e, what);
    return false;
  };
  std::int64_t e = 0, a = 0;
  if (!get_int(r, obj, "entity", e) || e < 0 || e > 0xFFFF) return bad(Err::BadArgs, "entity");
  if (!get_int(r, obj, "attr", a) || a < 0 || a > 0xFF) return bad(Err::BadArgs, "attr");
  const auto* ent = reg.find(static_cast<std::uint16_t>(e));
  if (ent == nullptr || (!allow_chip && ent->device_type == hubproto::DeviceType::Chip)) {
    return bad(Err::NotFound, "no such entity");
  }
  state = nullptr;
  for (std::uint8_t i = 0; i < ent->attr_count; ++i) {
    if (ent->attrs[i].attr_id == a) state = &ent->attrs[i];
  }
  if (state == nullptr) return bad(Err::NotFound, "no such attribute");
  entity = static_cast<std::uint16_t>(e);
  attr = static_cast<std::uint8_t>(a);
  return true;
}

// The entity's on/off attribute, required to be a writable bool (schedules, timers).
template <typename Reg, typename Reader>
bool on_off_target(const Reg& reg, const Reader& r, int args, std::uint16_t& entity, Outcome& fail) {
  std::int64_t e = 0;
  if (!get_int(r, args, "entity", e) || e < 0 || e > 0xFFFF) {
    fail = Outcome::fail(Err::BadArgs, "entity");
    return false;
  }
  const auto* ent = reg.find(static_cast<std::uint16_t>(e));
  if (ent == nullptr || ent->device_type == hubproto::DeviceType::Chip) {
    fail = Outcome::fail(Err::NotFound, "no such entity");
    return false;
  }
  for (std::uint8_t i = 0; i < ent->attr_count; ++i) {
    const AttrState& a = ent->attrs[i];
    if (a.attr_id == kOnOffAttr && a.value_type == hubproto::ValueType::Bool &&
        (a.flags & hubproto::kFlagWritable) != 0) {
      entity = static_cast<std::uint16_t>(e);
      return true;
    }
  }
  fail = Outcome::fail(Err::BadArgs, "entity has no on/off switch");
  return false;
}

// What a schedule or timer does: either {entity, action} (on/off/toggle of a switch) or {scene: "name"}
// (apply that scene). A scene is stored as the reserved target (kSceneTarget, attr = scene id).
template <typename Reg, typename Scenes, typename Reader>
bool action_target(const Reg& reg, const Scenes& scenes, const Reader& r, int args, std::uint16_t& entity,
                   std::uint8_t& attr, hubproto::Value& value, Outcome& fail) {
  const int sc = r.find(args, "scene");
  if (sc < 0) {
    if (!parse_action(r, args, value)) {
      fail = Outcome::fail(Err::BadArgs, "action: on, off or toggle (or scene)");
      return false;
    }
    attr = kOnOffAttr;
    return on_off_target(reg, r, args, entity, fail);
  }
  if (r.find(args, "entity") >= 0 || r.find(args, "action") >= 0) {
    fail = Outcome::fail(Err::BadArgs, "scene excludes entity/action");
    return false;
  }
  char name[kMaxNameChars + 2];
  if (!r.get_string(sc, name, sizeof(name)) || !core::valid_scene_name(name)) {
    fail = Outcome::fail(Err::BadArgs, "scene: a scene name");
    return false;
  }
  const auto* found = scenes.find(name);
  if (found == nullptr) {
    fail = Outcome::fail(Err::NotFound, "no such scene");
    return false;
  }
  entity = core::kSceneTarget;
  attr = found->id;
  value = hubproto::Value::make_bool(true);
  return true;
}

}  // namespace detail

// ---- names ---------------------------------------------------------------------------

template <typename Reg, typename Reader>
Outcome name_set(Reg& reg, const Reader& r, int args) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  std::int64_t e = 0;
  if (!detail::get_int(r, args, "entity", e) || e < 0 || e > 0xFFFF) return Outcome::fail(Err::BadArgs, "entity");
  const auto id = static_cast<std::uint16_t>(e);
  const auto* ent = reg.find(id);
  if (ent == nullptr || ent->device_type == hubproto::DeviceType::Chip) return Outcome::fail(Err::NotFound, "no such entity");
  char name[kMaxEntityNameBytes + 2];
  if (!r.get_string(r.find(args, "name"), name, sizeof(name))) return Outcome::fail(Err::BadArgs, "name");
  if (!detail::valid_entity_name(name)) {
    return Outcome::fail(Err::BadArgs, "name: up to 15 characters, letters and spaces");
  }
  const auto* other = reg.find_by_name(name);
  if (other != nullptr && other->entity_id != id) return Outcome::fail(Err::BadArgs, "name in use");
  if (!reg.set_name(id, name)) return Outcome::fail(Err::TableFull, "no room for more names");
  return Outcome::done();
}

template <typename Reg, typename Reader>
Outcome name_clear(Reg& reg, const Reader& r, int args) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  std::int64_t e = 0;
  if (!detail::get_int(r, args, "entity", e) || e < 0 || e > 0xFFFF) return Outcome::fail(Err::BadArgs, "entity");
  const auto id = static_cast<std::uint16_t>(e);
  if (reg.find(id) == nullptr) return Outcome::fail(Err::NotFound, "no such entity");
  reg.clear_name(id);
  return Outcome::done();
}

// ---- history -------------------------------------------------------------------------

// `history`: {"before": seq, "limit": n, "entity": id}, every member optional. `before` pages back
// (events strictly older than it; 0 or absent = from the newest), `limit` is 1..kHistoryPage, `entity`
// keeps only events about that entity (also one that has since been removed: its history stays).
struct HistoryQuery {
  std::uint32_t before = 0;
  std::size_t limit = kHistoryPage;
  bool entity_only = false;
  std::uint16_t entity = 0;
};

template <typename Reader>
Outcome history_parse(const Reader& r, int args, HistoryQuery& q) {
  q = HistoryQuery{};
  if (args < 0) return Outcome::done();
  std::int64_t v = 0;
  if (r.find(args, "before") >= 0) {
    if (!detail::get_int(r, args, "before", v) || v < 0 || v > 0xFFFFFFFFll) return Outcome::fail(Err::BadArgs, "before");
    q.before = static_cast<std::uint32_t>(v);
  }
  if (r.find(args, "limit") >= 0) {
    if (!detail::get_int(r, args, "limit", v) || v < 1 || v > static_cast<std::int64_t>(kHistoryPage)) {
      return Outcome::fail(Err::BadArgs, "limit: 1..24");
    }
    q.limit = static_cast<std::size_t>(v);
  }
  if (r.find(args, "entity") >= 0) {
    if (!detail::get_int(r, args, "entity", v) || v < 0 || v > 0xFFFF) return Outcome::fail(Err::BadArgs, "entity");
    q.entity_only = true;
    q.entity = static_cast<std::uint16_t>(v);
  }
  return Outcome::done();
}

// ---- rules ---------------------------------------------------------------------------

template <typename Reg, typename Scenes, typename Engine, typename Reader>
Outcome rule_add(Reg& reg, const Scenes& scenes, Engine& rules, const Reader& r, int args) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  Outcome fail;
  Rule ru{};
  ru.match = nullptr;

  // when: any attribute (a chip's own telemetry included), any comparison.
  const int when = r.find(args, "when");
  const int then = r.find(args, "then");
  if (when < 0 || then < 0) return Outcome::fail(Err::BadArgs, "when/then required");
  const AttrState* ws = nullptr;
  if (!detail::lookup(reg, r, when, true, ru.trigger_entity, ru.trigger_attr, ws, fail, "when")) return fail;
  char opname[6];
  if (!r.get_string(r.find(when, "op"), opname, sizeof(opname)) || !detail::parse_cmp(opname, ru.op)) {
    return Outcome::fail(Err::BadArgs, "when.op");
  }
  if (ru.op == CmpOp::Always) {
    ru.operand.type = ws->value_type;
    ru.operand.len = static_cast<std::uint8_t>(hubproto::value_size(ws->value_type));
  } else if (!to_value(read_scalar(r, r.find(when, "value")), ws->value_type, ru.operand)) {
    return Outcome::fail(Err::BadArgs, "when.value does not fit the attribute");
  }

  // then: either {scene: "name"} or a writable attribute of a real device.
  if (r.find(then, "scene") >= 0) {
    if (r.find(then, "entity") >= 0 || r.find(then, "attr") >= 0 || r.find(then, "value") >= 0) {
      return Outcome::fail(Err::BadArgs, "then: scene excludes entity/attr/value");
    }
    if (!detail::action_target(reg, scenes, r, then, ru.action_entity, ru.action_attr, ru.action_value, fail)) return fail;
  } else {
    const AttrState* ts = nullptr;
    if (!detail::lookup(reg, r, then, false, ru.action_entity, ru.action_attr, ts, fail, "then")) return fail;
    if ((ts->flags & hubproto::kFlagWritable) == 0) return Outcome::fail(Err::ReadOnly, "then: attribute is read-only");
    if (!to_value(read_scalar(r, r.find(then, "value")), ts->value_type, ru.action_value)) {
      return Outcome::fail(Err::BadArgs, "then.value does not fit the attribute");
    }
  }

  const int between = r.find(args, "between");
  if (between >= 0) {
    char from[16], to[16];
    std::uint16_t p1 = 0, p2 = 0;
    if (!r.get_string(r.find(between, "from"), from, sizeof(from)) || !parse_time_point(from, p1) ||
        !r.get_string(r.find(between, "to"), to, sizeof(to)) || !parse_time_point(to, p2) || p1 == p2) {
      return Outcome::fail(Err::BadArgs, "between: HH:MM or sunrise/sunset[+-min], two different times");
    }
    ru.window_start = p1;
    ru.window_end = p2;
  }

  const int cond = r.find(args, "and");
  if (cond >= 0) {
    const AttrState* cs = nullptr;
    if (!detail::lookup(reg, r, cond, true, ru.cond_entity, ru.cond_attr, cs, fail, "and")) return fail;
    char cop[6];
    if (!r.get_string(r.find(cond, "op"), cop, sizeof(cop)) || !detail::parse_cmp(cop, ru.cond_op) ||
        ru.cond_op == CmpOp::Always) {
      return Outcome::fail(Err::BadArgs, "and.op");
    }
    if (!to_value(read_scalar(r, r.find(cond, "value")), cs->value_type, ru.cond_operand)) {
      return Outcome::fail(Err::BadArgs, "and.value does not fit the attribute");
    }
    ru.has_cond = true;
  }

  const int cd = r.find(args, "cooldown_s");
  if (cd >= 0) {
    std::int64_t s = 0;
    if (!r.get_i64(cd, s) || s < 1 || s > 65535) return Outcome::fail(Err::BadArgs, "cooldown_s: 1..65535");
    ru.cooldown_s = static_cast<std::uint16_t>(s);
  }

  if (!rules.add_rule(ru)) return Outcome::fail(Err::TableFull, "rule table is full");
  return Outcome::at(static_cast<std::int64_t>(rules.rule_count()) - 1);
}

template <typename Engine, typename Reader>
Outcome rule_del(Engine& rules, const Reader& r, int args) {
  std::int64_t i = 0;
  if (args < 0 || !detail::get_int(r, args, "index", i) || i < 0) return Outcome::fail(Err::BadArgs, "index");
  if (!rules.remove_rule(static_cast<std::size_t>(i))) return Outcome::fail(Err::NotFound, "no such rule");
  return Outcome::done();
}

// ---- schedules -----------------------------------------------------------------------

// `date` ("YYYY-MM-DD", local) makes a one-off: it runs on that day and is removed (then `days` must be
// absent). `today` is the hub's local civil_day_number, 0 while its clock is unknown (a date in the past
// is then not noticed here: the table drops it the next day).
template <typename Reg, typename Scenes, typename Sched, typename Reader>
Outcome schedule_add(Reg& reg, const Scenes& scenes, Sched& sched, const Reader& r, int args,
                     std::int32_t today = 0) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  Outcome fail;
  char t[16];
  std::uint16_t point = 0;
  if (!r.get_string(r.find(args, "time"), t, sizeof(t)) || !parse_time_point(t, point)) {
    return Outcome::fail(Err::BadArgs, "time: HH:MM or sunrise/sunset[+-min]");
  }
  std::uint8_t days = kEveryDay;
  if (!detail::parse_days(r, r.find(args, "days"), days)) return Outcome::fail(Err::BadArgs, "days");
  std::int32_t once = 0;
  const int date_tok = r.find(args, "date");
  if (date_tok >= 0) {
    char d[16];
    if (r.find(args, "days") >= 0) return Outcome::fail(Err::BadArgs, "date and days exclude each other");
    if (!r.get_string(date_tok, d, sizeof(d)) || !parse_date(d, once)) return Outcome::fail(Err::BadArgs, "date: YYYY-MM-DD");
    if (today != 0 && once < today) return Outcome::fail(Err::BadArgs, "date is in the past");
  }
  hubproto::Value action{};
  std::uint16_t entity = 0;
  std::uint8_t attr = 0;
  if (!detail::action_target(reg, scenes, r, args, entity, attr, action, fail)) return fail;
  const int slot = sched.add_point(entity, attr, action, point, days, once);
  if (slot < 0) return Outcome::fail(Err::TableFull, "schedule table is full");
  return Outcome::at(slot);
}

template <typename Sched, typename Reader>
Outcome schedule_del(Sched& sched, const Reader& r, int args) {
  std::int64_t i = 0;
  if (args < 0 || !detail::get_int(r, args, "index", i) || i < 0) return Outcome::fail(Err::BadArgs, "index");
  if (!sched.remove(static_cast<std::size_t>(i))) return Outcome::fail(Err::NotFound, "no such schedule");
  return Outcome::done();
}

// ---- timers --------------------------------------------------------------------------

inline constexpr std::int64_t kMaxTimerDelayS = 7 * 24 * 3600;

template <typename Reg, typename Scenes, typename Timers, typename Reader>
Outcome timer_add(Reg& reg, const Scenes& scenes, Timers& timers, const Reader& r, int args, std::uint32_t now_ms,
                  std::uint64_t now_epoch_s) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  Outcome fail;
  std::int64_t delay = 0;
  if (!detail::get_int(r, args, "delay_s", delay) || delay < 1 || delay > kMaxTimerDelayS) {
    return Outcome::fail(Err::BadArgs, "delay_s: 1..604800");
  }
  hubproto::Value action{};
  std::uint16_t entity = 0;
  std::uint8_t attr = 0;
  if (!detail::action_target(reg, scenes, r, args, entity, attr, action, fail)) return fail;
  const int slot = timers.schedule(entity, attr, action, static_cast<std::uint32_t>(delay) * 1000u,
                                   now_ms, now_epoch_s);
  if (slot < 0) return Outcome::fail(Err::TableFull, "timer table is full");
  return Outcome::at(slot);
}

template <typename Timers, typename Reader>
Outcome timer_cancel(Timers& timers, const Reader& r, int args) {
  std::int64_t i = 0;
  if (args < 0 || !detail::get_int(r, args, "index", i) || i < 0) return Outcome::fail(Err::BadArgs, "index");
  if (!timers.cancel(static_cast<std::size_t>(i))) return Outcome::fail(Err::NotFound, "no such timer");
  return Outcome::done();
}

// ---- scenes -----------------------------------------------------------------------------

namespace detail {
template <typename Reader>
bool scene_name(const Reader& r, int args, char* out, std::size_t cap, Outcome& fail) {
  if (args < 0 || !r.get_string(r.find(args, "name"), out, cap) || !core::valid_scene_name(out)) {
    fail = Outcome::fail(Err::BadArgs, "name: a-z 0-9 _, starts with a letter, max 23");
    return false;
  }
  return true;
}
}  // namespace detail

/** `scene.put {name, actions:[{entity, attr, value}…]}`: create or replace. Every action must name a
 *  writable attribute of a real device with a value that fits it; one attribute at most once. */
template <typename Reg, typename Scenes, typename Reader>
Outcome scene_put(const Reg& reg, Scenes& scenes, const Reader& r, int args) {
  Outcome fail;
  char name[kMaxNameChars + 2];
  if (!detail::scene_name(r, args, name, sizeof(name), fail)) return fail;
  const int list = r.find(args, "actions");
  if (list < 0 || !r.is(list, Reader::Type::Array) || r.count(list) < 1 ||
      static_cast<std::size_t>(r.count(list)) > Scenes::kMaxActions) {
    return Outcome::fail(Err::BadArgs, "actions: 1..8 items");
  }
  typename Scenes::Action acts[Scenes::kMaxActions];
  const std::size_t n = static_cast<std::size_t>(r.count(list));
  for (std::size_t i = 0; i < n; ++i) {
    const int item = r.at(list, static_cast<int>(i));
    char where[24];
    std::snprintf(where, sizeof(where), "actions[%u]", static_cast<unsigned>(i));
    const AttrState* st = nullptr;
    if (!detail::lookup(reg, r, item, false, acts[i].entity_id, acts[i].attr_id, st, fail, where)) return fail;
    if ((st->flags & hubproto::kFlagWritable) == 0) return Outcome::fail(Err::ReadOnly, "action: attribute is read-only");
    if (!to_value(read_scalar(r, r.find(item, "value")), st->value_type, acts[i].value)) {
      return Outcome::fail(Err::BadArgs, "action value does not fit the attribute");
    }
    for (std::size_t k = 0; k < i; ++k) {
      if (acts[k].entity_id == acts[i].entity_id && acts[k].attr_id == acts[i].attr_id) {
        return Outcome::fail(Err::BadArgs, "the same attribute twice");
      }
    }
  }
  if (scenes.put(name, acts, n) < 0) return Outcome::fail(Err::TableFull, "scene table is full");
  return Outcome::done();
}

/** `scene.del {name}`. `in_use(id)` says whether a schedule, timer or rule applies the scene: such a
 *  scene is kept (deleting it would leave them pointing at nothing). */
template <typename Scenes, typename Reader, typename InUse>
Outcome scene_del(Scenes& scenes, const Reader& r, int args, InUse&& in_use) {
  Outcome fail;
  char name[kMaxNameChars + 2];
  if (!detail::scene_name(r, args, name, sizeof(name), fail)) return fail;
  const auto* sc = scenes.find(name);
  if (sc == nullptr) return Outcome::fail(Err::NotFound, "no such scene");
  if (in_use(sc->id)) return Outcome::fail(Err::BadArgs, "scene is used by a schedule, timer or rule");
  scenes.remove(name);
  return Outcome::done();
}

/** Apply a scene by name (used by the app request and the console): an action whose target has since
 *  disappeared, become read-only or changed type is skipped. */
template <typename Reg, typename Scenes, typename Send>
typename Scenes::Applied apply_scene(const Reg& reg, const Scenes& scenes, const char* name, Send&& send) {
  return scenes.apply(name, [&](const typename Scenes::Action& a) {
    const auto* ent = reg.find(a.entity_id);
    if (ent == nullptr || ent->device_type == hubproto::DeviceType::Chip) return false;
    for (std::uint8_t i = 0; i < ent->attr_count; ++i) {
      const AttrState& st = ent->attrs[i];
      if (st.attr_id != a.attr_id) continue;
      if ((st.flags & hubproto::kFlagWritable) == 0 || st.value_type != a.value.type) return false;
      return send(a.entity_id, a.attr_id, a.value);
    }
    return false;
  });
}

/** `scene.apply {name}`: send every action through `send(entity, attr, value) -> bool`. An action whose
 *  target has since disappeared, become read-only or changed type is skipped, not an error. */
template <typename Reg, typename Scenes, typename Reader, typename Send>
Outcome scene_apply(const Reg& reg, const Scenes& scenes, const Reader& r, int args, Send&& send) {
  Outcome fail;
  char name[kMaxNameChars + 2];
  if (!detail::scene_name(r, args, name, sizeof(name), fail)) return fail;
  const auto res = apply_scene(reg, scenes, name, send);
  if (!res.found) return Outcome::fail(Err::NotFound, "no such scene");
  return Outcome::counts(static_cast<std::int64_t>(res.sent), static_cast<std::int64_t>(res.skipped));
}

// ---- location --------------------------------------------------------------------------------

/** `location.set {lat, lon}` (degrees, north/east positive). The caller stores it. */
template <typename Reader>
Outcome location_parse(const Reader& r, int args, double& lat, double& lon) {
  if (args < 0 || !r.get_double(r.find(args, "lat"), lat) || !r.get_double(r.find(args, "lon"), lon)) {
    return Outcome::fail(Err::BadArgs, "lat and lon: numbers");
  }
  if (!(lat >= -90.0 && lat <= 90.0) || !(lon >= -180.0 && lon <= 180.0)) {
    return Outcome::fail(Err::BadArgs, "lat -90..90, lon -180..180");
  }
  return Outcome::done();
}

// ---- Wi-Fi ------------------------------------------------------------------------------------

/** `wifi.set {ssid, password}` ("" = an open network). The caller applies it as a trial (WifiTrial). */
template <typename Reader>
Outcome wifi_parse(const Reader& r, int args, char* ssid, std::size_t ssid_cap, char* password, std::size_t pass_cap) {
  if (args < 0 || !r.get_string(r.find(args, "ssid"), ssid, ssid_cap)) return Outcome::fail(Err::BadArgs, "ssid");
  if (!r.get_string(r.find(args, "password"), password, pass_cap)) return Outcome::fail(Err::BadArgs, "password");
  const char* why = nullptr;
  if (!core::wifi_credentials_valid(ssid, password, &why)) return Outcome::fail(Err::BadArgs, why != nullptr ? why : "credentials");
  return Outcome::done();
}

/** A token the hub accepts: 8..kMaxToken printable ASCII characters without spaces (what `webtoken` checks). */
inline bool token_valid(const char* t) noexcept {
  if (t == nullptr) return false;
  const std::size_t n = std::strlen(t);
  if (n < 8 || n > kMaxToken) return false;
  for (std::size_t i = 0; i < n; ++i) {
    if (t[i] < 0x21 || t[i] > 0x7E) return false;
  }
  return true;
}

/** `setup.apply {ssid, password, token}`: the home network and the app's long-term token, in one request,
 *  from a client that authenticated with the setup code. The caller saves both and applies the network. */
template <typename Reader>
Outcome setup_parse(const Reader& r, int args, char* ssid, std::size_t ssid_cap, char* password, std::size_t pass_cap,
                    char* token, std::size_t token_cap) {
  const Outcome wifi = wifi_parse(r, args, ssid, ssid_cap, password, pass_cap);
  if (!wifi.ok) return wifi;
  if (!r.get_string(r.find(args, "token"), token, token_cap) || !token_valid(token)) {
    return Outcome::fail(Err::BadArgs, "token: 8..64 printable characters");
  }
  return Outcome::done();
}

// ---- Zigbee chip operations: pair, unpair, identify ----------------------------------------
//
// These do not change anything on the brain: they validate the request and say which
// chip-meta command to send (a COMMAND to the chip's self entity, attr = the op, u16 = the
// argument; mirrored in zigbee-c6 handle_meta). The caller publishes it, exactly as the
// console does. Fire-and-forget: the answer says "sent", the result shows up as an entity
// appearing (pair) or going away (unpair), or a device blinking (identify).

inline constexpr std::uint8_t kMetaPair = 0x01;
inline constexpr std::uint8_t kMetaUnpair = 0x02;
inline constexpr std::uint8_t kMetaIdentify = 0x03;
inline constexpr std::int64_t kDefaultPairSeconds = 180;
inline constexpr std::int64_t kMaxPairSeconds = 254;  // ZigBee permit-join is one byte, 255 = forever

struct ChipMeta {
  Outcome out;
  std::uint8_t chip = 0;
  std::uint8_t attr = 0;
  std::uint16_t arg = 0;
};

/** `pair {seconds?}`: open the Zigbee network for joining (1..254 s, default 180). */
template <typename Reader>
ChipMeta pair_meta(const Reader& r, int args, std::uint8_t zigbee_chip) {
  std::int64_t sec = kDefaultPairSeconds;
  if (args >= 0 && r.find(args, "seconds") >= 0) {
    if (!detail::get_int(r, args, "seconds", sec) || sec < 1 || sec > kMaxPairSeconds) {
      return {Outcome::fail(Err::BadArgs, "seconds: 1..254"), 0, 0, 0};
    }
  }
  return {Outcome::done(), zigbee_chip, kMetaPair, static_cast<std::uint16_t>(sec)};
}

/** `unpair {entity}` / `identify {entity}`: a device on the Zigbee chip (not a chip, not a Matter mirror). */
template <typename Reg, typename Reader>
ChipMeta device_meta(const Reg& reg, const Reader& r, int args, std::uint8_t attr, std::uint8_t zigbee_chip) {
  std::int64_t e = 0;
  if (args < 0 || !detail::get_int(r, args, "entity", e) || e < 0 || e > 0xFFFF) {
    return {Outcome::fail(Err::BadArgs, "entity"), 0, 0, 0};
  }
  const auto* ent = reg.find(static_cast<std::uint16_t>(e));
  if (ent == nullptr || ent->device_type == hubproto::DeviceType::Chip) {
    return {Outcome::fail(Err::NotFound, "no such entity"), 0, 0, 0};
  }
  if (ent->chip_id != zigbee_chip) return {Outcome::fail(Err::BadArgs, "not a Zigbee device"), 0, 0, 0};
  return {Outcome::done(), zigbee_chip, attr, static_cast<std::uint16_t>(e)};
}

// ---- firmware update (OTA) -----------------------------------------------------------------
//
// `ota.start` and `ota.status`: updating a co-processor over the UART link (docs/ota.md).
// The hub downloads the image and streams it to the chip; the app only asks for it and
// watches the progress.

inline constexpr std::uint8_t kOtaChipZigbee = 0x01;  // the Zigbee C6
inline constexpr std::uint8_t kOtaChipThread = 0x02;  // the Thread/Matter C6
inline constexpr std::size_t kMaxOtaUrlChars = 255;
inline constexpr std::size_t kSha256HexChars = 64;

/** The chip an update may target: 1 (Zigbee) or 2 (Thread). */
inline bool ota_chip_valid(std::int64_t chip) noexcept {
  return chip == static_cast<std::int64_t>(kOtaChipZigbee) || chip == static_cast<std::int64_t>(kOtaChipThread);
}

/** The image must come over HTTP(S). */
inline bool ota_url_valid(const char* url) noexcept {
  if (url == nullptr) return false;
  const std::size_t n = std::strlen(url);
  if (n == 0 || n > kMaxOtaUrlChars) return false;
  return std::strncmp(url, "http://", 7) == 0 || std::strncmp(url, "https://", 8) == 0;
}

/** The digest of the image: exactly 64 hex characters. */
inline bool sha256_hex_valid(const char* s) noexcept {
  if (s == nullptr) return false;
  if (std::strlen(s) != kSha256HexChars) return false;
  for (std::size_t i = 0; i < kSha256HexChars; ++i) {
    const char c = s[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

struct OtaRequest {
  std::uint8_t chip = 0;
  char url[kMaxOtaUrlChars + 1] = {};
  char sha256[kSha256HexChars + 1] = {};
};

/** `ota.start {chip, url, sha256}`: what to update and with which image. The caller starts the
 *  job and answers busy if one is already running. */
template <typename Reader>
Outcome ota_start_parse(const Reader& r, int args, OtaRequest& out) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  std::int64_t chip = 0;
  if (!detail::get_int(r, args, "chip", chip) || !ota_chip_valid(chip)) {
    return Outcome::fail(Err::BadArgs, "chip: 1 (zigbee) or 2 (thread)");
  }
  out.chip = static_cast<std::uint8_t>(chip);
  if (!r.get_string(r.find(args, "url"), out.url, sizeof(out.url)) || !ota_url_valid(out.url)) {
    return Outcome::fail(Err::BadArgs, "url: http:// or https://");
  }
  if (!r.get_string(r.find(args, "sha256"), out.sha256, sizeof(out.sha256)) || !sha256_hex_valid(out.sha256)) {
    return Outcome::fail(Err::BadArgs, "sha256: 64 hex characters");
  }
  return Outcome::done();
}

// ---- access: the app token and the relay ----------------------------------------------------

inline constexpr std::size_t kMaxRelayUrlChars = 255;
inline constexpr std::size_t kMaxRelaySecretChars = 63;

/** The relay is reached over a WebSocket (the console takes wss://relay.example.com:9090/tunnel/my-hub);
 *  http(s) is accepted too, for a relay that fronts it. */
inline bool relay_url_valid(const char* url) noexcept {
  if (url == nullptr) return false;
  const std::size_t n = std::strlen(url);
  if (n == 0 || n > kMaxRelayUrlChars) return false;
  return std::strncmp(url, "wss://", 6) == 0 || std::strncmp(url, "ws://", 5) == 0 ||
         std::strncmp(url, "https://", 8) == 0 || std::strncmp(url, "http://", 7) == 0;
}

struct RelayRequest {
  char url[kMaxRelayUrlChars + 1] = {};
  char secret[kMaxRelaySecretChars + 1] = {};
};

/** `access.token.set {token}`: the same rule as the `webtoken` console command. The caller saves
 *  it and restarts the app server, so the client has to reconnect with the new token. */
template <typename Reader>
Outcome access_token_parse(const Reader& r, int args, char* token, std::size_t cap) {
  if (args < 0 || !r.get_string(r.find(args, "token"), token, cap) || !token_valid(token)) {
    return Outcome::fail(Err::BadArgs, "token: 8..64 printable characters");
  }
  return Outcome::done();
}

/** `access.relay.set {url, secret}`: the remote-access relay, like the `relay` console command.
 *
 * The secret may be left out: a hub that was flashed with a secret of its own (the
 * receiver of the box never types one) uses the one in its firmware instead. */
template <typename Reader>
Outcome access_relay_parse(const Reader& r, int args, RelayRequest& out) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  if (!r.get_string(r.find(args, "url"), out.url, sizeof(out.url)) || !relay_url_valid(out.url)) {
    return Outcome::fail(Err::BadArgs, "url: ws://, wss://, http:// or https://");
  }
  const int s = r.find(args, "secret");
  if (s >= 0) {
    if (!r.get_string(s, out.secret, sizeof(out.secret))) {
      return Outcome::fail(Err::BadArgs, "secret");
    }
  } else {
    out.secret[0] = '\0';
  }
  return Outcome::done();
}

}  // namespace core::app
