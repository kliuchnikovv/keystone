#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/sha256.hpp"

namespace {

std::string hex(const std::uint8_t* d) {
  std::string s;
  char b[3];
  for (int i = 0; i < 32; ++i) {
    std::snprintf(b, sizeof(b), "%02x", d[i]);
    s += b;
  }
  return s;
}

std::string sha_of(const std::string& in) {
  core::Sha256 h;
  h.update(reinterpret_cast<const std::uint8_t*>(in.data()), in.size());
  std::uint8_t d[32];
  h.finish(d);
  return hex(d);
}

}  // namespace

TEST_CASE("sha256 matches the FIPS 180-4 test vectors") {
  CHECK(sha_of("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha_of("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256 of a million 'a' (multi-block) matches the vector") {
  core::Sha256 h;
  const std::string chunk(1000, 'a');
  for (int i = 0; i < 1000; ++i) {
    h.update(reinterpret_cast<const std::uint8_t*>(chunk.data()), chunk.size());
  }
  std::uint8_t d[32];
  h.finish(d);
  CHECK(hex(d) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 is independent of how the input is split into updates") {
  const std::string msg(1000, 'x');
  const auto whole = sha_of(msg);
  for (std::size_t step : {1u, 7u, 63u, 64u, 65u, 240u}) {
    core::Sha256 h;
    for (std::size_t i = 0; i < msg.size(); i += step) {
      const std::size_t n = (msg.size() - i < step) ? msg.size() - i : step;
      h.update(reinterpret_cast<const std::uint8_t*>(msg.data()) + i, n);
    }
    std::uint8_t d[32];
    h.finish(d);
    CHECK(hex(d) == whole);
  }
}
