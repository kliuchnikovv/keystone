#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <string>

#include "core/entity_names.hpp"

TEST_CASE("EntityNames basic mapping, update and clear") {
  core::EntityNames<4, 16> names;

  CHECK(names.size() == 0);
  CHECK(names.get_name(0x0100) == nullptr);
  CHECK(names.find_id("plug") == 0);

  // Set name
  CHECK(names.set_name(0x0100, "plug"));
  CHECK(names.size() == 1);
  CHECK(names.get_name(0x0100) != nullptr);
  CHECK(std::string(names.get_name(0x0100)) == "plug");
  CHECK(names.find_id("plug") == 0x0100);

  // Update name for same id
  CHECK(names.set_name(0x0100, "livingroom"));
  CHECK(names.size() == 1);
  CHECK(std::string(names.get_name(0x0100)) == "livingroom");
  CHECK(names.find_id("plug") == 0);
  CHECK(names.find_id("livingroom") == 0x0100);

  // Clear name
  CHECK(names.clear_name(0x0100));
  CHECK(names.size() == 0);
  CHECK(names.get_name(0x0100) == nullptr);
  CHECK(!names.clear_name(0x0100));
}

TEST_CASE("EntityNames enforces uniqueness and capacity") {
  core::EntityNames<2, 8> names;

  CHECK(names.set_name(0x0100, "lamp"));
  // Duplicate name rejected for different id
  CHECK_FALSE(names.set_name(0x0101, "lamp"));
  // Setting same name on same id succeeds
  CHECK(names.set_name(0x0100, "lamp"));

  // Second entity
  CHECK(names.set_name(0x0101, "fan"));
  CHECK(names.size() == 2);

  // Table full
  CHECK_FALSE(names.set_name(0x0102, "tv"));

  // Name too long (MaxNameLen = 8 -> max len 7)
  CHECK_FALSE(names.set_name(0x0100, "12345678")); // 8 chars -> fails
  CHECK(names.set_name(0x0100, "1234567"));  // 7 chars -> ok

  // Null or empty name rejected
  CHECK_FALSE(names.set_name(0x0100, nullptr));
  CHECK_FALSE(names.set_name(0x0100, ""));
}

TEST_CASE("EntityNames resolve handles names, hex and decimal strings") {
  core::EntityNames<4, 16> names;
  names.set_name(0x0100, "plug");

  std::uint16_t id = 0;

  // By name
  CHECK(names.resolve("plug", id));
  CHECK(id == 0x0100);

  // By hex literal
  CHECK(names.resolve("0x0100", id));
  CHECK(id == 0x0100);
  CHECK(names.resolve("0x20", id));
  CHECK(id == 0x0020);

  // By decimal literal
  CHECK(names.resolve("256", id));
  CHECK(id == 256);

  // Invalid strings
  CHECK_FALSE(names.resolve("unknown_plug", id));
  CHECK_FALSE(names.resolve("0xZZZZ", id));
  CHECK_FALSE(names.resolve("", id));
  CHECK_FALSE(names.resolve(nullptr, id));
}

TEST_CASE("EntityNames enumeration via entry_at") {
  core::EntityNames<4, 16> names;
  names.set_name(0x0010, "motion");
  names.set_name(0x0020, "light");

  CHECK(names.size() == 2);
  core::EntityNames<4, 16>::Entry e{};
  CHECK(names.entry_at(0, e));
  CHECK(e.id == 0x0010);
  CHECK(std::string(e.name) == "motion");

  CHECK(names.entry_at(1, e));
  CHECK(e.id == 0x0020);
  CHECK(std::string(e.name) == "light");

  CHECK_FALSE(names.entry_at(2, e));
}

TEST_CASE("EntityNames reads a v1 blob (24-byte names) into the wider table") {
  // A blob as the old firmware wrote it: header plus entries with a 24-byte name.
  constexpr std::size_t kLegacyEntry = 2 + 24;
  std::array<std::uint8_t, 6 + 2 * kLegacyEntry> buf{};
  buf[0] = 0x4D;  // 'NM'
  buf[1] = 0x4E;
  buf[2] = 1;  // version 1
  buf[3] = 0;
  buf[4] = 2;  // two entries
  buf[5] = 0;
  const auto put = [&buf](std::size_t i, std::uint16_t id, const char* name) {
    const std::size_t off = 6 + i * kLegacyEntry;
    buf[off] = static_cast<std::uint8_t>(id & 0xFF);
    buf[off + 1] = static_cast<std::uint8_t>(id >> 8);
    std::memcpy(buf.data() + off + 2, name, std::strlen(name));
  };
  put(0, 0x0100, "kitchen_plug");
  put(1, 0x0101, "hall_sensor");

  core::EntityNames<4, 32> names;
  REQUIRE(names.load(buf.data(), buf.size()));
  CHECK(names.size() == 2);
  CHECK(std::string(names.get_name(0x0100)) == "kitchen_plug");
  CHECK(names.find_id("hall_sensor") == 0x0101);
}

TEST_CASE("EntityNames keeps a long name (31 bytes) and a Cyrillic one") {
  core::EntityNames<4, 32> names;
  CHECK(names.set_name(0x0100, "abcdefghijklmnopqrstuvwxyz01234"));  // 31 bytes
  CHECK(names.set_name(0x0101, "Кухня розетка"));
  CHECK_FALSE(names.set_name(0x0102, "abcdefghijklmnopqrstuvwxyz012345"));  // 32 bytes: too long

  std::array<std::uint8_t, 256> buf{};
  const std::size_t written = names.save(buf.data(), buf.size());
  REQUIRE(written > 0);
  core::EntityNames<4, 32> loaded;
  REQUIRE(loaded.load(buf.data(), written));
  CHECK(std::string(loaded.get_name(0x0100)) == "abcdefghijklmnopqrstuvwxyz01234");
  CHECK(std::string(loaded.get_name(0x0101)) == "Кухня розетка");
}

TEST_CASE("EntityNames save and load round-trips correctly") {
  core::EntityNames<4, 16> names;
  CHECK(names.set_name(0x0100, "plug"));
  CHECK(names.set_name(0x0101, "heater"));

  std::array<std::uint8_t, 128> buf{};
  std::size_t written = names.save(buf.data(), buf.size());
  CHECK(written == names.serialized_size());
  CHECK(written > 0);

  // Buffer too small for save
  CHECK(names.save(buf.data(), written - 1) == 0);

  // Load into fresh instance
  core::EntityNames<4, 16> loaded;
  CHECK(loaded.load(buf.data(), written));
  CHECK(loaded.size() == 2);
  CHECK(loaded.find_id("plug") == 0x0100);
  CHECK(loaded.find_id("heater") == 0x0101);

  // Buffer truncated
  CHECK_FALSE(loaded.load(buf.data(), written - 1));

  // Magic corrupted
  buf[0] ^= 0xFF;
  CHECK_FALSE(loaded.load(buf.data(), written));
}
