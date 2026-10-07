// DesiredState + Reconciler: remember what the hub last *asked* an attribute to
// be, and put it back when the hub (re)learns the real value after a gap.
//
// Invariant (docs/backlog.md § Recovery): a desired value exists only if the hub
// itself commanded it (console, timer, rule, Matter write). We never invent one
// from an observed value, so a lamp nobody ever commanded is never "corrected".
//
// Behaviour, per (entity, attr) that has a desired value:
//   - STATE equals desired            -> nothing to do.
//   - First value after the hub had none (brain boot, chip reboot cleared the
//     registry): it differs -> send ONE corrective COMMAND (event Corrected).
//   - Later spontaneous STATE that differs, more than `adopt_grace_ms` after our
//     last command: a human (button, app) changed it -> the desired follows
//     (event Adopted). The human wins; we do not fight a physical toggle.
//
// Time is injected; persistable (all-or-nothing load); radio-free and portable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"
#include "hubproto/payload.hpp"

namespace core {

template <std::size_t MaxEntries = 32>
class DesiredState {
 public:
  static constexpr std::uint16_t kMagic = 0x4453;  // 'SD'
  static constexpr std::uint8_t  kVersion = 1;
  static constexpr std::size_t   kHeaderSize = 6;
  static constexpr std::size_t   kEntryWireSize = 9;

  struct Entry {
    bool            used = false;
    std::uint16_t   entity_id = 0;
    std::uint8_t    attr_id = 0;
    hubproto::Value value{};
    std::uint32_t   set_ms = 0;  // when the hub last asked (not persisted)
  };

  // Insert or overwrite. Returns false only if the table is full.
  bool set(std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v,
           std::uint32_t now_ms) noexcept {
    Entry* slot = find_mut(entity, attr);
    if (slot == nullptr) {
      for (auto& e : entries_) if (!e.used) { slot = &e; break; }
      if (slot == nullptr) return false;
      slot->used = true;
      slot->entity_id = entity;
      slot->attr_id = attr;
    }
    slot->value = v;
    slot->set_ms = now_ms;
    return true;
  }

  const Entry* find(std::uint16_t entity, std::uint8_t attr) const noexcept {
    for (const auto& e : entries_) {
      if (e.used && e.entity_id == entity && e.attr_id == attr) return &e;
    }
    return nullptr;
  }
  Entry* find_mut(std::uint16_t entity, std::uint8_t attr) noexcept {
    return const_cast<Entry*>(static_cast<const DesiredState*>(this)->find(entity, attr));
  }

  bool clear(std::uint16_t entity, std::uint8_t attr) noexcept {
    Entry* e = find_mut(entity, attr);
    if (e == nullptr) return false;
    e->used = false;
    return true;
  }
  // Drop every attribute of an entity. Returns how many were removed.
  std::size_t clear_entity(std::uint16_t entity) noexcept {
    std::size_t n = 0;
    for (auto& e : entries_) if (e.used && e.entity_id == entity) { e.used = false; ++n; }
    return n;
  }
  void clear_all() noexcept { for (auto& e : entries_) e.used = false; }

  std::size_t count() const noexcept {
    std::size_t n = 0;
    for (const auto& e : entries_) if (e.used) ++n;
    return n;
  }
  // i-th used entry (0..count()-1), or nullptr.
  const Entry* at(std::size_t i) const noexcept {
    for (const auto& e : entries_) {
      if (!e.used) continue;
      if (i == 0) return &e;
      --i;
    }
    return nullptr;
  }

  std::size_t serialized_size() const noexcept { return kHeaderSize + count() * kEntryWireSize; }

  std::size_t save(void* out, std::size_t cap) const noexcept {
    if (out == nullptr || cap < serialized_size()) return 0;
    auto* p = static_cast<std::uint8_t*>(out);
    detail::put_u16_le(p, kMagic);
    p[2] = kVersion;
    p[3] = 0;
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(count()));
    std::size_t off = kHeaderSize;
    for (const auto& e : entries_) {
      if (!e.used) continue;
      detail::put_u16_le(p + off, e.entity_id);
      p[off + 2] = e.attr_id;
      p[off + 3] = static_cast<std::uint8_t>(e.value.type);
      p[off + 4] = e.value.len;
      std::memcpy(p + off + 5, e.value.raw, 4);
      off += kEntryWireSize;
    }
    return off;
  }

  // Replace the table. All-or-nothing; a bad buffer changes nothing.
  bool load(const void* in, std::size_t len, std::uint32_t now_ms = 0) noexcept {
    if (in == nullptr || len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in);
    if (detail::get_u16_le(p) != kMagic || p[2] != kVersion) return false;
    const std::size_t n = detail::get_u16_le(p + 4);
    if (n > MaxEntries || len < kHeaderSize + n * kEntryWireSize) return false;
    Entry tmp[MaxEntries];
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint8_t* e = p + kHeaderSize + i * kEntryWireSize;
      const auto t = static_cast<hubproto::ValueType>(e[3]);
      const std::size_t sz = hubproto::value_size(t);
      if (sz == 0 || e[4] != sz) return false;
      tmp[i].used = true;
      tmp[i].entity_id = detail::get_u16_le(e);
      tmp[i].attr_id = e[2];
      tmp[i].value.type = t;
      tmp[i].value.len = e[4];
      std::memcpy(tmp[i].value.raw, e + 5, 4);
      tmp[i].set_ms = now_ms;
    }
    for (std::size_t i = 0; i < MaxEntries; ++i) entries_[i] = i < n ? tmp[i] : Entry{};
    return true;
  }

 private:
  Entry entries_[MaxEntries];
};

template <typename Dispatcher, std::size_t MaxEntries = 32>
class Reconciler {
 public:
  enum class Kind : std::uint8_t { Corrected, Adopted };
  using NowFn = std::uint32_t (*)(void* ctx);
  // actual: the value just reported; desired: what the hub wanted (for Adopted
  // that is the value it WAS before adopting).
  using EventFn = void (*)(void* ctx, Kind kind, std::uint16_t entity,
                           std::uint8_t attr, const hubproto::Value& actual,
                           const hubproto::Value& desired);

  struct Config {
    std::uint32_t adopt_grace_ms = 5000;  // after our own command, STATE lag is normal
  };

  Reconciler(Dispatcher& disp, NowFn now_fn, void* now_ctx, Config cfg) noexcept
      : disp_(disp), now_fn_(now_fn), now_ctx_(now_ctx), cfg_(cfg) {}
  Reconciler(Dispatcher& disp, NowFn now_fn, void* now_ctx) noexcept
      : Reconciler(disp, now_fn, now_ctx, Config{}) {}

  void on_event(EventFn fn, void* ctx) noexcept { ev_fn_ = fn; ev_ctx_ = ctx; }

  DesiredState<MaxEntries>& desired() noexcept { return desired_; }
  const DesiredState<MaxEntries>& desired() const noexcept { return desired_; }

  // True once if the table changed since the last call (persist it then).
  bool take_dirty() noexcept {
    const bool d = dirty_;
    dirty_ = false;
    return d;
  }

  // The hub commanded entity/attr (wire to CommandDispatcher::on_send).
  void note_command(std::uint16_t entity, std::uint8_t attr,
                    const hubproto::Value& v) noexcept {
    const auto* cur = desired_.find(entity, attr);
    if (cur == nullptr || cur->value != v) dirty_ = true;
    desired_.set(entity, attr, v, now());
  }
  static void note_command_thunk(void* ctx, std::uint16_t entity, std::uint8_t attr,
                                 const hubproto::Value& v) noexcept {
    static_cast<Reconciler*>(ctx)->note_command(entity, attr, v);
  }

  // A STATE was applied (wire to StateMachine::on_report).
  void on_report(std::uint16_t entity, std::uint8_t attr, const hubproto::Value& actual,
                 bool had_before, bool /*changed*/) noexcept {
    const auto* d = desired_.find(entity, attr);
    if (d == nullptr || d->value == actual) return;
    const std::uint32_t t = now();
    const hubproto::Value wanted = d->value;
    if (!had_before) {
      std::uint16_t seq = 0;
      disp_.send(entity, attr, wanted, t, seq);
      desired_.set(entity, attr, wanted, t);  // restart the adopt grace
      emit(Kind::Corrected, entity, attr, actual, wanted);
    } else if (t - d->set_ms > cfg_.adopt_grace_ms) {
      desired_.set(entity, attr, actual, t);
      dirty_ = true;
      emit(Kind::Adopted, entity, attr, actual, wanted);
    }
  }
  static void on_report_thunk(void* ctx, std::uint16_t entity, std::uint8_t attr,
                              const hubproto::Value& actual, bool had_before,
                              bool changed) noexcept {
    static_cast<Reconciler*>(ctx)->on_report(entity, attr, actual, had_before, changed);
  }

 private:
  std::uint32_t now() const noexcept { return now_fn_ != nullptr ? now_fn_(now_ctx_) : 0; }
  void emit(Kind k, std::uint16_t e, std::uint8_t a, const hubproto::Value& actual,
            const hubproto::Value& desired) noexcept {
    if (ev_fn_ != nullptr) ev_fn_(ev_ctx_, k, e, a, actual, desired);
  }

  Dispatcher& disp_;
  NowFn now_fn_;
  void* now_ctx_;
  Config cfg_;
  DesiredState<MaxEntries> desired_;
  bool dirty_ = false;
  EventFn ev_fn_ = nullptr;
  void* ev_ctx_ = nullptr;
};

}  // namespace core
