// StateMachine: applies inbound STATE updates to the registry and notifies
// subscribers when an attribute's value actually changes (SPEC §7).
//
// STATE carries raw value bytes whose length depends on each attribute's
// value_type, which is not on the wire — the type is resolved from the registry
// (populated by DECLARE). Change notifications are edge-triggered: a repeated
// STATE with the same value fires nothing, which is what rules want.
//
// Templated on the registry type; radio-free.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace core {

template <typename Registry, std::size_t MaxSubscribers = 4>
class StateMachine {
 public:
  using ChangeFn = void (*)(void* ctx, std::uint16_t entity_id,
                            std::uint8_t attr_id, const hubproto::Value& value);

  StateMachine(Bus& bus, Registry& reg) noexcept : reg_(reg) {
    bus.subscribe(&StateMachine::on_msg, this);
  }

  // Register a change handler. Returns false if the table is full.
  bool on_change(ChangeFn fn, void* ctx) noexcept;

  // Called for EVERY applied STATE entry, changed or not. `had_before` is false
  // for the first value learned for the attribute (after a boot, or a chip
  // reboot cleared the registry); `changed` says whether the value differs.
  using ReportFn = void (*)(void* ctx, std::uint16_t entity_id, std::uint8_t attr_id,
                            const hubproto::Value& value, bool had_before, bool changed);
  bool on_report(ReportFn fn, void* ctx) noexcept {
    if (fn == nullptr || report_count_ >= kMaxReportSubs) return false;
    report_subs_[report_count_++] = {fn, ctx};
    return true;
  }

 private:
  static void on_msg(void* ctx, const hubproto::Header& h,
                     const std::uint8_t* payload, std::size_t len) noexcept {
    static_cast<StateMachine*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void notify(std::uint16_t entity_id, std::uint8_t attr_id,
              const hubproto::Value& v) noexcept;

  struct Sub {
    ChangeFn fn = nullptr;
    void* ctx = nullptr;
  };

  Registry& reg_;
  Sub subs_[MaxSubscribers];
  int sub_count_ = 0;

  static constexpr int kMaxReportSubs = 2;
  struct ReportSub {
    ReportFn fn = nullptr;
    void* ctx = nullptr;
  };
  ReportSub report_subs_[kMaxReportSubs];
  int report_count_ = 0;
};

// ---- implementation -------------------------------------------------------

template <typename Registry, std::size_t MaxSubscribers>
bool StateMachine<Registry, MaxSubscribers>::on_change(ChangeFn fn,
                                                       void* ctx) noexcept {
  if (fn == nullptr || sub_count_ >= static_cast<int>(MaxSubscribers)) return false;
  subs_[sub_count_].fn = fn;
  subs_[sub_count_].ctx = ctx;
  ++sub_count_;
  return true;
}

template <typename Registry, std::size_t MaxSubscribers>
void StateMachine<Registry, MaxSubscribers>::notify(
    std::uint16_t entity_id, std::uint8_t attr_id,
    const hubproto::Value& v) noexcept {
  for (int i = 0; i < sub_count_; ++i) {
    subs_[i].fn(subs_[i].ctx, entity_id, attr_id, v);
  }
}

template <typename Registry, std::size_t MaxSubscribers>
void StateMachine<Registry, MaxSubscribers>::handle(const hubproto::Header& h,
                                                    const std::uint8_t* payload,
                                                    std::size_t len) noexcept {
  if (h.type != hubproto::MsgType::State) return;

  hubproto::StateReader r = hubproto::state_reader(payload, len);
  if (!r.valid) return;

  std::uint8_t attr_id = 0;
  while (hubproto::state_next_attr(r, attr_id)) {
    hubproto::ValueType vt{};
    // We cannot skip an attribute of unknown length, so an unresolved type ends
    // parsing of this STATE.
    if (!reg_.value_type_of(r.entity_id, attr_id, vt)) return;

    hubproto::Value value{};
    if (!hubproto::state_consume(r, vt, value)) return;

    hubproto::Value previous{};
    const bool had = reg_.get_value(r.entity_id, attr_id, previous);
    if (!reg_.set_value(r.entity_id, attr_id, value)) continue;
    const bool changed = !had || previous != value;
    if (changed) notify(r.entity_id, attr_id, value);
    for (int i = 0; i < report_count_; ++i) {
      report_subs_[i].fn(report_subs_[i].ctx, r.entity_id, attr_id, value, had, changed);
    }
  }
}

}  // namespace core
