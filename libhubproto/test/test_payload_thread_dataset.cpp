#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/frame.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

using namespace hubproto;

TEST_CASE("the Thread dataset messages sit at the documented codes") {
  CHECK(static_cast<std::uint8_t>(MsgType::ThreadDatasetReq) == 0x62);
  CHECK(static_cast<std::uint8_t>(MsgType::ThreadDataset) == 0x63);
  CHECK(is_known_type(0x62));
  CHECK(is_known_type(0x63));
  CHECK_FALSE(is_known_type(0x64));
}

TEST_CASE("THREAD_DATASET carries the length and the TLVs, and round-trips") {
  // A few TLVs: channel 15, a PAN id, a network key.
  const std::array<std::uint8_t, 13> tlvs = {0x02, 0x03, 15, 0x00, 0x00,
                                             0x03, 0x02, 0x12, 0x34,
                                             0x07, 0x02, 0xaa, 0xbb};
  std::array<std::uint8_t, 32> buf{};
  auto r = encode_thread_dataset(tlvs.data(), tlvs.size(), buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(r.len == 1 + tlvs.size());
  CHECK(buf[0] == tlvs.size());

  ThreadDatasetView view{};
  REQUIRE(decode_thread_dataset(buf.data(), r.len, view));
  CHECK(view.len == tlvs.size());
  for (std::size_t i = 0; i < tlvs.size(); ++i) CHECK(view.bytes[i] == tlvs[i]);
}

TEST_CASE("THREAD_DATASET refuses what it cannot carry") {
  std::array<std::uint8_t, 32> buf{};
  const std::uint8_t one = 0x42;
  // Nothing to send.
  CHECK_FALSE(encode_thread_dataset(&one, 0, buf.data(), buf.size()).ok);
  CHECK_FALSE(encode_thread_dataset(nullptr, 4, buf.data(), buf.size()).ok);
  // More than a Thread dataset can be.
  std::array<std::uint8_t, kThreadDatasetMax + 1> big{};
  CHECK_FALSE(encode_thread_dataset(big.data(), big.size(), buf.data(), buf.size()).ok);
  // The room in the buffer: one byte for the length, and none for the data.
  CHECK_FALSE(encode_thread_dataset(&one, 1, buf.data(), 1).ok);

  // A length that does not match the bytes.
  buf[0] = 4;
  ThreadDatasetView view{};
  CHECK(decode_thread_dataset(buf.data(), 5, view));      // exactly 1 + 4
  CHECK_FALSE(decode_thread_dataset(buf.data(), 4, view));  // too short
  CHECK_FALSE(decode_thread_dataset(buf.data(), 6, view));  // too long
  buf[0] = 0;
  CHECK_FALSE(decode_thread_dataset(buf.data(), 6, view));  // an empty dataset
  buf[0] = kThreadDatasetMax + 1;
  CHECK_FALSE(decode_thread_dataset(buf.data(), 6, view));  // a length we cannot have
}

TEST_CASE("the Thread dataset fits one frame") {
  CHECK(1 + kThreadDatasetMax <= kMaxPayloadSize);
}

TEST_CASE("a real-sized Thread dataset survives the frame layer") {
  // The dataset a Thread leader really has: 109 bytes of TLVs with zero bytes
  // in them (the network key is mostly zeros) — the case the COBS framing has
  // to survive. This is the frame the co-processor sends to the brain.
  std::array<std::uint8_t, 109> tlvs{};
  for (std::size_t i = 0; i < tlvs.size(); ++i) {
    tlvs[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
  }
  tlvs[7] = 0x00;
  tlvs[8] = 0x00;

  std::array<std::uint8_t, 1 + kThreadDatasetMax> payload{};
  auto enc = encode_thread_dataset(tlvs.data(), tlvs.size(), payload.data(), payload.size());
  REQUIRE(enc.ok);
  REQUIRE(enc.len == 1 + tlvs.size());

  Header h{};
  h.version = kProtocolVersion;
  h.type = MsgType::ThreadDataset;
  h.chip_id = 0x02;
  h.boot_id = 4;
  h.seq = 17;
  h.entity_id = 0;

  std::array<std::uint8_t, kMaxFrameSize> frame{};
  auto fr = encode_frame(h, payload.data(), enc.len, frame.data(), frame.size());
  REQUIRE(fr.ok);
  // The frame is big but well inside the limits of the layer.
  CHECK(fr.len > 100);
  CHECK(fr.len <= kMaxFrameSize);

  // Byte by byte, the way the UART task hands the bytes over.
  FrameReader<> reader;
  bool ready = false;
  for (std::size_t i = 0; i < fr.len; ++i) {
    if (reader.push(frame[i])) ready = true;
  }
  REQUIRE(ready);
  CHECK(reader.bad_frames() == 0);
  CHECK(reader.header().type == MsgType::ThreadDataset);
  CHECK(reader.header().chip_id == 0x02);
  CHECK(reader.header().seq == 17);
  REQUIRE(reader.payload_len() == enc.len);

  ThreadDatasetView view{};
  REQUIRE(decode_thread_dataset(reader.payload(), reader.payload_len(), view));
  CHECK(view.len == tlvs.size());
  for (std::size_t i = 0; i < tlvs.size(); ++i) CHECK(view.bytes[i] == tlvs[i]);
}

TEST_CASE("a Thread dataset arrives whole when the bytes come in one piece") {
  // The same frame, but handed over in one chunk (the RX task may read it all
  // at once). The reader must not care how the bytes were cut up.
  std::array<std::uint8_t, 109> tlvs{};
  for (std::size_t i = 0; i < tlvs.size(); ++i) {
    tlvs[i] = static_cast<std::uint8_t>((i * 91u + 7u) & 0xFFu);
  }
  std::array<std::uint8_t, 1 + kThreadDatasetMax> payload{};
  auto enc = encode_thread_dataset(tlvs.data(), tlvs.size(), payload.data(), payload.size());
  REQUIRE(enc.ok);

  Header h{};
  h.version = kProtocolVersion;
  h.type = MsgType::ThreadDataset;
  h.chip_id = 0x02;
  std::array<std::uint8_t, kMaxFrameSize> frame{};
  auto fr = encode_frame(h, payload.data(), enc.len, frame.data(), frame.size());
  REQUIRE(fr.ok);

  FrameReader<> reader;
  bool ready = false;
  for (std::size_t i = 0; i < fr.len; ++i) {
    if (reader.push(frame[i])) ready = true;
  }
  REQUIRE(ready);
  ThreadDatasetView view{};
  REQUIRE(decode_thread_dataset(reader.payload(), reader.payload_len(), view));
  CHECK(view.len == tlvs.size());
}
