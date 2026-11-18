#include <doctest/doctest.h>

#include <cstring>

#include "core/matter_identity.hpp"

using namespace core::matter;

TEST_CASE("the unique id of a bridged entity is stable and says which entity it is") {
  char buf[32];
  // The Zigbee chip's first entity (0x0100) and a Matter device (0x0300).
  CHECK(unique_id_for(0x01, 0x0100, buf, sizeof(buf)) == 6);
  CHECK(std::strcmp(buf, "1:0100") == 0);

  CHECK(unique_id_for(0x03, 0x0300, buf, sizeof(buf)) == 6);
  CHECK(std::strcmp(buf, "3:0300") == 0);

  // The same entity always gives the same id — that is the whole point: the
  // controller matches the device it saw before against this string.
  char again[32];
  unique_id_for(0x01, 0x0100, again, sizeof(again));
  unique_id_for(0x03, 0x0300, buf, sizeof(buf));
  CHECK(std::strcmp(buf, again) != 0);  // a different entity, a different id
  unique_id_for(0x01, 0x0100, buf, sizeof(buf));
  CHECK(std::strcmp(buf, again) == 0);
}

TEST_CASE("the extra humidity endpoint has its own id but the main one keeps the old") {
  char buf[32];
  // 0 = the main endpoint: unchanged, so an existing accessory is not dropped.
  CHECK(unique_id_for(0x01, 0x0300, buf, sizeof(buf), 0) == 6);
  CHECK(std::strcmp(buf, "1:0300") == 0);

  // 1 = the humidity endpoint: distinct, so Home creates a second accessory.
  CHECK(unique_id_for(0x01, 0x0300, buf, sizeof(buf), 1) == 8);
  CHECK(std::strcmp(buf, "1:0300:h") == 0);

  char small[8];
  CHECK(unique_id_for(0x01, 0x0300, small, sizeof(small), 1) == 0);
}

TEST_CASE("the unique id needs room for at least the shortest form") {
  char small[4];
  // Too small for "1:100" — nothing is written, and nothing is left behind.
  CHECK(unique_id_for(0x01, 0x0100, small, sizeof(small)) == 0);
  char buf[32];
  CHECK(unique_id_for(0x01, 0x0100, buf, 0) == 0);
  CHECK(unique_id_for(0x01, 0x0100, nullptr, 32) == 0);
}

TEST_CASE("the node label is the name a person gave the device") {
  char buf[64];
  CHECK(node_label_for("kitchen plug", buf, sizeof(buf)) == 12);
  CHECK(std::strcmp(buf, "kitchen plug") == 0);

  // No name: the label stays empty (the controller shows its own default).
  CHECK(node_label_for(nullptr, buf, sizeof(buf)) == 0);
  CHECK(buf[0] == '\0');
  CHECK(node_label_for("", buf, sizeof(buf)) == 0);
  CHECK(buf[0] == '\0');
}

TEST_CASE("the node label is cut at a character boundary") {
  char buf[64];
  // The registry allows 31 characters, the Matter label 32 — so the name fits
  // whole. But a longer one has to be cut, and not in the middle of a letter.
  const char* long_name = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEF";
  const std::size_t n = node_label_for(long_name, buf, 12);
  CHECK(n == 11);
  CHECK(std::strcmp(buf, "abcdefghijk") == 0);

  // A multi-byte character is not cut in half: the label ends on a character
  // boundary, so the last copied byte is the last byte of a whole character
  // (a continuation byte of a two-byte letter is fine — what must not happen
  // is a *lead* byte with its continuation missing).
  const char* cyrillic = "свет";  // four letters, two bytes each
  char out[8];
  const std::size_t m = node_label_for(cyrillic, out, sizeof(out));
  CHECK(m == 6);  // three letters of two bytes each fit into 7 bytes
  CHECK(out[m] == '\0');
  // The last byte belongs to a complete character: the byte before it is a
  // lead byte, not a continuation byte.
  CHECK((static_cast<unsigned char>(out[m - 2]) & 0xC0) != 0x80);

  // With room for the whole word, the whole word is copied.
  char whole[16];
  CHECK(node_label_for(cyrillic, whole, sizeof(whole)) == 8);
  CHECK(std::strcmp(whole, cyrillic) == 0);

  // A three-byte character cut in the middle is dropped whole too.
  const char* emoji = "\xF0\x9F\x8C\x99";  // a sun, four bytes
  char e[5];
  const std::size_t k = node_label_for(emoji, e, sizeof(e));
  CHECK(k == 4);  // four bytes plus the NUL fit exactly
  CHECK(std::strcmp(e, emoji) == 0);
  char e2[4];
  CHECK(node_label_for(emoji, e2, sizeof(e2)) == 0);  // one byte short: nothing fits
}

TEST_CASE("the product name is the hub's device type") {
  CHECK(std::strcmp(product_name_for(0x01), "light") == 0);
  CHECK(std::strcmp(product_name_for(0x02), "sensor") == 0);
  CHECK(std::strcmp(product_name_for(0x07), "occupancy") == 0);
}

TEST_CASE("the product name of a Matter device type says what it is") {
  // The Matter type of the endpoint is what the bridge has at hand (it is
  // persisted with the endpoint), so the controller's device list can be
  // filled from it alone.
  CHECK(std::strcmp(product_name_for_kind(0x0100), "on/off light") == 0);
  CHECK(std::strcmp(product_name_for_kind(0x0101), "dimmable light") == 0);
  CHECK(std::strcmp(product_name_for_kind(0x010A), "plug-in unit") == 0);
  CHECK(std::strcmp(product_name_for_kind(0x0107), "occupancy sensor") == 0);
  CHECK(std::strcmp(product_name_for_kind(0x0301), "thermostat") == 0);

  // A kind the hub does not know: no name rather than a wrong one.
  CHECK(product_name_for_kind(0xDEAD) == nullptr);
}
