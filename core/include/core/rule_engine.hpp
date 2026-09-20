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

#include "hubproto/payload.hpp"

namespace core {

// Predicate on the changed value; nullptr means "always match".
using Predicate = bool (*)(const hubproto::Value& value);

struct Rule {
  std::uint16_t   trigger_entity;
  std::uint8_t    trigger_attr;
  Predicate       match;
  std::uint16_t   action_entity;
  std::uint8_t    action_attr;
  hubproto::Value action_value;
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

 private:
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
    if (r.match != nullptr && !r.match(value)) continue;

    const std::uint32_t now = (now_fn_ != nullptr) ? now_fn_(now_ctx_) : 0;
    std::uint16_t seq = 0;
    disp_.send(r.action_entity, r.action_attr, r.action_value, now, seq);
  }
}

}  // namespace core
