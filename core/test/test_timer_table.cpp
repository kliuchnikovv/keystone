#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "core/timer_table.hpp"
#include "hubproto/payload.hpp"

namespace {

struct FiredRecord {
  std::size_t   slot = 0;
  std::uint16_t entity_id = 0;
  std::uint8_t  attr_id = 0;
  hubproto::Value val{};
  bool fired = false;
};

void test_callback(void* ctx, std::size_t slot, std::uint16_t entity_id,
                   std::uint8_t attr_id, const hubproto::Value& val) {
  auto* rec = static_cast<FiredRecord*>(ctx);
  rec->slot = slot;
  rec->entity_id = entity_id;
  rec->attr_id = attr_id;
  rec->val = val;
  rec->fired = true;
}

}  // namespace

TEST_CASE("TimerTable basic schedule and monotonic poll") {
  core::TimerTable<4> tt;
  CHECK(tt.active_count() == 0);

  FiredRecord rec{};
  auto on_val = hubproto::Value::make_bool(true);

  int slot = tt.schedule(0x0100, 0x01, on_val, 5000 /* 5s */, 1000 /* now_ms */);
  REQUIRE(slot == 0);
  CHECK(tt.active_count() == 1);

  // Poll before deadline
  CHECK(tt.poll(2000, 0, test_callback, &rec) == 0);
  CHECK_FALSE(rec.fired);
  CHECK(tt.active_count() == 1);

  // Poll at deadline
  CHECK(tt.poll(6000, 0, test_callback, &rec) == 1);
  CHECK(rec.fired);
  CHECK(rec.entity_id == 0x0100);
  CHECK(rec.attr_id == 0x01);
  CHECK(rec.val.as_bool() == true);
  CHECK(tt.active_count() == 0);
}

TEST_CASE("TimerTable capacity and cancel") {
  core::TimerTable<2> tt;
  auto val = hubproto::Value::make_bool(false);

  int s1 = tt.schedule(0x0100, 0x01, val, 1000, 0);
  int s2 = tt.schedule(0x0101, 0x01, val, 2000, 0);
  CHECK(s1 == 0);
  CHECK(s2 == 1);
  CHECK(tt.active_count() == 2);

  // Full
  CHECK(tt.schedule(0x0102, 0x01, val, 3000, 0) == -1);

  // Cancel slot 0
  CHECK(tt.cancel(0));
  CHECK(tt.active_count() == 1);
  CHECK_FALSE(tt.cancel(0));  // already cancelled

  // Now slot 0 is reusable
  int s3 = tt.schedule(0x0102, 0x01, val, 3000, 0);
  CHECK(s3 == 0);
  CHECK(tt.active_count() == 2);
}

TEST_CASE("TimerTable wall-clock deadline handling and clock sync") {
  core::TimerTable<4> tt;
  auto val = hubproto::Value::make_u8(42);

  // 1. Scheduled when clock is not synced (epoch = 0)
  int s = tt.schedule(0x0200, 0x05, val, 10000 /* 10s */, 5000 /* now_ms */, 0);
  REQUIRE(s >= 0);
  const auto* t = tt.get(static_cast<std::size_t>(s));
  REQUIRE(t != nullptr);
  CHECK_FALSE(t->has_wall_deadline);
  CHECK(t->deadline_epoch_s == 0);

  // Clock sync occurs at now_ms = 7000 (3s into the 10s timer), epoch = 1700000000
  tt.on_clock_synced(1700000000, 7000);
  CHECK(t->has_wall_deadline);
  // Remaining time was 15000 - 7000 = 8000ms = 8s -> 1700000000 + 8 = 1700000008
  CHECK(t->deadline_epoch_s == 1700000008);

  // Poll with wall-clock time
  FiredRecord rec{};
  CHECK(tt.poll(7000, 1700000005, test_callback, &rec) == 0);
  CHECK(tt.poll(7000, 1700000008, test_callback, &rec) == 1);
  CHECK(rec.fired);
  CHECK(rec.val.as_u8() == 42);
}

TEST_CASE("TimerTable save and load across reboot") {
  core::TimerTable<4> tt;
  auto val1 = hubproto::Value::make_bool(true);
  auto val2 = hubproto::Value::make_i16(-50);

  // Timer 1: 30 seconds delay with wall clock at epoch 1000 -> deadline 1030
  tt.schedule(0x0100, 0x01, val1, 30000, 5000, 1000);

  // Timer 2: 300 seconds delay with wall clock at epoch 1000 -> deadline 1300
  tt.schedule(0x0101, 0x02, val2, 300000, 5000, 1000);

  // v2 save() needs the current monotonic anchor to compute remaining delay.
  std::array<std::uint8_t, 256> buf{};
  std::size_t written = tt.save(buf.data(), buf.size(), 5000);
  REQUIRE(written > 0);
  CHECK(written == tt.serialized_size());

  // Simulate reboot after 40 seconds (epoch = 1040):
  // Timer 1 (deadline 1030) is overdue by 10s (< 600s max overdue) -> should be due immediately.
  // Timer 2 (deadline 1300) has 260s remaining.
  core::TimerTable<4> restored;
  REQUIRE(restored.load(buf.data(), written, 0 /* boot monotonic */, 1040 /* boot epoch */));
  CHECK(restored.active_count() == 2);

  FiredRecord rec{};
  // Poll immediately on boot: Timer 1 should fire!
  CHECK(restored.poll(0, 1040, test_callback, &rec) == 1);
  CHECK(rec.fired);
  CHECK(rec.entity_id == 0x0100);
  CHECK(restored.active_count() == 1);

  // Timer 2 remains active until epoch 1300
  rec.fired = false;
  CHECK(restored.poll(1000, 1200, test_callback, &rec) == 0);
  CHECK(restored.poll(260000, 1300, test_callback, &rec) == 1);
  CHECK(rec.fired);
  CHECK(rec.entity_id == 0x0101);
  CHECK(rec.val.as_i16() == -50);
}

TEST_CASE("TimerTable drops excessively stale timers on reboot") {
  core::TimerTable<4> tt;
  auto val = hubproto::Value::make_bool(false);

  // Deadline epoch = 1010
  tt.schedule(0x0100, 0x01, val, 10000, 0, 1000);

  std::array<std::uint8_t, 256> buf{};
  std::size_t written = tt.save(buf.data(), buf.size(), 0);
  REQUIRE(written > 0);

  // Reboot happens 1 hour later (epoch = 4610, overdue by 3600s > max 600s)
  core::TimerTable<4> restored;
  REQUIRE(restored.load(buf.data(), written, 0, 4610, 600 /* max 600s overdue */));
  CHECK(restored.active_count() == 0);  // dropped as stale
}

TEST_CASE("TimerTable restores remaining delay when clock is unknown") {
  core::TimerTable<4> tt;
  auto val = hubproto::Value::make_bool(true);

  // Scheduled with a wall clock: deadline epoch = 1030 (30s from 1000)
  tt.schedule(0x0100, 0x01, val, 30000, 5000, 1000);

  std::array<std::uint8_t, 256> buf{};
  // Remaining at save time (now_ms = 5000) = 30000
  std::size_t written = tt.save(buf.data(), buf.size(), 5000);
  REQUIRE(written > 0);

  // Reboot: NVS restored before the first SNTP sync (epoch unknown = 0).
  // The wall deadline is persisted but without a clock the timer is re-armed
  // from the remaining delay so it keeps counting via monotonic time.
  core::TimerTable<4> restored;
  REQUIRE(restored.load(buf.data(), written, 500 /* boot mono */, 0 /* no clock */));
  CHECK(restored.active_count() == 1);

  // Before remaining elapsed: not due
  FiredRecord rec{};
  CHECK(restored.poll(500 + 15000, 0, test_callback, &rec) == 0);
  CHECK_FALSE(rec.fired);
  // At remaining = 30000 -> fires via monotonic
  CHECK(restored.poll(500 + 30000, 0, test_callback, &rec) == 1);
  CHECK(rec.fired);
  CHECK(rec.entity_id == 0x0100);

  // Second plane: wall deadlines also survive a clock resync — overdue after
  // a long off-time is judged against max_overdue when the clock re-syncs.
  core::TimerTable<4> tt2;
  tt2.schedule(0x0100, 0x01, val, 10000, 0, 1000);  // deadline 1010
  std::array<std::uint8_t, 256> buf2{};
  std::size_t written2 = tt2.save(buf2.data(), buf2.size(), 0);
  REQUIRE(written2 > 0);
  core::TimerTable<4> restored2;
  REQUIRE(restored2.load(buf2.data(), written2, 0, 0));
  CHECK(restored2.active_count() == 1);
  // Clock syncs 10s after the deadline (< 600s max overdue): due immediately
  restored2.on_clock_synced(1020, 77);
  FiredRecord rec2{};
  CHECK(restored2.poll(77, 1020, test_callback, &rec2) == 1);
  CHECK(rec2.fired);
  CHECK(rec2.entity_id == 0x0100);

  // Same scenario, but sync arrives an hour late: dropped as stale
  core::TimerTable<4> restored3;
  REQUIRE(restored3.load(buf2.data(), written2, 0, 0));
  CHECK(restored3.active_count() == 1);
  restored3.on_clock_synced(4610, 77, 600 /* max overdue */);
  CHECK(restored3.active_count() == 0);
}

TEST_CASE("TimerTable relative timers (no wall clock) survive reboots") {
  core::TimerTable<4> tt;
  auto val = hubproto::Value::make_bool(true);

  // Scheduled while the clock was never synced: no wall deadline
  tt.schedule(0x0100, 0x01, val, 30000, 5000, 0);

  std::array<std::uint8_t, 256> buf{};
  std::size_t written = tt.save(buf.data(), buf.size(), 5000);
  REQUIRE(written > 0);

  // Remaining (30000) is persisted and survives a reboot even though there's
  // no absolute deadline — wall vs. relative is no longer a loss condition.
  core::TimerTable<4> restored;
  REQUIRE(restored.load(buf.data(), written, 200 /* boot mono */, 0 /* no clock */));
  CHECK(restored.active_count() == 1);
  FiredRecord rec{};
  CHECK(restored.poll(200 + 15000, 0, test_callback, &rec) == 0);
  CHECK(restored.poll(200 + 30000, 0, test_callback, &rec) == 1);
  CHECK(rec.fired);

  // And with a wall clock available on this boot too: same monotonic re-arm.
  core::TimerTable<4> restored_with_clock;
  REQUIRE(restored_with_clock.load(buf.data(), written, 0, 1040));
  CHECK(restored_with_clock.active_count() == 1);
  FiredRecord rec2{};
  CHECK(restored_with_clock.poll(15000, 1040, test_callback, &rec2) == 0);
  CHECK(restored_with_clock.poll(30000, 1040, test_callback, &rec2) == 1);
  CHECK(rec2.fired);
}
