#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/scene_table.hpp"
#include "hubproto/payload.hpp"

namespace {

using Table = core::SceneTable<3, 4>;
using Action = Table::Action;
using hubproto::Value;

Action act(std::uint16_t e, std::uint8_t a, Value v) {
  Action x;
  x.entity_id = e;
  x.attr_id = a;
  x.value = v;
  return x;
}

}  // namespace

TEST_CASE("scene names: a letter, then letters, digits or underscore, at most 23") {
  CHECK(core::valid_scene_name("evening"));
  CHECK(core::valid_scene_name("a"));
  CHECK(core::valid_scene_name("movie_night_2"));
  CHECK(core::valid_scene_name("abcdefghijklmnopqrstuvw"));       // 23
  CHECK_FALSE(core::valid_scene_name("abcdefghijklmnopqrstuvwx"));  // 24
  for (const char* bad : {"", "1evening", "_x", "Evening", "two words", "a-b", "é", "a\n"}) {
    CAPTURE(bad);
    CHECK_FALSE(core::valid_scene_name(bad));
  }
  CHECK_FALSE(core::valid_scene_name(nullptr));
}

TEST_CASE("put creates, then replaces by name; remove frees the slot") {
  Table t;
  const Action a[] = {act(0x100, 1, Value::make_bool(true))};
  const int s0 = t.put("evening", a, 1);
  REQUIRE(s0 >= 0);
  const Action b[] = {act(0x101, 1, Value::make_bool(false)), act(0x101, 2, Value::make_u8(40))};
  CHECK(t.put("evening", b, 2) == s0);  // same name: replaced in place
  REQUIRE(t.find("evening") != nullptr);
  CHECK(t.find("evening")->count == 2);
  CHECK(t.active_count() == 1);

  CHECK(t.put("morning", a, 1) >= 0);
  CHECK(t.put("night", a, 1) >= 0);
  CHECK(t.put("fourth", a, 1) == -1);   // full
  CHECK(t.put("night", b, 2) >= 0);     // but replacing still works when full
  CHECK(t.remove("morning"));
  CHECK_FALSE(t.remove("morning"));
  CHECK(t.put("fourth", a, 1) >= 0);    // the slot is reusable
  CHECK(t.find("nope") == nullptr);
  CHECK(t.find(nullptr) == nullptr);
}

TEST_CASE("put rejects bad names, no actions, too many actions and untyped values") {
  Table t;
  const Action one[] = {act(1, 1, Value::make_bool(true))};
  CHECK(t.put("Bad", one, 1) == -1);
  CHECK(t.put("ok", one, 0) == -1);
  CHECK(t.put("ok", nullptr, 1) == -1);
  const Action five[] = {one[0], one[0], one[0], one[0], one[0]};
  CHECK(t.put("ok", five, 5) == -1);
  CHECK(t.put("ok", five, 4) >= 0);
  const Action untyped[] = {act(1, 1, Value{})};
  CHECK(t.put("other", untyped, 1) == -1);
  CHECK(t.active_count() == 1);
}

TEST_CASE("apply sends every action and counts accepted and skipped") {
  Table t;
  const Action a[] = {act(0x100, 1, Value::make_bool(true)), act(0x999, 1, Value::make_bool(true)),
                      act(0x101, 2, Value::make_u8(40))};
  REQUIRE(t.put("evening", a, 3) >= 0);
  std::vector<std::uint16_t> seen;
  const auto r = t.apply("evening", [&](const Action& x) {
    seen.push_back(x.entity_id);
    return x.entity_id != 0x999;  // pretend that entity is gone
  });
  CHECK(r.found);
  CHECK(r.sent == 2);
  CHECK(r.skipped == 1);
  CHECK(seen == std::vector<std::uint16_t>{0x100, 0x999, 0x101});  // in order
  const auto none = t.apply("nope", [&](const Action&) { return true; });
  CHECK_FALSE(none.found);
  CHECK(none.sent == 0);
}

TEST_CASE("save / load round-trips every scene, action and value type") {
  Table t;
  const Action a[] = {act(0x100, 1, Value::make_bool(true)), act(0x101, 2, Value::make_u8(40)),
                      act(0x102, 3, Value::make_u16(3000))};
  REQUIRE(t.put("evening", a, 3) >= 0);
  const Action b[] = {act(0x100, 1, Value::make_bool(false))};
  REQUIRE(t.put("off", b, 1) >= 0);

  std::uint8_t buf[512];
  const std::size_t n = t.save(buf, sizeof(buf));
  REQUIRE(n == t.serialized_size());
  CHECK(t.save(buf, n - 1) == 0);  // too small

  Table u;
  const Action junk[] = {act(7, 7, Value::make_bool(true))};
  REQUIRE(u.put("stale", junk, 1) >= 0);
  REQUIRE(u.load(buf, n));
  CHECK(u.find("stale") == nullptr);  // replaced, not merged
  REQUIRE(u.find("evening") != nullptr);
  const auto* e = u.find("evening");
  CHECK(e->count == 3);
  CHECK(e->actions[1].value.raw[0] == 40);
  CHECK(e->actions[2].value.type == hubproto::ValueType::U16);
  CHECK(u.find("off")->actions[0].value.as_bool() == false);
}

TEST_CASE("load is all-or-nothing and survives every truncation and corruption") {
  Table t;
  const Action a[] = {act(0x100, 1, Value::make_bool(true)), act(0x101, 2, Value::make_u8(40))};
  REQUIRE(t.put("evening", a, 2) >= 0);
  REQUIRE(t.put("night", a, 1) >= 0);
  std::uint8_t buf[512];
  const std::size_t n = t.save(buf, sizeof(buf));

  Table keep;
  const Action k[] = {act(9, 9, Value::make_bool(true))};
  REQUIRE(keep.put("keep", k, 1) >= 0);
  for (std::size_t cut = 0; cut < n; ++cut) {  // every strict prefix
    CAPTURE(cut);
    CHECK_FALSE(keep.load(buf, cut));
    CHECK(keep.find("keep") != nullptr);
  }
  // Mutations of single bytes: either rejected (and nothing changes) or a valid table.
  for (std::size_t i = 0; i < n; ++i) {
    std::uint8_t copy[512];
    std::memcpy(copy, buf, n);
    copy[i] = static_cast<std::uint8_t>(copy[i] ^ 0xFF);
    Table probe;
    REQUIRE(probe.put("keep", k, 1) >= 0);
    if (!probe.load(copy, n)) {
      CHECK(probe.find("keep") != nullptr);
    } else {
      CHECK(probe.active_count() <= 3);
    }
  }
  // Specific corruptions.
  std::uint8_t bad[512];
  std::memcpy(bad, buf, n);
  bad[0] ^= 1;  // magic
  CHECK_FALSE(keep.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[2] = 9;  // version
  CHECK_FALSE(keep.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[4] = 4;  // more scenes than the table holds
  CHECK_FALSE(keep.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[6 + 24 + 1] = 0;  // first scene with zero actions
  CHECK_FALSE(keep.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[6] = 'X';  // name no longer valid
  CHECK_FALSE(keep.load(bad, n));
  CHECK(keep.find("keep") != nullptr);
}

TEST_CASE("load rejects duplicate names") {
  Table t;
  const Action a[] = {act(1, 1, Value::make_bool(true))};
  REQUIRE(t.put("one", a, 1) >= 0);
  REQUIRE(t.put("two", a, 1) >= 0);
  std::uint8_t buf[256];
  const std::size_t n = t.save(buf, sizeof(buf));
  // Rename the second scene to the first's name.
  const std::size_t second = 6 + 24 + 2 + 9;
  std::memcpy(buf + second, "one", 4);
  Table u;
  CHECK_FALSE(u.load(buf, n));
  CHECK(u.active_count() == 0);
}

TEST_CASE("scenes have stable ids: kept on replace, unique, reused only after a delete, persisted") {
  Table t;
  const Action a[] = {act(1, 1, Value::make_bool(true))};
  REQUIRE(t.put("one", a, 1) >= 0);
  REQUIRE(t.put("two", a, 1) >= 0);
  const std::uint8_t one = t.find("one")->id;
  const std::uint8_t two = t.find("two")->id;
  CHECK(one != 0);
  CHECK(two != 0);
  CHECK(one != two);
  CHECK(t.find_by_id(two) == t.find("two"));
  CHECK(t.find_by_id(99) == nullptr);

  REQUIRE(t.put("one", a, 1) >= 0);  // replaced: same id
  CHECK(t.find("one")->id == one);

  REQUIRE(t.remove("one"));
  CHECK(t.find_by_id(one) == nullptr);
  REQUIRE(t.put("three", a, 1) >= 0);
  CHECK(t.find("three")->id == one);  // the freed id is the smallest free one

  std::uint8_t buf[256];
  const std::size_t n = t.save(buf, sizeof(buf));
  Table u;
  REQUIRE(u.load(buf, n));
  CHECK(u.find("two")->id == two);
  CHECK(u.find("three")->id == one);
  CHECK(u.find_by_id(one) == u.find("three"));

  // A stored table with a zero or duplicate id is corrupt.
  std::uint8_t bad[256];
  std::memcpy(bad, buf, n);
  bad[6 + 24] = 0;
  CHECK_FALSE(u.load(bad, n));
  std::memcpy(bad, buf, n);
  bad[6 + 24 + 2 + 9 + 24] = bad[6 + 24];  // second scene's id := first scene's id
  CHECK_FALSE(u.load(bad, n));
}
