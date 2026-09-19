#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "hubproto/cobs.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes encode(const Bytes& in) {
  Bytes out(hubproto::cobs_encode_bound(in.size()));
  auto r = hubproto::cobs_encode(in.data(), in.size(), out.data(), out.size());
  REQUIRE(r.ok);
  out.resize(r.len);
  return out;
}

Bytes decode(const Bytes& in) {
  Bytes out(in.size() + 1);
  auto r = hubproto::cobs_decode(in.data(), in.size(), out.data(), out.size());
  REQUIRE(r.ok);
  out.resize(r.len);
  return out;
}

}  // namespace

TEST_CASE("cobs encodes canonical vectors") {
  CHECK(encode({}) == Bytes{0x01});
  CHECK(encode({0x00}) == Bytes{0x01, 0x01});
  CHECK(encode({0x00, 0x00}) == Bytes{0x01, 0x01, 0x01});
  CHECK(encode({0x11, 0x22, 0x00, 0x33}) == Bytes{0x03, 0x11, 0x22, 0x02, 0x33});
  CHECK(encode({0x11, 0x22, 0x33, 0x44}) == Bytes{0x05, 0x11, 0x22, 0x33, 0x44});
  CHECK(encode({0x11, 0x00, 0x00, 0x00}) == Bytes{0x02, 0x11, 0x01, 0x01, 0x01});
}

TEST_CASE("cobs output never contains a zero byte") {
  Bytes in{0x00, 0x11, 0x00, 0x00, 0x22};
  Bytes enc = encode(in);
  for (auto b : enc) CHECK(b != 0x00);
}

TEST_CASE("cobs round-trips arbitrary data") {
  Bytes in;
  for (int i = 0; i < 600; ++i) in.push_back(static_cast<std::uint8_t>(i * 7));
  CHECK(decode(encode(in)) == in);
}

TEST_CASE("cobs round-trips a 254-byte non-zero run boundary") {
  Bytes in;
  for (int i = 0; i < 254; ++i) in.push_back(static_cast<std::uint8_t>(1 + i % 254));
  Bytes enc = encode(in);
  CHECK(enc.size() == 255);       // one 0xFF group, no extra overhead byte
  CHECK(enc[0] == 0xFF);
  CHECK(decode(enc) == in);
}

TEST_CASE("cobs decode rejects a code that overruns the block") {
  Bytes bad{0x05, 0x11};  // code says 4 more data bytes, only 1 present
  Bytes out(8);
  auto r = hubproto::cobs_decode(bad.data(), bad.size(), out.data(), out.size());
  CHECK_FALSE(r.ok);
}

TEST_CASE("cobs encode reports overflow instead of writing past the buffer") {
  Bytes in{0x11, 0x22, 0x33};
  std::uint8_t tiny[2];
  auto r = hubproto::cobs_encode(in.data(), in.size(), tiny, sizeof(tiny));
  CHECK_FALSE(r.ok);
}
