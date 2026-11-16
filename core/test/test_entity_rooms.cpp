#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include "core/entity_rooms.hpp"

TEST_CASE("EntityRooms: a device goes into a room and comes out again") {
  core::EntityRooms<4, 16> rooms;

  CHECK(rooms.size() == 0);
  CHECK(rooms.get_room(0x0100) == nullptr);

  CHECK(rooms.set_room(0x0100, "Kitchen"));
  CHECK(rooms.size() == 1);
  CHECK(std::string(rooms.get_room(0x0100)) == "Kitchen");

  // A second device in the same room: unlike a name, a room is shared.
  CHECK(rooms.set_room(0x0101, "Kitchen"));
  CHECK(rooms.size() == 2);
  CHECK(std::string(rooms.get_room(0x0101)) == "Kitchen");

  // The same device moves.
  CHECK(rooms.set_room(0x0100, "Hallway"));
  CHECK(rooms.size() == 2);
  CHECK(std::string(rooms.get_room(0x0100)) == "Hallway");

  CHECK(rooms.clear_room(0x0100));
  CHECK(rooms.size() == 1);
  CHECK(rooms.get_room(0x0100) == nullptr);
  CHECK_FALSE(rooms.clear_room(0x0100));
}

TEST_CASE("EntityRooms refuses an empty or too long name") {
  core::EntityRooms<4, 8> rooms;
  CHECK_FALSE(rooms.set_room(0x0100, ""));
  CHECK_FALSE(rooms.set_room(0x0100, nullptr));
  CHECK_FALSE(rooms.set_room(0x0100, "123456789"));  // 9 bytes in an 8-byte field
  CHECK(rooms.size() == 0);
  CHECK(rooms.set_room(0x0100, "1234567"));         // 7 bytes fits
  CHECK(rooms.size() == 1);
}

TEST_CASE("EntityRooms keeps its capacity") {
  core::EntityRooms<2, 8> rooms;
  CHECK(rooms.set_room(1, "Kitchen"));
  CHECK(rooms.set_room(2, "Kitchen"));
  CHECK_FALSE(rooms.set_room(3, "Kitchen"));  // the table is full
  CHECK(rooms.set_room(1, "Hallway"));       // but an existing entry can change
  CHECK(rooms.size() == 2);
}

TEST_CASE("EntityRooms survives a save and load") {
  core::EntityRooms<8, 16> rooms;
  rooms.set_room(0x0100, "Kitchen");
  rooms.set_room(0x0300, "Living room");

  std::uint8_t buf[512];
  const std::size_t n = rooms.save(buf, sizeof(buf));
  REQUIRE(n == rooms.serialized_size());

  core::EntityRooms<8, 16> back;
  REQUIRE(back.load(buf, n));
  CHECK(back.size() == 2);
  CHECK(std::string(back.get_room(0x0100)) == "Kitchen");
  CHECK(std::string(back.get_room(0x0300)) == "Living room");
}

TEST_CASE("EntityRooms refuses a corrupt blob") {
  core::EntityRooms<8, 16> rooms;
  std::uint8_t buf[512];
  const std::size_t n = rooms.save(buf, sizeof(buf));
  REQUIRE(n > 0);

  buf[0] ^= 0xFF;  // the magic is wrong
  core::EntityRooms<8, 16> back;
  CHECK_FALSE(back.load(buf, n));
  CHECK(back.size() == 0);

  // A blob whose header is fine but whose entry is not (an empty room) leaves
  // nothing behind either — half a table is worse than none.
  core::EntityRooms<8, 16> half;
  half.set_room(1, "Kitchen");
  half.set_room(2, "Hallway");
  const std::size_t m = half.save(buf, sizeof(buf));
  REQUIRE(m == half.serialized_size());
  // Blank the second entry's room bytes (offset: header 6 + (id 2 + room 16) per entry).
  std::memset(buf + 6 + 2 + 16 + 2, 0, 16);
  CHECK_FALSE(back.load(buf, m));
  CHECK(back.size() == 0);
}

TEST_CASE("EntityRooms lists every room once") {
  core::EntityRooms<8, 16> rooms;
  rooms.set_room(1, "Kitchen");
  rooms.set_room(2, "Kitchen");
  rooms.set_room(3, "Hallway");

  std::vector<std::string> seen;
  rooms.each_room([&](const char* r) { seen.push_back(r); });
  REQUIRE(seen.size() == 2);
  CHECK(seen[0] == "Kitchen");
  CHECK(seen[1] == "Hallway");
}
