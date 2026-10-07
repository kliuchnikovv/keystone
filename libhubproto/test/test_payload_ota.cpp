#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include "hubproto/frame.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

using namespace hubproto;

TEST_CASE("OTA message types are known and sit at the documented codes") {
  CHECK(static_cast<std::uint8_t>(MsgType::OtaBegin) == 0x50);
  CHECK(static_cast<std::uint8_t>(MsgType::OtaAbort) == 0x56);
  for (std::uint8_t t = 0x50; t <= 0x58; ++t) CHECK(is_known_type(t));
  CHECK_FALSE(is_known_type(0x59));
}

TEST_CASE("OTA_INFO has the documented layout and round-trips") {
  OtaInfo in{};
  in.running_slot = 1;
  in.confirmed = true;
  in.last_ota_slot = 0;
  for (std::size_t i = 0; i < kOtaSha256Size; ++i) in.last_ota_sha256[i] = static_cast<std::uint8_t>(0xA0 + i);
  std::array<std::uint8_t, 64> buf{};
  auto r = encode_ota_info(in, buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(r.len == 35);
  CHECK(buf[0] == 1);
  CHECK(buf[1] == 0x01);
  CHECK(buf[2] == 0);
  CHECK(buf[3] == 0xA0);
  OtaInfo out{};
  REQUIRE(decode_ota_info(buf.data(), r.len, out));
  CHECK(out.running_slot == 1);
  CHECK(out.confirmed);
  CHECK(out.last_ota_slot == 0);
  CHECK(out.last_ota_sha256[31] == 0xA0 + 31);
  CHECK_FALSE(decode_ota_info(buf.data(), r.len - 1, out));
  CHECK_FALSE(encode_ota_info(in, buf.data(), 34).ok);
  in.confirmed = false;
  encode_ota_info(in, buf.data(), buf.size());
  CHECK(buf[1] == 0);
}

TEST_CASE("OTA_BEGIN has the exact little-endian layout and round-trips") {
  OtaBegin in{};
  in.total_size = 0x00123456;
  for (std::size_t i = 0; i < kOtaSha256Size; ++i) in.sha256[i] = static_cast<std::uint8_t>(i);

  std::array<std::uint8_t, 64> buf{};
  auto r = encode_ota_begin(in, buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(r.len == 36);
  CHECK(buf[0] == 0x56);
  CHECK(buf[1] == 0x34);
  CHECK(buf[2] == 0x12);
  CHECK(buf[3] == 0x00);
  CHECK(buf[4] == 0x00);
  CHECK(buf[35] == 31);

  OtaBegin out{};
  REQUIRE(decode_ota_begin(buf.data(), r.len, out));
  CHECK(out.total_size == in.total_size);
  CHECK(out.sha256[31] == 31);
}

TEST_CASE("OTA_BEGIN rejects short, long and under-sized buffers") {
  std::array<std::uint8_t, 40> buf{};
  OtaBegin out{};
  CHECK_FALSE(decode_ota_begin(buf.data(), 35, out));
  CHECK_FALSE(decode_ota_begin(buf.data(), 37, out));
  OtaBegin in{};
  CHECK_FALSE(encode_ota_begin(in, buf.data(), 35).ok);
}

TEST_CASE("OTA_BEGIN_ACK round-trips and rejects an unknown status") {
  std::array<std::uint8_t, 3> buf{};
  auto r = encode_ota_begin_ack({OtaStatus::Ok, 240}, buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(buf[0] == 0);
  CHECK(buf[1] == 240);
  CHECK(buf[2] == 0);
  OtaBeginAck out{};
  REQUIRE(decode_ota_begin_ack(buf.data(), r.len, out));
  CHECK(out.status == OtaStatus::Ok);
  CHECK(out.chunk_size == 240);

  buf[0] = 0x99;
  CHECK_FALSE(decode_ota_begin_ack(buf.data(), 3, out));
  CHECK_FALSE(decode_ota_begin_ack(buf.data(), 2, out));
}

TEST_CASE("OTA_CHUNK carries offset + data and decodes zero-copy") {
  const std::array<std::uint8_t, 3> data{0xAA, 0xBB, 0xCC};
  std::array<std::uint8_t, 255> buf{};
  auto r = encode_ota_chunk(0x01020304, data.data(), data.size(), buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(r.len == 7);
  CHECK(buf[0] == 0x04);
  CHECK(buf[3] == 0x01);

  OtaChunkView v{};
  REQUIRE(decode_ota_chunk(buf.data(), r.len, v));
  CHECK(v.offset == 0x01020304);
  CHECK(v.len == 3);
  CHECK(v.data == buf.data() + 4);
  CHECK(v.data[2] == 0xCC);
}

TEST_CASE("OTA_CHUNK enforces the size limits both ways") {
  std::array<std::uint8_t, 300> data{};
  std::array<std::uint8_t, 300> buf{};
  CHECK(encode_ota_chunk(0, data.data(), kOtaMaxChunk, buf.data(), buf.size()).ok);
  CHECK_FALSE(encode_ota_chunk(0, data.data(), kOtaMaxChunk + 1, buf.data(), buf.size()).ok);
  CHECK_FALSE(encode_ota_chunk(0, data.data(), 0, buf.data(), buf.size()).ok);
  CHECK_FALSE(encode_ota_chunk(0, data.data(), 10, buf.data(), 13).ok);

  OtaChunkView v{};
  CHECK_FALSE(decode_ota_chunk(buf.data(), 4, v));                    // no data
  CHECK_FALSE(decode_ota_chunk(buf.data(), 4 + kOtaMaxChunk + 1, v));  // too much
  CHECK(decode_ota_chunk(buf.data(), 4 + kOtaMaxChunk, v));
}

TEST_CASE("a full OTA_CHUNK still fits one frame payload") {
  CHECK(kOtaChunkHeader + kOtaMaxChunk <= hubproto::kMaxPayloadSize);
}

TEST_CASE("OTA_CHUNK_ACK round-trips") {
  std::array<std::uint8_t, 5> buf{};
  auto r = encode_ota_chunk_ack({OtaStatus::BadOffset, 0xDEADBEEF}, buf.data(), buf.size());
  REQUIRE(r.ok);
  CHECK(buf[0] == 4);
  CHECK(buf[1] == 0xEF);
  OtaChunkAck out{};
  REQUIRE(decode_ota_chunk_ack(buf.data(), r.len, out));
  CHECK(out.status == OtaStatus::BadOffset);
  CHECK(out.next_offset == 0xDEADBEEF);
  CHECK_FALSE(decode_ota_chunk_ack(buf.data(), 4, out));
}

TEST_CASE("OTA_END_ACK round-trips") {
  std::array<std::uint8_t, 1> buf{};
  auto r = encode_ota_end_ack(OtaStatus::VerifyFail, buf.data(), buf.size());
  REQUIRE(r.ok);
  OtaStatus s{};
  REQUIRE(decode_ota_end_ack(buf.data(), r.len, s));
  CHECK(s == OtaStatus::VerifyFail);
  CHECK_FALSE(decode_ota_end_ack(buf.data(), 0, s));
}
