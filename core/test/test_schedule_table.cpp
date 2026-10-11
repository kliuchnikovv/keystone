#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "core/schedule_table.hpp"
#include "hubproto/payload.hpp"
#include <utility>

namespace {

using core::LocalTime;

struct Fired {
  int count = 0;
  std::size_t slot = 0;
  std::uint16_t entity = 0;
  bool on = false;
};

void on_fire(void* ctx, std::size_t slot, const core::ScheduleTable<4>::Schedule& s) {
  auto* f = static_cast<Fired*>(ctx);
  ++f->count;
  f->slot = slot;
  f->entity = s.entity_id;
  f->on = s.value.as_bool();
}

// 2026-10-12 is a Monday.
LocalTime mon(int h, int m) { return LocalTime{2026, 10, 12, h, m, 1}; }

}  // namespace

TEST_CASE("civil_day_number counts days since the epoch") {
  CHECK(core::civil_day_number(1970, 1, 1) == 0);
  CHECK(core::civil_day_number(1970, 1, 2) == 1);
  CHECK(core::civil_day_number(2000, 3, 1) == 11017);
  CHECK(core::civil_day_number(2026, 10, 13) - core::civil_day_number(2026, 10, 12) == 1);
  // A leap day exists and is distinct.
  CHECK(core::civil_day_number(2024, 3, 1) - core::civil_day_number(2024, 2, 28) == 2);
  CHECK(core::civil_day_number(2025, 3, 1) - core::civil_day_number(2025, 2, 28) == 1);
}

TEST_CASE("ScheduleTable fires at its minute, once per day") {
  core::ScheduleTable<4> t;
  Fired f;
  REQUIRE(t.add(0x0100, 0x01, hubproto::Value::make_bool(false), 22, 0) == 0);

  CHECK(t.poll(mon(21, 59), &on_fire, &f) == 0);
  CHECK(t.poll(mon(22, 0), &on_fire, &f) == 1);
  CHECK(f.entity == 0x0100);
  CHECK(f.on == false);
  // Polled every second: the same minute (and later minutes inside the window)
  // must not fire again.
  CHECK(t.poll(mon(22, 0), &on_fire, &f) == 0);
  CHECK(t.poll(mon(22, 5), &on_fire, &f) == 0);
  CHECK(f.count == 1);

  // Next day it fires again.
  CHECK(t.poll(LocalTime{2026, 10, 13, 22, 0, 2}, &on_fire, &f) == 1);
  CHECK(f.count == 2);
}

TEST_CASE("ScheduleTable catches up inside the late window, not beyond it") {
  core::ScheduleTable<4> t;
  Fired f;
  t.add(0x0100, 0x01, hubproto::Value::make_bool(true), 7, 30);

  // Hub booted / synced its clock 8 minutes late: still runs.
  CHECK(t.poll(mon(7, 38), &on_fire, &f) == 1);

  // Off for hours: a stale action is not replayed.
  core::ScheduleTable<4> t2;
  t2.add(0x0100, 0x01, hubproto::Value::make_bool(true), 7, 30);
  CHECK(t2.poll(mon(7, 41), &on_fire, &f) == 0);
  CHECK(t2.poll(mon(18, 0), &on_fire, &f) == 0);
  // A custom window is honoured.
  CHECK(t2.poll(mon(8, 30), &on_fire, &f, 60) == 1);
}

TEST_CASE("ScheduleTable honours the weekday mask") {
  core::ScheduleTable<4> t;
  Fired f;
  t.add(0x0100, 0x01, hubproto::Value::make_bool(true), 8, 0, core::kWeekdays);
  t.add(0x0200, 0x01, hubproto::Value::make_bool(true), 8, 0, core::kWeekend);

  // Monday: only the weekday schedule.
  CHECK(t.poll(mon(8, 0), &on_fire, &f) == 1);
  CHECK(f.entity == 0x0100);
  // Saturday 2026-10-17: only the weekend one.
  CHECK(t.poll(LocalTime{2026, 10, 17, 8, 0, 6}, &on_fire, &f) == 1);
  CHECK(f.entity == 0x0200);
}

TEST_CASE("ScheduleTable add validates arguments and capacity") {
  core::ScheduleTable<2> t;
  const auto v = hubproto::Value::make_bool(true);
  CHECK(t.add(1, 1, v, 24, 0) == -1);
  CHECK(t.add(1, 1, v, 0, 60) == -1);
  CHECK(t.add(1, 1, v, -1, 0) == -1);
  CHECK(t.add(1, 1, v, 1, 1, 0x00) == -1);  // no day selected
  CHECK(t.add(1, 1, v, 1, 1) == 0);
  CHECK(t.add(2, 1, v, 1, 2) == 1);
  CHECK(t.add(3, 1, v, 1, 3) == -1);  // full
  CHECK(t.active_count() == 2);
  CHECK(t.remove(0));
  CHECK_FALSE(t.remove(0));
  CHECK(t.get(0) == nullptr);
  CHECK(t.add(3, 1, v, 1, 3) == 0);  // slot reused
}

TEST_CASE("ScheduleTable save/load round-trips, including last fired day") {
  core::ScheduleTable<4> a;
  Fired f;
  a.add(0x0100, 0x01, hubproto::Value::make_bool(false), 22, 0, core::kWeekdays);
  a.add(0x0200, 0x01, hubproto::Value::make_u8(2), 6, 45);
  REQUIRE(a.poll(mon(22, 1), &on_fire, &f) == 1);  // marks slot 0 as fired today

  std::uint8_t buf[128];
  const std::size_t n = a.save(buf, sizeof(buf));
  REQUIRE(n == a.serialized_size());

  core::ScheduleTable<4> b;
  REQUIRE(b.load(buf, n));
  REQUIRE(b.active_count() == 2);
  const auto* s0 = b.get(0);
  REQUIRE(s0 != nullptr);
  CHECK(s0->entity_id == 0x0100);
  CHECK(s0->minute_of_day == 22 * 60);
  CHECK(s0->weekdays == core::kWeekdays);
  CHECK(s0->value == hubproto::Value::make_bool(false));
  CHECK(b.get(1)->value == hubproto::Value::make_u8(2));

  // After a reboot inside the window the schedule must not fire a second time.
  CHECK(b.poll(mon(22, 3), &on_fire, &f) == 0);
}

TEST_CASE("ScheduleTable save needs room; load is all-or-nothing") {
  core::ScheduleTable<4> a;
  a.add(0x0100, 0x01, hubproto::Value::make_bool(true), 1, 0);
  std::uint8_t small[8];
  CHECK(a.save(small, sizeof(small)) == 0);

  std::uint8_t buf[64];
  const std::size_t n = a.save(buf, sizeof(buf));
  REQUIRE(n > 0);

  core::ScheduleTable<4> b;
  b.add(0x0999, 0x01, hubproto::Value::make_bool(true), 5, 5);

  std::uint8_t bad[64];
  std::memcpy(bad, buf, n);
  bad[0] ^= 0xFF;  // magic
  CHECK_FALSE(b.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[2] = 9;  // version
  CHECK_FALSE(b.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[core::ScheduleTable<4>::kHeaderSize + 9] = 0xFF;  // minute >= 1440
  bad[core::ScheduleTable<4>::kHeaderSize + 10] = 0xFF;
  CHECK_FALSE(b.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[core::ScheduleTable<4>::kHeaderSize + 3] = 0x7E;  // unknown value type
  CHECK_FALSE(b.load(bad, n));
  CHECK_FALSE(b.load(buf, n - 1));  // truncated
  std::memcpy(bad, buf, n);
  bad[4] = 9;  // more entries than capacity
  CHECK_FALSE(b.load(bad, n));

  // None of the failures touched the existing table.
  REQUIRE(b.active_count() == 1);
  CHECK(b.get(0)->entity_id == 0x0999);

  CHECK(b.load(buf, n));
  CHECK(b.get(0)->entity_id == 0x0100);
}

// ---- sun-relative schedules ---------------------------------------------------------------

using hubproto::Value;

namespace {
core::SunTimes sun_today(int rise, int set) {
  core::SunTimes s;
  s.valid = true;
  s.rise = rise;
  s.set = set;
  return s;
}
std::uint16_t pt(const char* text) {
  std::uint16_t p = 0;
  REQUIRE(core::parse_time_point(text, p));
  return p;
}
}  // namespace

TEST_CASE("a sunset schedule fires at sunset plus its offset, once, and follows the sun day by day") {
  core::ScheduleTable<4> t;
  REQUIRE(t.add_point(0x100, 1, Value::make_bool(true), pt("sunset+30")) >= 0);
  Fired f;
  const auto sun = sun_today(6 * 60, 20 * 60);  // sunset 20:00 -> due 20:30
  CHECK(t.poll(mon(20, 29), &on_fire, &f, 10, &sun) == 0);
  CHECK(t.poll(mon(20, 30), &on_fire, &f, 10, &sun) == 1);
  CHECK(t.poll(mon(20, 31), &on_fire, &f, 10, &sun) == 0);  // once per day

  // Next day the sun sets earlier: the schedule moves with it.
  const core::LocalTime tue{2026, 10, 13, 20, 10, 2};
  const auto sun2 = sun_today(6 * 60 + 2, 19 * 60 + 40);  // due 20:10
  CHECK(t.poll(tue, &on_fire, &f, 10, &sun2) == 1);
  CHECK(f.count == 2);
}

TEST_CASE("a sunrise schedule with a negative offset") {
  core::ScheduleTable<4> t;
  REQUIRE(t.add_point(0x100, 1, Value::make_bool(false), pt("sunrise-15")) >= 0);
  Fired f;
  const auto sun = sun_today(6 * 60 + 40, 18 * 60);  // due 06:25
  CHECK(t.poll(mon(6, 24), &on_fire, &f, 10, &sun) == 0);
  CHECK(t.poll(mon(6, 25), &on_fire, &f, 10, &sun) == 1);
  CHECK_FALSE(f.on);
}

TEST_CASE("a sun-relative schedule never fires without the sun: no location, polar day, or an offset that leaves the day") {
  core::ScheduleTable<4> t;
  REQUIRE(t.add_point(0x100, 1, Value::make_bool(true), pt("sunset+30")) >= 0);
  Fired f;
  CHECK(t.poll(mon(20, 30), &on_fire, &f, 10, nullptr) == 0);    // no location
  const core::SunTimes none;
  CHECK(t.poll(mon(20, 30), &on_fire, &f, 10, &none) == 0);      // sun does not set today
  const auto late = sun_today(5 * 60, 23 * 60 + 50);             // sunset+30 = 24:20, outside the day
  CHECK(t.poll(mon(23, 59), &on_fire, &f, 10, &late) == 0);
  CHECK(t.poll(mon(0, 20), &on_fire, &f, 10, &late) == 0);
  CHECK(f.count == 0);
  // A skipped day does not use up the schedule: tomorrow with a normal sunset it fires.
  const core::LocalTime tue{2026, 10, 13, 20, 30, 2};
  const auto ok = sun_today(6 * 60, 20 * 60);
  CHECK(t.poll(tue, &on_fire, &f, 10, &ok) == 1);
}

TEST_CASE("fixed schedules ignore the sun entirely") {
  core::ScheduleTable<4> t;
  REQUIRE(t.add(0x100, 1, Value::make_bool(true), 7, 30) >= 0);
  Fired f;
  CHECK(t.poll(mon(7, 30), &on_fire, &f, 10, nullptr) == 1);
}

TEST_CASE("sun-relative schedules are stored as they are, and a bad stored point is corruption") {
  core::ScheduleTable<4> t;
  REQUIRE(t.add_point(0x100, 1, Value::make_bool(true), pt("sunset+30")) >= 0);
  REQUIRE(t.add_point(0x101, 1, Value::make_bool(false), pt("sunrise-180"), core::kWeekdays) >= 0);
  REQUIRE(t.add(0x102, 1, Value::make_bool(true), 22, 0) >= 0);
  std::uint8_t buf[128];
  const std::size_t n = t.save(buf, sizeof(buf));
  REQUIRE(n > 0);
  core::ScheduleTable<4> u;
  REQUIRE(u.load(buf, n));
  CHECK(u.get(0)->minute_of_day == pt("sunset+30"));
  CHECK(u.get(1)->minute_of_day == pt("sunrise-180"));
  CHECK(u.get(2)->minute_of_day == 22 * 60);

  // Corrupt the first schedule's time point: offset +181 / reserved bits / 1440 / 0xFFFF.
  for (std::uint16_t bad : {static_cast<std::uint16_t>(core::solar_point(true, 0) + 181), static_cast<std::uint16_t>(core::solar_point(true, 0) | 0x1000),
                            static_cast<std::uint16_t>(1440), static_cast<std::uint16_t>(0xFFFF)}) {
    std::uint8_t copy[128];
    std::memcpy(copy, buf, n);
    core::detail::put_u16_le(copy + 6 + 9, bad);  // header(6) + entity(2) attr(1) value(6)
    core::ScheduleTable<4> v;
    CAPTURE(bad);
    CHECK_FALSE(v.load(copy, n));
    CHECK(v.active_count() == 0);
  }
  CHECK(t.add_point(0x103, 1, Value::make_bool(true), 1440) == -1);
  CHECK(t.add_point(0x103, 1, Value::make_bool(true), core::solar_point(true, 200)) == -1);
}
