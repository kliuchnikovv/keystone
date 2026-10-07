// TimerTable: fixed-capacity, allocation-free table of scheduled attribute actions.
//
// Supports relative (monotonic ms) and absolute wall-clock (epoch seconds)
// deadlines. Persistable to NVS/disk so timers survive reboots (SPEC / backlog §4).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"
#include "hubproto/payload.hpp"

namespace core {

template <std::size_t MaxTimers = 8>
class TimerTable {
 public:
  static constexpr std::uint16_t kMagic = 0x544D;  // 'TM'
  // v2: the u32 at entry offset 18 holds the REMAINING delay in ms (monotonic
  // time restarts across reboots, so persisting an absolute anchor — as v1 did
  // — made every timer fire ~1s after a restore, or get lost).
  static constexpr std::uint8_t  kVersion = 2;
  static constexpr std::size_t   kHeaderSize = 6;
  static constexpr std::size_t   kEntryWireSize = 22;

  // Maximum age (in seconds) of an expired timer on boot that we still apply.
  // Beyond this threshold, an overdue timer is considered stale and dropped.
  static constexpr std::uint64_t kDefaultMaxOverdueSeconds = 600;  // 10 minutes

  struct Timer {
    bool                active = false;
    std::uint16_t       entity_id = 0;
    std::uint8_t        attr_id = 0;
    hubproto::Value     value{};
    std::uint32_t       when_monotonic_ms = 0;
    std::uint64_t       deadline_epoch_s = 0;
    bool                has_wall_deadline = false;
  };

  using FireCallback = void (*)(void* ctx, std::size_t slot, std::uint16_t entity_id,
                                std::uint8_t attr_id, const hubproto::Value& val);

  // Schedule a timer. Returns slot index (0..MaxTimers-1) or -1 if full.
  int schedule(std::uint16_t entity_id, std::uint8_t attr_id,
               const hubproto::Value& val, std::uint32_t delay_ms,
               std::uint32_t now_monotonic_ms, std::uint64_t now_epoch_s = 0) noexcept {
    int slot = -1;
    for (std::size_t i = 0; i < MaxTimers; ++i) {
      if (!timers_[i].active) {
        slot = static_cast<int>(i);
        break;
      }
    }
    if (slot < 0) return -1;

    Timer& tm = timers_[static_cast<std::size_t>(slot)];
    tm.active = true;
    tm.entity_id = entity_id;
    tm.attr_id = attr_id;
    tm.value = val;
    tm.when_monotonic_ms = now_monotonic_ms + delay_ms;
    if (now_epoch_s > 0) {
      tm.has_wall_deadline = true;
      tm.deadline_epoch_s = now_epoch_s + static_cast<std::uint64_t>((delay_ms + 999) / 1000);
    } else {
      tm.has_wall_deadline = false;
      tm.deadline_epoch_s = 0;
    }
    return slot;
  }

  // Cancel timer at slot. Returns true if it was active.
  bool cancel(std::size_t slot) noexcept {
    if (slot >= MaxTimers || !timers_[slot].active) return false;
    timers_[slot].active = false;
    return true;
  }

  // Clear all timers.
  void clear() noexcept {
    for (auto& tm : timers_) tm.active = false;
  }

  // Read timer at slot. Returns nullptr if out of range or not active.
  const Timer* get(std::size_t slot) const noexcept {
    if (slot >= MaxTimers || !timers_[slot].active) return nullptr;
    return &timers_[slot];
  }

  // Number of active timers.
  std::size_t active_count() const noexcept {
    std::size_t count = 0;
    for (const auto& tm : timers_) {
      if (tm.active) ++count;
    }
    return count;
  }

  // Called when wall-clock time becomes known / synchronized (e.g. SNTP sync).
  // Backfills missing wall-clock deadlines and re-anchors pending timers to the
  // absolute deadline. A deadline overdue beyond `max_overdue_s` (e.g. the hub
  // was powered off past it) is dropped as stale.
  void on_clock_synced(std::uint64_t now_epoch_s, std::uint32_t now_monotonic_ms,
                       std::uint64_t max_overdue_s = kDefaultMaxOverdueSeconds) noexcept {
    if (now_epoch_s == 0) return;
    for (auto& tm : timers_) {
      if (!tm.active) continue;
      if (!tm.has_wall_deadline) {
        // Compute how many ms remain
        std::int32_t rem_ms = static_cast<std::int32_t>(tm.when_monotonic_ms - now_monotonic_ms);
        if (rem_ms < 0) rem_ms = 0;
        tm.has_wall_deadline = true;
        tm.deadline_epoch_s = now_epoch_s + static_cast<std::uint64_t>((static_cast<std::uint32_t>(rem_ms) + 999) / 1000);
      } else {
        // Re-align monotonic deadline with absolute wall-clock deadline
        if (tm.deadline_epoch_s > now_epoch_s) {
          std::uint64_t rem_s = tm.deadline_epoch_s - now_epoch_s;
          tm.when_monotonic_ms = now_monotonic_ms + static_cast<std::uint32_t>(rem_s * 1000);
        } else if (now_epoch_s - tm.deadline_epoch_s > max_overdue_s) {
          // Deadline passed long ago (e.g. the hub was off): stale, drop it.
          tm.active = false;
        } else {
          // Already due
          tm.when_monotonic_ms = now_monotonic_ms;
        }
      }
    }
  }

  // Poll for due timers and invoke `cb` for each. Returns count of fired timers.
  std::size_t poll(std::uint32_t now_monotonic_ms, std::uint64_t now_epoch_s,
                   FireCallback cb, void* ctx) noexcept {
    std::size_t fired = 0;
    for (std::size_t i = 0; i < MaxTimers; ++i) {
      Timer& tm = timers_[i];
      if (!tm.active) continue;

      bool due = false;
      if (tm.has_wall_deadline && now_epoch_s > 0) {
        due = (now_epoch_s >= tm.deadline_epoch_s);
      } else {
        // Relative evaluation: either a relative timer, or an absolute one
        // whose clock isn't known yet — in both cases `when_monotonic_ms`
        // holds a valid remaining-delay anchor (fresh after load()).
        due = (static_cast<std::int32_t>(now_monotonic_ms - tm.when_monotonic_ms) >= 0);
      }

      if (due) {
        tm.active = false;
        ++fired;
        if (cb) {
          cb(ctx, i, tm.entity_id, tm.attr_id, tm.value);
        }
      }
    }
    return fired;
  }

  // Serialized size needed for current active timers.
  std::size_t serialized_size() const noexcept {
    return kHeaderSize + active_count() * kEntryWireSize;
  }

  // Save active timers to buffer. Returns bytes written, or 0 on error.
  // `now_monotonic_ms` is needed to persist each timer's REMAINING delay —
  // the absolute monotonic anchor is meaningless after a reboot.
  std::size_t save(void* out_buf, std::size_t out_cap,
                   std::uint32_t now_monotonic_ms) const noexcept {
    if (!out_buf) return 0;
    const std::size_t needed = serialized_size();
    if (out_cap < needed) return 0;

    auto* p = static_cast<std::uint8_t*>(out_buf);
    detail::put_u16_le(p + 0, kMagic);
    p[2] = kVersion;
    p[3] = 0;  // reserved
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(active_count()));

    std::size_t off = kHeaderSize;
    for (const auto& tm : timers_) {
      if (!tm.active) continue;
      detail::put_u16_le(p + off, tm.entity_id);
      p[off + 2] = tm.attr_id;
      p[off + 3] = static_cast<std::uint8_t>(tm.value.type);
      p[off + 4] = tm.value.len;
      std::memcpy(p + off + 5, tm.value.raw, 4);
      p[off + 9] = tm.has_wall_deadline ? 0x01 : 0x00;
      detail::put_u64_le(p + off + 10, tm.deadline_epoch_s);
      const std::int32_t rem_ms =
          static_cast<std::int32_t>(tm.when_monotonic_ms - now_monotonic_ms);
      detail::put_u32_le(p + off + 18,
                         rem_ms > 0 ? static_cast<std::uint32_t>(rem_ms) : 0u);
      off += kEntryWireSize;
    }
    return needed;
  }

  // Load timers from buffer across reboot.
  // Re-anchors active timers to `now_monotonic_ms` and `now_epoch_s`.
  // Drops timers overdue by more than `max_overdue_s`.
  bool load(const void* in_buf, std::size_t in_len,
            std::uint32_t now_monotonic_ms, std::uint64_t now_epoch_s = 0,
            std::uint64_t max_overdue_s = kDefaultMaxOverdueSeconds) noexcept {
    if (!in_buf || in_len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in_buf);

    if (detail::get_u16_le(p + 0) != kMagic) return false;
    if (p[2] != kVersion) return false;

    const std::uint16_t num_entries = detail::get_u16_le(p + 4);
    const std::size_t expected = kHeaderSize + static_cast<std::size_t>(num_entries) * kEntryWireSize;
    if (in_len < expected) return false;

    clear();
    std::size_t off = kHeaderSize;
    std::size_t slot = 0;
    for (std::size_t i = 0; i < num_entries && slot < MaxTimers; ++i) {
      const std::uint16_t entity_id = detail::get_u16_le(p + off);
      const std::uint8_t  attr_id   = p[off + 2];
      const auto          v_type    = static_cast<hubproto::ValueType>(p[off + 3]);
      const std::uint8_t  v_len     = p[off + 4];
      hubproto::Value val{};
      val.type = v_type;
      val.len = v_len;
      std::memcpy(val.raw, p + off + 5, 4);

      const bool          has_wall  = (p[off + 9] & 0x01) != 0;
      const std::uint64_t deadline_epoch_s = detail::get_u64_le(p + off + 10);
      // v2 wire: remaining delay in ms (see kVersion). v1 stored an absolute
      // monotonic anchor — rejected by the version check above.
      const std::uint32_t remaining_ms = detail::get_u32_le(p + off + 18);
      off += kEntryWireSize;

      Timer& tm = timers_[slot];
      tm.entity_id = entity_id;
      tm.attr_id = attr_id;
      tm.value = val;
      tm.has_wall_deadline = has_wall;
      tm.deadline_epoch_s = deadline_epoch_s;

      if (has_wall && now_epoch_s > 0) {
        if (now_epoch_s >= deadline_epoch_s) {
          // Already expired. Check if too stale
          const std::uint64_t overdue = now_epoch_s - deadline_epoch_s;
          if (overdue > max_overdue_s) {
            // Drop stale timer
            continue;
          }
          // Expired recently: mark due immediately
          tm.when_monotonic_ms = now_monotonic_ms;
        } else {
          // Future deadline
          const std::uint64_t rem_s = deadline_epoch_s - now_epoch_s;
          tm.when_monotonic_ms = now_monotonic_ms + static_cast<std::uint32_t>(rem_s * 1000);
        }
      } else {
        // Either no wall clock yet, or a relative timer: re-arm the stored
        // remaining delay against this boot's monotonic time. (v1 ignored the
        // stored value and used now+1000ms, firing timers a second after boot.)
        tm.when_monotonic_ms = now_monotonic_ms + remaining_ms;
      }

      tm.active = true;
      ++slot;
    }
    return true;
  }

 private:
  Timer timers_[MaxTimers] = {};
};

}  // namespace core
