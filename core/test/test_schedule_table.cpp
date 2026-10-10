#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "core/schedule_table.hpp"
#include "hubproto/payload.hpp"

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
