#include "core/presence.hpp"

#include <cstring>
#include <vector>

#include "doctest/doctest.h"

using core::PresenceTable;

TEST_CASE("a phone reports where it is") {
  PresenceTable<> t;
  CHECK(t.count() == 0);
  CHECK(t.report("Anna", true, 100));
  CHECK(t.count() == 1);
  const auto* p = t.find_by_name("Anna");
  REQUIRE(p != nullptr);
  CHECK(p->known);
  CHECK(p->home);
  CHECK(p->state(100) == core::PhonePresence::State::Home);
  CHECK(p->state(100 + core::kPresenceStaleS - 1) == core::PhonePresence::State::Home);
}

TEST_CASE("each phone is its own entity") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 10));
  REQUIRE(t.report("Boris", false, 10));
  const auto* a = t.find_by_name("Anna");
  const auto* b = t.find_by_name("Boris");
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK(a->entity_id != b->entity_id);
  CHECK(a->entity_id >= core::kPhoneEntityBase);
  CHECK(a->entity_id < core::kPhoneEntityLimit);
  // A house with two people has two entities and two rules.
  CHECK(t.count() == 2);
}

TEST_CASE("a phone that stopped reporting is unknown, not away") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 100));
  const auto* p = t.find_by_name("Anna");
  REQUIRE(p != nullptr);
  // Still fresh.
  CHECK(p->state(100 + core::kPresenceStaleS - 1) == core::PhonePresence::State::Home);
  // Gone quiet: the hub stops believing it. Otherwise a crashed app would switch the
  // lights off.
  CHECK(p->state(100 + core::kPresenceStaleS) == core::PhonePresence::State::Unknown);
  CHECK(p->state(100 + core::kPresenceStaleS + 3600) == core::PhonePresence::State::Unknown);
  // The value is not given at all when it is unknown.
  bool v = true;
  CHECK_FALSE(t.value_of(p->entity_id, 100 + core::kPresenceStaleS, v));
}

TEST_CASE("a phone going away and coming back") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 100));
  const auto* p = t.find_by_name("Anna");
  REQUIRE(p != nullptr);
  REQUIRE(t.report("Anna", false, 200));
  CHECK(p->state(200) == core::PhonePresence::State::Away);
  REQUIRE(t.report("Anna", true, 300));
  CHECK(p->state(300) == core::PhonePresence::State::Home);
  // Still one entity: the phone did not become a second one.
  CHECK(t.count() == 1);
  CHECK(t.find_by_name("Anna")->entity_id == p->entity_id);
}

TEST_CASE("a phone without a name or with a silly one is refused") {
  PresenceTable<> t;
  CHECK_FALSE(t.report("", true, 10));
  CHECK_FALSE(t.report(nullptr, true, 10));
  CHECK_FALSE(t.report("a name that is much too long to fit", true, 10));
  CHECK(t.count() == 0);
}

TEST_CASE("the table is bounded") {
  PresenceTable<2> t;
  CHECK(t.report("Anna", true, 10));
  CHECK(t.report("Boris", true, 10));
  CHECK_FALSE(t.report("Carla", true, 10));
  CHECK(t.count() == 2);
}

TEST_CASE("a phone that stopped reporting is announced once") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 100));
  const auto id = t.find_by_name("Anna")->entity_id;

  // Nothing is stale yet.
  CHECK(t.take_stale(100 + core::kPresenceStaleS - 1) == nullptr);

  // Now it is: the caller publishes "not home" for it, so a rule stops acting on
  // an answer that is no longer true.
  const auto* stale = t.take_stale(100 + core::kPresenceStaleS);
  REQUIRE(stale != nullptr);
  CHECK(stale->entity_id == id);
  CHECK_FALSE(stale->known);

  // And it happens once, not on every pass.
  CHECK(t.take_stale(100 + core::kPresenceStaleS + 60) == nullptr);

  // A fresh report makes it known again.
  REQUIRE(t.report("Anna", false, 200));
  CHECK(t.find_by_name("Anna")->known);
  CHECK(t.find_by_name("Anna")->state(200) == core::PhonePresence::State::Away);
}

TEST_CASE("a phone can be renamed and removed") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 10));
  const auto id = t.find_by_name("Anna")->entity_id;
  CHECK(t.rename(id, "Anna's phone"));
  CHECK(t.find_by_name("Anna's phone") != nullptr);
  CHECK(t.find_by_name("Anna") == nullptr);
  // The name must stay unique.
  REQUIRE(t.report("Boris", true, 10));
  const auto b = t.find_by_name("Boris")->entity_id;
  CHECK_FALSE(t.rename(b, "Anna's phone"));
  CHECK(t.remove(id));
  CHECK(t.count() == 1);
  CHECK_FALSE(t.remove(id));
}

TEST_CASE("the phones survive a reboot") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 100));
  REQUIRE(t.report("Boris", false, 100));
  const auto id = t.find_by_name("Anna")->entity_id;

  std::vector<std::uint8_t> buf(t.serialized_size());
  const std::size_t n = t.save(buf.data(), buf.size());
  CHECK(n == buf.size());

  PresenceTable<> loaded;
  REQUIRE(loaded.load(buf.data(), n));
  CHECK(loaded.count() == 2);
  CHECK(loaded.find_by_name("Anna") != nullptr);
  CHECK(loaded.find_by_name("Anna")->entity_id == id);
  // A fresh boot: the phone has not reported yet, so its presence is unknown — the
  // rules that watch it stay quiet until it says something.
  CHECK(loaded.find_by_name("Anna")->state(0) == core::PhonePresence::State::Unknown);
  CHECK_FALSE(loaded.find_by_name("Anna")->known);
}

TEST_CASE("a corrupt blob changes nothing") {
  PresenceTable<> t;
  REQUIRE(t.report("Anna", true, 100));
  std::vector<std::uint8_t> buf(t.serialized_size());
  const std::size_t n = t.save(buf.data(), buf.size());

  PresenceTable<> loaded;
  // Wrong magic.
  std::vector<std::uint8_t> bad = buf;
  bad[0] ^= 0xFF;
  CHECK_FALSE(loaded.load(bad.data(), bad.size()));
  // Truncated.
  CHECK_FALSE(loaded.load(buf.data(), n - 1));
  // A count that does not match the buffer.
  std::vector<std::uint8_t> liar = buf;
  liar[4] = 9;
  CHECK_FALSE(loaded.load(liar.data(), liar.size()));
  // An entity id that is not one of ours.
  std::vector<std::uint8_t> alien = buf;
  alien[core::PresenceTable<>::kHeaderSize] = 0x10;
  CHECK_FALSE(loaded.load(alien.data(), alien.size()));

  // The table that was there is still there.
  CHECK(t.count() == 1);
  CHECK(t.find_by_name("Anna") != nullptr);
}
