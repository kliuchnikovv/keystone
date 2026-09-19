#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "hubproto/frame.hpp"

using hubproto::FrameReader;
using hubproto::Header;
using hubproto::MsgType;

namespace {

std::vector<std::uint8_t> build_frame(const Header& h,
                                      const std::vector<std::uint8_t>& payload) {
  std::array<std::uint8_t, hubproto::kMaxFrameSize> buf{};
  auto r = hubproto::encode_frame(h, payload.data(), payload.size(), buf.data(),
                                  buf.size());
  REQUIRE(r.ok);
  return std::vector<std::uint8_t>(buf.begin(), buf.begin() + r.len);
}

Header sample_header() {
  Header h{};
  h.version = 0x01;
  h.type = MsgType::State;
  h.chip_id = 0x03;
  h.boot_id = 0x09;
  h.seq = 0x1234;
  h.entity_id = 0x0007;
  return h;
}

}  // namespace

TEST_CASE("encode_frame ends with a single 0x00 delimiter and no interior zero") {
  Header h = sample_header();
  std::vector<std::uint8_t> payload{0x00, 0x11, 0x00, 0x22};  // contains zeros
  auto frame = build_frame(h, payload);

  REQUIRE(frame.size() >= 2);
  CHECK(frame.back() == 0x00);
  for (std::size_t i = 0; i + 1 < frame.size(); ++i) CHECK(frame[i] != 0x00);
}

TEST_CASE("frame round-trips header and payload through the reader") {
  Header h = sample_header();
  std::vector<std::uint8_t> payload{0xDE, 0xAD, 0x00, 0xBE, 0xEF};
  auto frame = build_frame(h, payload);

  FrameReader<> reader;
  bool ready = false;
  for (auto b : frame) ready = reader.push(b);

  REQUIRE(ready);
  CHECK(reader.header().type == MsgType::State);
  CHECK(reader.header().chip_id == 0x03);
  CHECK(reader.header().boot_id == 0x09);
  CHECK(reader.header().seq == 0x1234);
  CHECK(reader.header().entity_id == 0x0007);
  REQUIRE(reader.payload_len() == payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i) {
    CHECK(reader.payload()[i] == payload[i]);
  }
}

TEST_CASE("empty-payload frame (e.g. HEARTBEAT) round-trips") {
  Header h = sample_header();
  h.type = MsgType::Heartbeat;
  auto frame = build_frame(h, {});

  FrameReader<> reader;
  bool ready = false;
  for (auto b : frame) ready = reader.push(b);
  REQUIRE(ready);
  CHECK(reader.header().type == MsgType::Heartbeat);
  CHECK(reader.payload_len() == 0);
}

TEST_CASE("a corrupted byte fails the CRC and the frame is dropped") {
  Header h = sample_header();
  auto frame = build_frame(h, {0x11, 0x22, 0x33});
  frame[2] ^= 0xFF;  // flip a byte inside the COBS block

  FrameReader<> reader;
  bool ready = false;
  for (auto b : frame) ready = reader.push(b) || ready;
  CHECK_FALSE(ready);
}

TEST_CASE("reader resyncs after leading garbage and a stray delimiter") {
  Header h = sample_header();
  auto frame = build_frame(h, {0x01, 0x02, 0x03});

  FrameReader<> reader;
  // Garbage bytes terminated by a delimiter: the delimiter flushes the junk
  // (as a failed decode) and the next clean frame must resync on it.
  for (int junk : {0x42, 0x99, 0x7F, 0x00}) reader.push(static_cast<std::uint8_t>(junk));

  bool ready = false;
  for (auto b : frame) ready = reader.push(b);
  REQUIRE(ready);
  CHECK(reader.header().seq == 0x1234);
  CHECK(reader.payload_len() == 3);
}

TEST_CASE("two frames back to back both decode") {
  Header h1 = sample_header();
  h1.seq = 0x0001;
  Header h2 = sample_header();
  h2.seq = 0x0002;
  h2.type = MsgType::Heartbeat;

  auto f1 = build_frame(h1, {0xAA});
  auto f2 = build_frame(h2, {});

  FrameReader<> reader;
  int frames = 0;
  std::array<std::uint16_t, 2> seqs{};
  for (auto b : f1) {
    if (reader.push(b)) seqs[static_cast<std::size_t>(frames++)] = reader.header().seq;
  }
  for (auto b : f2) {
    if (reader.push(b)) seqs[static_cast<std::size_t>(frames++)] = reader.header().seq;
  }
  REQUIRE(frames == 2);
  CHECK(seqs[0] == 0x0001);
  CHECK(seqs[1] == 0x0002);
}
