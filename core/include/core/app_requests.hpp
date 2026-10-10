// Management operations of the app protocol (`req` / `res`, docs/app-protocol.md §5):
// names, rules, schedules and timers. Each operation parses its `args` JSON, checks
// it against the registry the same way the console does (and a little stricter:
// writable targets, value types and ranges), applies it to the container and reports
// an Outcome. Persisting to NVS and re-broadcasting stay with the caller, so this is
// portable and host-tested.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "core/app_protocol.hpp"
#include "core/entity_registry.hpp"  // AttrState

namespace core::app {

struct Outcome {
  bool ok = true;
  Err err = Err::Internal;
  char msg[40] = {};
  bool has_index = false;
  std::int64_t index = 0;

  static Outcome fail(Err e, const char* m) {
    Outcome o;
    o.ok = false;
    o.err = e;
    std::strncpy(o.msg, m, sizeof(o.msg) - 1);
    return o;
  }
  static Outcome done() { return Outcome{}; }
  static Outcome at(std::int64_t i) {
    Outcome o;
    o.has_index = true;
    o.index = i;
    return o;
  }
};

inline constexpr std::uint8_t kOnOffAttr = 0x01;
inline constexpr std::size_t kMaxNameChars = 23;  // EntityNames::MaxNameLen (24) minus the NUL

namespace detail {

template <typename Reader>
bool get_int(const Reader& r, int obj, const char* key, std::int64_t& out) {
  return r.get_i64(r.find(obj, key), out);
}

inline bool valid_entity_name(const char* s) {
  const std::size_t n = std::strlen(s);
  if (n == 0 || n > kMaxNameChars) return false;
  if (s[0] < 'a' || s[0] > 'z') return false;  // starts with a letter: never mistaken for 0x10 / 123
  for (std::size_t i = 1; i < n; ++i) {
    const char c = s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
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
  char name[kMaxNameChars + 2];
  if (!r.get_string(r.find(args, "name"), name, sizeof(name))) return Outcome::fail(Err::BadArgs, "name");
  if (!detail::valid_entity_name(name)) return Outcome::fail(Err::BadArgs, "name: a-z 0-9 _, starts with a letter, max 23");
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

// ---- rules ---------------------------------------------------------------------------

template <typename Reg, typename Engine, typename Reader>
Outcome rule_add(Reg& reg, Engine& rules, const Reader& r, int args) {
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

  // then: a writable attribute of a real device.
  const AttrState* ts = nullptr;
  if (!detail::lookup(reg, r, then, false, ru.action_entity, ru.action_attr, ts, fail, "then")) return fail;
  if ((ts->flags & hubproto::kFlagWritable) == 0) return Outcome::fail(Err::ReadOnly, "then: attribute is read-only");
  if (!to_value(read_scalar(r, r.find(then, "value")), ts->value_type, ru.action_value)) {
    return Outcome::fail(Err::BadArgs, "then.value does not fit the attribute");
  }

  const int between = r.find(args, "between");
  if (between >= 0) {
    char from[8], to[8];
    int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
    if (!r.get_string(r.find(between, "from"), from, sizeof(from)) || !detail::parse_hhmm(from, h1, m1) ||
        !r.get_string(r.find(between, "to"), to, sizeof(to)) || !detail::parse_hhmm(to, h2, m2) ||
        h1 * 60 + m1 == h2 * 60 + m2) {
      return Outcome::fail(Err::BadArgs, "between: HH:MM to HH:MM, different");
    }
    ru.window_start = static_cast<std::uint16_t>(h1 * 60 + m1);
    ru.window_end = static_cast<std::uint16_t>(h2 * 60 + m2);
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

template <typename Reg, typename Sched, typename Reader>
Outcome schedule_add(Reg& reg, Sched& sched, const Reader& r, int args) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  Outcome fail;
  char t[8];
  int h = 0, m = 0;
  if (!r.get_string(r.find(args, "time"), t, sizeof(t)) || !detail::parse_hhmm(t, h, m)) {
    return Outcome::fail(Err::BadArgs, "time: HH:MM");
  }
  std::uint8_t days = kEveryDay;
  if (!detail::parse_days(r, r.find(args, "days"), days)) return Outcome::fail(Err::BadArgs, "days");
  hubproto::Value action{};
  if (!detail::parse_action(r, args, action)) return Outcome::fail(Err::BadArgs, "action: on, off or toggle");
  std::uint16_t entity = 0;
  if (!detail::on_off_target(reg, r, args, entity, fail)) return fail;
  const int slot = sched.add(entity, kOnOffAttr, action, h, m, days);
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

template <typename Reg, typename Timers, typename Reader>
Outcome timer_add(Reg& reg, Timers& timers, const Reader& r, int args, std::uint32_t now_ms,
                  std::uint64_t now_epoch_s) {
  if (args < 0) return Outcome::fail(Err::BadArgs, "args");
  Outcome fail;
  std::int64_t delay = 0;
  if (!detail::get_int(r, args, "delay_s", delay) || delay < 1 || delay > kMaxTimerDelayS) {
    return Outcome::fail(Err::BadArgs, "delay_s: 1..604800");
  }
  hubproto::Value action{};
  if (!detail::parse_action(r, args, action)) return Outcome::fail(Err::BadArgs, "action: on, off or toggle");
  std::uint16_t entity = 0;
  if (!detail::on_off_target(reg, r, args, entity, fail)) return fail;
  const int slot = timers.schedule(entity, kOnOffAttr, action, static_cast<std::uint32_t>(delay) * 1000u,
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

}  // namespace core::app
