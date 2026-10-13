// Structured hub events (docs/brain-backlog.md § 7): "what happened, to what, and when" as data
// instead of log prose. Portable, no ESP-IDF; host-tested.
//
// Producers (the brain's state machine, dispatcher, rules, schedules, timers, scenes, chip
// monitor, reconciler) build an Event and hand it to an EventSink. A sink is anything that wants the
// history: the in-RAM EventRing (served to the app as `history`), later a flash file or the SD
// recorder (SPEC Phase 7). One producer path, any number of sinks (EventFanout).
//
// Time is the hub's uptime in whole seconds, not the wall clock: events happen before SNTP has
// synced, and the wall clock can step. The reader turns it into a wall time when it asks
// (`age = now - t_s`, wall = wall_now - age), so events from before the sync still get a right time.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/attrs.hpp"
#include "hubproto/payload.hpp"

namespace core {

enum class EventKind : std::uint8_t {
  Boot = 1,       // the hub (re)started; the ring starts here, so this is the gap marker. code = reset reason
  State = 2,      // an attribute changed: entity, attr, value
  Command = 3,    // the hub sent a command: entity, attr, value, code = CmdSource (who asked)
  CmdFailed = 4,  // a command was not carried out: entity, attr, code = hubproto::CmdAckStatus
  Rule = 5,       // a rule fired: ref = rule slot; entity/attr/value = its action (a scene: see Scene)
  Schedule = 6,   // a schedule fired: ref = slot
  Timer = 7,      // a timer fired: ref = slot
  Scene = 8,      // a scene was applied: ref = scene id, code = SceneVia
  Chip = 9,       // a co-processor changed health: ref = chip id, code = ChipEv, value = its argument
  Reconcile = 10, // desired state vs reality: entity, attr, value = actual, code = ReconcileEv
  Pairing = 11,   // the Zigbee network: code = PairingEv, entity = device (unpair/identify), ref = seconds (open)
};
inline constexpr std::uint8_t kEventKindMax = 11;

// Who made the hub send a command. Without it the history says a plug "was sent off" and nothing about why.
enum class CmdSource : std::uint8_t {
  Unknown = 0, Console = 1, App = 2, Rule = 3, Schedule = 4, Timer = 5, Scene = 6,
  Matter = 7,     // a Matter controller (Apple Home …) asked through the bridge
  Reconcile = 8,  // the hub put a device back to what it had last asked for
};
inline constexpr std::uint8_t kCmdSourceMax = 8;

enum class SceneVia : std::uint8_t { Console = 0, App = 1, Rule = 2, Schedule = 3, Timer = 4 };
enum class ChipEv : std::uint8_t { Lost = 0, Resumed = 1, Rebooted = 2, Flapping = 3 };
enum class ReconcileEv : std::uint8_t { Corrected = 0, Adopted = 1 };
enum class PairingEv : std::uint8_t { Opened = 0, Unpair = 1, Identify = 2 };

struct Event {
  std::uint32_t seq = 0;  // stamped by the ring: 1, 2, 3 … since boot, never reused
  std::uint32_t t_s = 0;  // hub uptime in seconds
  EventKind kind = EventKind::Boot;
  std::uint8_t code = 0;  // kind-specific, see above
  std::uint8_t attr = 0;
  bool has_value = false;
  std::uint16_t entity = 0;
  std::uint16_t ref = 0;
  hubproto::Value value{};
};

// Where events go.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void emit(const Event& e) noexcept = 0;
};

// Several sinks behind one. Sinks are not owned; the order is the order they were added.
template <std::size_t MaxSinks = 4>
class EventFanout final : public EventSink {
 public:
  bool add(EventSink* s) noexcept {
    if (s == nullptr || count_ >= MaxSinks) return false;
    sinks_[count_++] = s;
    return true;
  }
  void emit(const Event& e) noexcept override {
    for (std::size_t i = 0; i < count_; ++i) sinks_[i]->emit(e);
  }

 private:
  EventSink* sinks_[MaxSinks] = {};
  std::size_t count_ = 0;
};

// Attributes that report all the time (power, energy meter, light level): a change is not an event
// anybody wants in a history, and in a small ring they would push out everything that matters.
inline bool is_telemetry_attr(std::uint8_t attr) noexcept {
  return attr == 0x12 /*illuminance*/ || attr == 0x13 /*power_w*/ || attr == 0x14 /*energy_wh*/ ||
         attr >= 0x70 /*chip self-entity telemetry: uptime, heap, frame counters*/;
}

// What to keep of the STATE stream: a real device's attribute that is not telemetry. `entity` below
// 0x0100 are the chips' own entities (SPEC §6.1). A button's `event` returns to idle (0) by itself
// ~0.4 s after a press; that return is not something that happened.
inline bool state_is_historic(std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v) noexcept {
  if (entity < 0x0100 || is_telemetry_attr(attr)) return false;
  return !(attr == hubproto::attr::kEvent && v.type == hubproto::ValueType::Enum8 && v.as_enum8() == static_cast<std::uint8_t>(hubproto::attr::ButtonEvent::Idle));
}

// A chip in a crash loop reboots every couple of seconds and, with every reboot, says "rebooted" and
// "flapping" again. Those repeats would push everything else out of a small ring, so the first of each
// kind goes through and the next ones wait `min_gap_s`. (Lost/Resumed are rare by construction.) The
// log lines are not gated, and the event's value (the reboot count) still shows how many there were.
class ChipEventGate {
 public:
  static constexpr std::uint8_t kChips = 16;  // chip ids are small (SPEC §6.1)

  explicit ChipEventGate(std::uint32_t min_gap_s = 60) noexcept : min_gap_s_(min_gap_s) {}

  // True if this chip event should be recorded.
  bool allow(std::uint8_t chip, ChipEv code, std::uint32_t now_s) noexcept {
    if (code != ChipEv::Rebooted && code != ChipEv::Flapping) return true;
    Slot& s = slot_[chip % kChips][code == ChipEv::Rebooted ? 0 : 1];
    if (s.seen && static_cast<std::uint32_t>(now_s - s.t_s) < min_gap_s_) {
      ++suppressed_;
      return false;
    }
    s.seen = true;
    s.t_s = now_s;
    return true;
  }
  std::uint32_t suppressed() const noexcept { return suppressed_; }

 private:
  struct Slot {
    bool seen = false;
    std::uint32_t t_s = 0;
  };
  Slot slot_[kChips][2] = {};
  std::uint32_t min_gap_s_;
  std::uint32_t suppressed_ = 0;
};

// The STATE stream repeats itself: when a chip restarts, everything it knows is learned again and the
// registry reports "on" for a plug that was already on. A history wants changes. This remembers the last
// value recorded per (entity, attribute) and says whether a new report differs. A button's `event` is
// exempt (two identical presses are two presses). A small table, oldest entry replaced.
template <std::size_t N = 32>
class StateDedup {
 public:
  bool changed(std::uint16_t entity, std::uint8_t attr, const hubproto::Value& v) noexcept {
    if (attr == hubproto::attr::kEvent) return true;
    for (std::size_t i = 0; i < used_; ++i) {
      Entry& e = tab_[i];
      if (e.entity != entity || e.attr != attr) continue;
      if (e.value == v) return false;
      e.value = v;
      return true;
    }
    Entry& e = tab_[used_ < N ? used_++ : (next_++ % N)];
    e = Entry{entity, attr, v};
    return true;
  }
  // The device is gone (unpaired): its next report is news again.
  void forget(std::uint16_t entity) noexcept {
    std::size_t w = 0;
    for (std::size_t i = 0; i < used_; ++i) {
      if (tab_[i].entity != entity) tab_[w++] = tab_[i];
    }
    used_ = w;
  }

 private:
  struct Entry {
    std::uint16_t entity = 0;
    std::uint8_t attr = 0;
    hubproto::Value value{};
  };
  Entry tab_[N] = {};
  std::size_t used_ = 0;
  std::size_t next_ = 0;
};

// Fixed-size in-RAM history, newest wins. Not thread-safe: the owner serialises emit() and read().
template <std::size_t N>
class EventRing final : public EventSink {
  static_assert(N >= 2, "ring too small");

 public:
  void emit(const Event& e) noexcept override {
    Event& slot = buf_[next_seq_ % N];
    slot = e;
    slot.seq = next_seq_;
    ++next_seq_;
  }

  // The last sequence number written (0 = nothing yet) and the oldest one still held.
  std::uint32_t latest() const noexcept { return next_seq_ - 1; }
  std::uint32_t oldest() const noexcept {
    const std::uint32_t held = static_cast<std::uint32_t>(next_seq_ - 1 < N ? next_seq_ - 1 : N);
    return held == 0 ? 0 : next_seq_ - held;
  }
  // Events newer than `after` (a seq the reader already has), OLDEST first, at most `max`. If the reader
  // fell behind the ring (`after` older than what is held), it starts from the oldest that is held.
  std::size_t read_after(std::uint32_t after, std::size_t max, Event* out) const noexcept {
    if (latest() == 0 || max == 0 || after >= latest()) return 0;
    std::uint32_t s = after + 1 < oldest() ? oldest() : after + 1;
    std::size_t n = 0;
    for (; s <= latest() && n < max; ++s) out[n++] = buf_[s % N];
    return n;
  }
  std::size_t size() const noexcept { return latest() == 0 ? 0 : static_cast<std::size_t>(latest() - oldest() + 1); }

  // Up to `max` events, newest first, strictly older than `before` (0 = start from the newest),
  // optionally only those about one entity (`entity_filter`: events that carry it). Returns how
  // many were copied into `out`.
  std::size_t read(std::uint32_t before, std::size_t max, Event* out, bool entity_only = false,
                   std::uint16_t entity = 0) const noexcept {
    if (latest() == 0 || max == 0) return 0;
    std::uint32_t s = (before == 0 || before > next_seq_) ? next_seq_ : before;  // one past the newest to take
    std::size_t n = 0;
    const std::uint32_t lo = oldest();
    while (s > lo && n < max) {
      --s;
      const Event& e = buf_[s % N];
      if (entity_only && !(e.entity == entity && mentions_entity(e.kind))) continue;
      out[n++] = e;
    }
    return n;
  }

 private:
  static bool mentions_entity(EventKind k) noexcept {
    return k == EventKind::State || k == EventKind::Command || k == EventKind::CmdFailed ||
           k == EventKind::Reconcile || k == EventKind::Pairing || k == EventKind::Rule;
  }

  Event buf_[N] = {};
  std::uint32_t next_seq_ = 1;
};

}  // namespace core
