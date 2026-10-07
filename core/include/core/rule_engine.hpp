// RuleEngine: a minimal in-code trigger -> condition -> action table (SPEC §7).
//
// Because every entity from every radio chip converges on one brain, cross-device
// rules ("occupancy in hall -> light in kitchen") run locally with no radio
// races. A rule fires when a state change matches its trigger entity/attr and its
// predicate; the action sends a COMMAND via the CommandDispatcher.
//
// Wire the engine to StateMachine::on_change via on_state_change. Time for the
// dispatched command comes from an injected NowFn (the core stays clock-free).
// A later phase may swap this table for an embedded script (Berry/Lua).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"
#include "hubproto/payload.hpp"

namespace core {

// Predicate on the changed value; nullptr means "always match".
using Predicate = bool (*)(const hubproto::Value& value);

// Data-driven comparison, so a rule can be stored, listed and edited at runtime
// (a Predicate function pointer can be none of those).
enum class CmpOp : std::uint8_t { Always = 0, Eq, Ne, Lt, Le, Gt, Ge };

// True if `v <op> operand`. Both must be the same type; XyColor supports only
// Eq/Ne (compared byte-wise). A type mismatch never matches.
inline bool value_matches(const hubproto::Value& v, CmpOp op,
                          const hubproto::Value& operand) noexcept {
  if (op == CmpOp::Always) return true;
  if (v.type != operand.type) return false;
  int cmp = 0;  // <0, 0, >0
  switch (v.type) {
    case hubproto::ValueType::Bool:
    case hubproto::ValueType::U8:
    case hubproto::ValueType::Enum8: {
      const int a = v.raw[0], b = operand.raw[0];
      cmp = (a > b) - (a < b);
      break;
    }
    case hubproto::ValueType::I16: {
      const int a = v.as_i16(), b = operand.as_i16();
      cmp = (a > b) - (a < b);
      break;
    }
    case hubproto::ValueType::U16: {
      const int a = v.as_u16(), b = operand.as_u16();
      cmp = (a > b) - (a < b);
      break;
    }
    case hubproto::ValueType::U32: {
      const std::uint32_t a = v.as_u32(), b = operand.as_u32();
      cmp = (a > b) - (a < b);
      break;
    }
    case hubproto::ValueType::F32: {
      const float a = v.as_f32(), b = operand.as_f32();
      cmp = (a > b) - (a < b);
      break;
    }
    case hubproto::ValueType::XyColor:
      if (op != CmpOp::Eq && op != CmpOp::Ne) return false;
      cmp = std::memcmp(v.raw, operand.raw, 4) == 0 ? 0 : 1;
      break;
    default:
      return false;
  }
  switch (op) {
    case CmpOp::Eq: return cmp == 0;
    case CmpOp::Ne: return cmp != 0;
    case CmpOp::Lt: return cmp < 0;
    case CmpOp::Le: return cmp <= 0;
    case CmpOp::Gt: return cmp > 0;
    case CmpOp::Ge: return cmp >= 0;
    case CmpOp::Always: return true;
  }
  return false;
}

// A rule matches when `match` (if set) accepts the value, otherwise when
// `value <op> operand` holds. Only rules without a `match` function are stored.
struct Rule {
  std::uint16_t   trigger_entity;
  std::uint8_t    trigger_attr;
  Predicate       match;
  std::uint16_t   action_entity;
  std::uint8_t    action_attr;
  hubproto::Value action_value;
  CmpOp           op = CmpOp::Always;
  hubproto::Value operand{};
};

template <typename Dispatcher, std::size_t MaxRules = 16>
class RuleEngine {
 public:
  using NowFn = std::uint32_t (*)(void* ctx);

  RuleEngine(Dispatcher& dispatcher, NowFn now_fn, void* now_ctx) noexcept
      : disp_(dispatcher), now_fn_(now_fn), now_ctx_(now_ctx) {}

  // Add a rule. Returns false if the table is full.
  bool add_rule(const Rule& rule) noexcept;

  // Adapter matching StateMachine::ChangeFn — register with sm.on_change().
  static void on_state_change(void* ctx, std::uint16_t entity_id,
                              std::uint8_t attr_id,
                              const hubproto::Value& value) noexcept {
    static_cast<RuleEngine*>(ctx)->evaluate(entity_id, attr_id, value);
  }

  std::size_t rule_count() const noexcept { return static_cast<std::size_t>(count_); }

  // Rule at index i, or nullptr.
  const Rule* rule_at(std::size_t i) const noexcept {
    return i < static_cast<std::size_t>(count_) ? &rules_[i] : nullptr;
  }

  // Remove rule i (later rules shift down). Returns false if out of range.
  bool remove_rule(std::size_t i) noexcept {
    if (i >= static_cast<std::size_t>(count_)) return false;
    for (int j = static_cast<int>(i); j + 1 < count_; ++j) rules_[j] = rules_[j + 1];
    --count_;
    return true;
  }

  // ---- persistence (data rules only: those without a `match` function) -----
  static constexpr std::uint16_t kMagic = 0x5255;  // 'UR'
  static constexpr std::uint8_t  kVersion = 1;
  static constexpr std::size_t   kHeaderSize = 6;
  static constexpr std::size_t   kEntryWireSize = 19;

  std::size_t stored_count() const noexcept {
    std::size_t n = 0;
    for (int i = 0; i < count_; ++i) if (rules_[i].match == nullptr) ++n;
    return n;
  }

  std::size_t serialized_size() const noexcept {
    return kHeaderSize + stored_count() * kEntryWireSize;
  }

  // Returns bytes written, or 0 if `cap` is too small.
  std::size_t save(void* out, std::size_t cap) const noexcept {
    if (out == nullptr || cap < serialized_size()) return 0;
    auto* p = static_cast<std::uint8_t*>(out);
    detail::put_u16_le(p, kMagic);
    p[2] = kVersion;
    p[3] = 0;
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(stored_count()));
    std::size_t off = kHeaderSize;
    for (int i = 0; i < count_; ++i) {
      const Rule& r = rules_[i];
      if (r.match != nullptr) continue;
      detail::put_u16_le(p + off, r.trigger_entity);
      p[off + 2] = r.trigger_attr;
      p[off + 3] = static_cast<std::uint8_t>(r.op);
      put_value(p + off + 4, r.operand);
      detail::put_u16_le(p + off + 10, r.action_entity);
      p[off + 12] = r.action_attr;
      put_value(p + off + 13, r.action_value);
      off += kEntryWireSize;
    }
    return off;
  }

  // Replace the stored (data) rules with those in the buffer; in-code rules with
  // a `match` function are kept. All-or-nothing: a corrupt or oversized buffer
  // changes nothing and returns false.
  bool load(const void* in, std::size_t len) noexcept {
    if (in == nullptr || len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in);
    if (detail::get_u16_le(p) != kMagic || p[2] != kVersion) return false;
    const std::size_t n = detail::get_u16_le(p + 4);
    if (len < kHeaderSize + n * kEntryWireSize) return false;
    std::size_t kept = 0;
    for (int i = 0; i < count_; ++i) if (rules_[i].match != nullptr) ++kept;
    if (kept + n > MaxRules) return false;

    Rule loaded[MaxRules];
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint8_t* e = p + kHeaderSize + i * kEntryWireSize;
      Rule& r = loaded[i];
      r.trigger_entity = detail::get_u16_le(e);
      r.trigger_attr = e[2];
      if (e[3] > static_cast<std::uint8_t>(CmpOp::Ge)) return false;
      r.op = static_cast<CmpOp>(e[3]);
      r.match = nullptr;
      if (!get_value(e + 4, r.operand)) return false;
      r.action_entity = detail::get_u16_le(e + 10);
      r.action_attr = e[12];
      if (!get_value(e + 13, r.action_value)) return false;
    }
    int w = 0;
    for (int i = 0; i < count_; ++i) if (rules_[i].match != nullptr) rules_[w++] = rules_[i];
    for (std::size_t i = 0; i < n; ++i) rules_[w++] = loaded[i];
    count_ = w;
    return true;
  }

 private:
  static void put_value(std::uint8_t* p, const hubproto::Value& v) noexcept {
    p[0] = static_cast<std::uint8_t>(v.type);
    p[1] = v.len;
    std::memcpy(p + 2, v.raw, 4);
  }
  // Rejects an unknown type or a length that disagrees with the type.
  static bool get_value(const std::uint8_t* p, hubproto::Value& v) noexcept {
    const auto t = static_cast<hubproto::ValueType>(p[0]);
    if (hubproto::value_size(t) == 0 || p[1] != hubproto::value_size(t)) return false;
    v.type = t;
    v.len = p[1];
    std::memcpy(v.raw, p + 2, 4);
    return true;
  }

  void evaluate(std::uint16_t entity_id, std::uint8_t attr_id,
                const hubproto::Value& value) noexcept;

  Dispatcher& disp_;
  NowFn now_fn_;
  void* now_ctx_;
  Rule rules_[MaxRules];
  int count_ = 0;
};

// ---- implementation -------------------------------------------------------

template <typename Dispatcher, std::size_t MaxRules>
bool RuleEngine<Dispatcher, MaxRules>::add_rule(const Rule& rule) noexcept {
  if (count_ >= static_cast<int>(MaxRules)) return false;
  rules_[count_++] = rule;
  return true;
}

template <typename Dispatcher, std::size_t MaxRules>
void RuleEngine<Dispatcher, MaxRules>::evaluate(
    std::uint16_t entity_id, std::uint8_t attr_id,
    const hubproto::Value& value) noexcept {
  for (int i = 0; i < count_; ++i) {
    const Rule& r = rules_[i];
    if (r.trigger_entity != entity_id || r.trigger_attr != attr_id) continue;
    if (r.match != nullptr) {
      if (!r.match(value)) continue;
    } else if (!value_matches(value, r.op, r.operand)) {
      continue;
    }

    const std::uint32_t now = (now_fn_ != nullptr) ? now_fn_(now_ctx_) : 0;
    std::uint16_t seq = 0;
    disp_.send(r.action_entity, r.action_attr, r.action_value, now, seq);
  }
}

}  // namespace core
