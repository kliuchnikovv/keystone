#include <doctest/doctest.h>

#include <array>
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
