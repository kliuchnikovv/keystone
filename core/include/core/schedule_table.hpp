// ScheduleTable: fixed-capacity, allocation-free table of recurring time-of-day
// actions ("every day at 22:00 turn the plug off").
//
// The core stays clock-free: the caller passes the current LOCAL wall time (the
// brain gets it from localtime_r, so the timezone is already applied). A schedule
// fires at most once per local calendar day, within `max_late_min` minutes after
// its time (a brain that boots or syncs its clock a little late still runs it; one
// that was off for hours does not replay a stale action). The last-fired day is
// persisted so a reboot inside that window does not fire it a second time.
// Not handled: a window that crosses midnight (23:58 + 10 min is not carried into
// the next day).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/civil_time.hpp"
#include "core/endian.hpp"
#include "core/sun.hpp"
#include "hubproto/payload.hpp"

namespace core {

// Weekday bitmask: bit n = weekday n (0 = Sunday).
inline constexpr std::uint8_t kEveryDay = 0x7F;
inline constexpr std::uint8_t kWeekdays = 0x3E;  // Mon..Fri
inline constexpr std::uint8_t kWeekend = 0x41;   // Sun, Sat

template <std::size_t MaxSchedules = 8>
class ScheduleTable {
 public:
  static constexpr std::uint16_t kMagic = 0x4353;  // 'SC'
  static constexpr std::uint8_t  kVersion = 1;
  static constexpr std::size_t   kHeaderSize = 6;
  static constexpr std::size_t   kEntryWireSize = 16;
  static constexpr int           kDefaultMaxLateMin = 10;
  static constexpr std::int32_t  kNeverFired = -1;

  struct Schedule {
    bool            active = false;
    std::uint16_t   entity_id = 0;
    std::uint8_t    attr_id = 0;
    hubproto::Value value{};
    std::uint16_t   minute_of_day = 0;  // a time point (core/sun.hpp): a local minute 0..1439, or sun-relative
    std::uint8_t    weekdays = kEveryDay;
    std::int32_t    last_fired_day = kNeverFired;  // civil_day_number
  };

  using FireCallback = void (*)(void* ctx, std::size_t slot, const Schedule& s);

  // Add a schedule. Returns the slot (0..MaxSchedules-1), or -1 if the table is
  // full or the arguments are out of range.
  int add(std::uint16_t entity_id, std::uint8_t attr_id, const hubproto::Value& value,
          int hour, int minute, std::uint8_t weekdays = kEveryDay) noexcept {
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return -1;
    if ((weekdays & kEveryDay) == 0) return -1;
    for (std::size_t i = 0; i < MaxSchedules; ++i) {
      if (items_[i].active) continue;
      Schedule& s = items_[i];
      s = Schedule{};
      s.active = true;
      s.entity_id = entity_id;
      s.attr_id = attr_id;
      s.value = value;
      s.minute_of_day = static_cast<std::uint16_t>(hour * 60 + minute);
      s.weekdays = static_cast<std::uint8_t>(weekdays & kEveryDay);
      return static_cast<int>(i);
    }
    return -1;
  }

  // Add a schedule at a time point: a fixed minute or relative to sunrise/sunset ("sunset+30").
  // A sun-relative schedule fires only on days the sun's time is known (a location is set, and the
  // sun rises and sets that day) and the offset time falls inside the day.
  int add_point(std::uint16_t entity_id, std::uint8_t attr_id, const hubproto::Value& value,
                std::uint16_t point, std::uint8_t weekdays = kEveryDay) noexcept {
    if (!valid_point(point) || (weekdays & kEveryDay) == 0) return -1;
    for (std::size_t i = 0; i < MaxSchedules; ++i) {
      if (items_[i].active) continue;
      Schedule& s = items_[i];
      s = Schedule{};
      s.active = true;
      s.entity_id = entity_id;
      s.attr_id = attr_id;
      s.value = value;
      s.minute_of_day = point;
      s.weekdays = static_cast<std::uint8_t>(weekdays & kEveryDay);
      return static_cast<int>(i);
    }
    return -1;
  }

  bool remove(std::size_t slot) noexcept {
    if (slot >= MaxSchedules || !items_[slot].active) return false;
    items_[slot].active = false;
    return true;
  }

  void clear() noexcept {
    for (auto& s : items_) s.active = false;
  }

  const Schedule* get(std::size_t slot) const noexcept {
    return (slot < MaxSchedules && items_[slot].active) ? &items_[slot] : nullptr;
  }

  std::size_t active_count() const noexcept {
    std::size_t n = 0;
    for (const auto& s : items_) if (s.active) ++n;
    return n;
  }

  // Fire every schedule that is due at `now`. Returns how many fired. `sun` is today's sunrise and
  // sunset (nullptr if unknown): sun-relative schedules wait for it and never fire without it.
  std::size_t poll(const LocalTime& now, FireCallback cb, void* ctx,
                   int max_late_min = kDefaultMaxLateMin, const SunTimes* sun = nullptr) noexcept {
    if (now.weekday < 0 || now.weekday > 6) return 0;
    const std::int32_t today = civil_day_number(now.year, now.month, now.day);
    const int minute_now = now.hour * 60 + now.minute;
    std::size_t fired = 0;
    for (std::size_t i = 0; i < MaxSchedules; ++i) {
      Schedule& s = items_[i];
      if (!s.active) continue;
      if ((s.weekdays & (1u << now.weekday)) == 0) continue;
      if (s.last_fired_day == today) continue;
      int due = 0;
      if (!point_minute(s.minute_of_day, sun, due)) continue;
      if (due < 0 || due >= 24 * 60) continue;  // a sun offset that leaves the day: skipped today
      const int late = minute_now - due;
      if (late < 0 || late > max_late_min) continue;
      s.last_fired_day = today;
      ++fired;
      if (cb != nullptr) cb(ctx, i, s);
    }
    return fired;
  }

  // ---- persistence ---------------------------------------------------------
  // Entry: entity u16, attr u8, value (type u8, len u8, raw[4]), minute u16,
  //        weekdays u8, last_fired_day i32.
  std::size_t serialized_size() const noexcept {
    return kHeaderSize + active_count() * kEntryWireSize;
  }

  // Returns bytes written, or 0 if `cap` is too small.
  std::size_t save(void* out, std::size_t cap) const noexcept {
    if (out == nullptr || cap < serialized_size()) return 0;
    auto* p = static_cast<std::uint8_t*>(out);
    detail::put_u16_le(p, kMagic);
    p[2] = kVersion;
    p[3] = 0;
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(active_count()));
    std::size_t off = kHeaderSize;
    for (const auto& s : items_) {
      if (!s.active) continue;
      detail::put_u16_le(p + off, s.entity_id);
      p[off + 2] = s.attr_id;
      p[off + 3] = static_cast<std::uint8_t>(s.value.type);
      p[off + 4] = s.value.len;
      std::memcpy(p + off + 5, s.value.raw, 4);
      detail::put_u16_le(p + off + 9, s.minute_of_day);
      p[off + 11] = s.weekdays;
      detail::put_u32_le(p + off + 12, static_cast<std::uint32_t>(s.last_fired_day));
      off += kEntryWireSize;
    }
    return off;
  }

  // Replace all schedules with those in the buffer. All-or-nothing: a corrupt or
  // oversized buffer changes nothing and returns false.
  bool load(const void* in, std::size_t len) noexcept {
    if (in == nullptr || len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in);
    if (detail::get_u16_le(p) != kMagic || p[2] != kVersion) return false;
    const std::size_t n = detail::get_u16_le(p + 4);
    if (n > MaxSchedules || len < kHeaderSize + n * kEntryWireSize) return false;

    Schedule loaded[MaxSchedules];
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint8_t* e = p + kHeaderSize + i * kEntryWireSize;
      Schedule& s = loaded[i];
      const auto vt = static_cast<hubproto::ValueType>(e[3]);
      if (hubproto::value_size(vt) == 0 || e[4] != hubproto::value_size(vt)) return false;
      const std::uint16_t minute = detail::get_u16_le(e + 9);
      if (!valid_point(minute) || (e[11] & kEveryDay) == 0) return false;
      s.active = true;
      s.entity_id = detail::get_u16_le(e);
      s.attr_id = e[2];
      s.value.type = vt;
      s.value.len = e[4];
      std::memcpy(s.value.raw, e + 5, 4);
      s.minute_of_day = minute;
      s.weekdays = static_cast<std::uint8_t>(e[11] & kEveryDay);
      s.last_fired_day = static_cast<std::int32_t>(detail::get_u32_le(e + 12));
    }
    clear();
    for (std::size_t i = 0; i < n; ++i) items_[i] = loaded[i];
    return true;
  }

 private:
  Schedule items_[MaxSchedules] = {};
};

}  // namespace core
