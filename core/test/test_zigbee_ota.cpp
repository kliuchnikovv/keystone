#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "core/zigbee_ota.hpp"

namespace {

using core::zota::FileHeader;

void put_u16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void put_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

std::uint16_t get_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

std::uint32_t get_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// A header that parse_header() accepts: magic, version, length 56, no optional
// fields, image_size == 56 (an empty payload).
std::array<std::uint8_t, 128> make_header(std::uint32_t image_size = 56) {
  std::array<std::uint8_t, 128> buf{};
  put_u32(buf.data() + 0, core::zota::kFileMagic);
  put_u16(buf.data() + 4, core::zota::kHeaderVersion);
  put_u16(buf.data() + 6, static_cast<std::uint16_t>(core::zota::kBaseHeaderSize));
  put_u16(buf.data() + 8, 0);              // field_control
  put_u16(buf.data() + 10, 0x117C);        // manufacturer_code
  put_u16(buf.data() + 12, 0x0100);        // image_type
  put_u32(buf.data() + 14, 0x00010203u);   // file_version
  put_u16(buf.data() + 18, 0x0002);        // stack_version
  put_u32(buf.data() + 52, image_size);    // image_size
  return buf;
}

const std::uint8_t kPayload[] = {0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD};

}  // namespace

TEST_CASE("crc32 matches the zlib (IEEE 802.3) test vectors") {
  const char* empty = "";
  CHECK(core::zota::crc32(reinterpret_cast<const std::uint8_t*>(empty), 0) == 0u);

  const char* digits = "123456789";
  CHECK(core::zota::crc32(reinterpret_cast<const std::uint8_t*>(digits), 9) == 0xCBF43926u);

  const char* fox = "The quick brown fox jumps over the lazy dog";
  CHECK(core::zota::crc32(reinterpret_cast<const std::uint8_t*>(fox), 43) == 0x414FA339u);
}

TEST_CASE("the incremental CRC equals the one-shot CRC") {
  const char* digits = "123456789";
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(digits);

  core::zota::Crc32 crc;
  crc.update(bytes, 4);
  crc.update(bytes + 4, 5);
  crc.update(bytes + 9, 0);
  CHECK(crc.value() == 0xCBF43926u);
  CHECK(crc.value() == core::zota::crc32(bytes, 9));

  // reset() starts a new digest.
  crc.reset();
  CHECK(crc.value() == 0u);
  crc.update(bytes, 9);
  CHECK(crc.value() == 0xCBF43926u);

  // Splitting the input anywhere gives the same digest.
  core::zota::Crc32 chunked;
  chunked.update(bytes, 1);
  chunked.update(bytes + 1, 3);
  chunked.update(bytes + 4, 5);
  CHECK(chunked.value() == 0xCBF43926u);
}

TEST_CASE("build_file produces a file parse_header reads back") {
  std::array<std::uint8_t, 256> buf{};
  const std::size_t n = core::zota::build_file(buf.data(), buf.size(), kPayload, sizeof(kPayload),
                                                0x117C, 0x0100, 0x00010203u, 0x0002);
  CHECK(n == sizeof(kPayload) + core::zota::kFileOverhead);

  FileHeader hdr;
  REQUIRE(core::zota::parse_header(buf.data(), n, hdr));
  CHECK(hdr.header_version == core::zota::kHeaderVersion);
  CHECK(hdr.header_length == core::zota::kBaseHeaderSize);
  CHECK(hdr.field_control == 0);
  CHECK(hdr.manufacturer_code == 0x117C);
  CHECK(hdr.image_type == 0x0100);
  CHECK(hdr.file_version == 0x00010203u);
  CHECK(hdr.stack_version == 0x0002);
  CHECK(hdr.image_size == n);
  CHECK(hdr.payload_offset == core::zota::kBaseHeaderSize);
  // payload_size is the region after the header (sub-element header + image + crc),
  // i.e. image_size - header_length.
  CHECK(hdr.payload_size == n - core::zota::kBaseHeaderSize);
  CHECK(hdr.payload_offset + hdr.payload_size == hdr.image_size);
  CHECK(core::zota::verify_crc(buf.data(), n));

  // The upgrade image sub-element holds the payload verbatim.
  CHECK(get_u16(buf.data() + core::zota::kBaseHeaderSize) == core::zota::kTagUpgradeImage);
  CHECK(get_u32(buf.data() + core::zota::kBaseHeaderSize + 2) == sizeof(kPayload));
  CHECK(std::memcmp(buf.data() + core::zota::kBaseHeaderSize + core::zota::kSubElementHeaderSize,
                     kPayload, sizeof(kPayload)) == 0);
}

TEST_CASE("parse_header rejects a malformed header") {
  FileHeader hdr;

  SUBCASE("wrong magic") {
    auto buf = make_header();
    put_u32(buf.data(), 0xDEADBEEFu);
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize, hdr));
  }

  SUBCASE("bad header version") {
    auto buf = make_header();
    put_u16(buf.data() + 4, 0x0200);
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize, hdr));
  }

  SUBCASE("header shorter than the fixed part") {
    auto buf = make_header();
    put_u16(buf.data() + 6, static_cast<std::uint16_t>(core::zota::kBaseHeaderSize - 1));
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize, hdr));
  }

  SUBCASE("header longer than the data we have") {
    auto buf = make_header();
    put_u16(buf.data() + 6, 200);
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize, hdr));
  }

  SUBCASE("image smaller than the header") {
    auto buf = make_header(10);
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize, hdr));
  }

  SUBCASE("optional fields do not fit in the header") {
    auto buf = make_header();
    put_u16(buf.data() + 8, 0x0002);  // device_specific_file needs 8 more bytes
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize, hdr));
  }

  SUBCASE("optional fields fit once the header is long enough") {
    auto buf = make_header();
    put_u16(buf.data() + 6, static_cast<std::uint16_t>(core::zota::kBaseHeaderSize + 8));
    put_u32(buf.data() + 52, static_cast<std::uint32_t>(core::zota::kBaseHeaderSize + 8));
    put_u16(buf.data() + 8, 0x0002);
    REQUIRE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize + 8, hdr));
    CHECK(hdr.field_control == 0x0002);
    CHECK(hdr.header_length == core::zota::kBaseHeaderSize + 8);
  }

  SUBCASE("fewer bytes than a header") {
    auto buf = make_header();
    CHECK_FALSE(core::zota::parse_header(buf.data(), core::zota::kBaseHeaderSize - 1, hdr));
  }
}

TEST_CASE("verify_crc rejects a flipped byte") {
  std::array<std::uint8_t, 256> buf{};
  const std::size_t n = core::zota::build_file(buf.data(), buf.size(), kPayload, sizeof(kPayload),
                                                0x117C, 0x0100, 1u, 0);
  REQUIRE(core::zota::verify_crc(buf.data(), n));

  buf[n / 2] = static_cast<std::uint8_t>(buf[n / 2] ^ 0x01);
  CHECK_FALSE(core::zota::verify_crc(buf.data(), n));

  // Too short to hold a CRC at all.
  CHECK_FALSE(core::zota::verify_crc(buf.data(), core::zota::kBaseHeaderSize));
}

TEST_CASE("device_accepts needs the manufacturer and the image type to match") {
  FileHeader hdr;
  hdr.manufacturer_code = 0x117C;
  hdr.image_type = 0x0100;

  CHECK(core::zota::device_accepts(hdr, 0x117C, 0x0100));
  CHECK_FALSE(core::zota::device_accepts(hdr, 0x117D, 0x0100));
  CHECK_FALSE(core::zota::device_accepts(hdr, 0x117C, 0x0200));
  CHECK_FALSE(core::zota::device_accepts(hdr, 0x0000, 0x0000));
}

TEST_CASE("unwrap_vendor_container finds the inner OTA file") {
  std::array<std::uint8_t, 128> container{};
  std::memcpy(container.data(), "NGIS", 4);
  const std::uint32_t inner_offset = 24;
  const std::uint32_t inner_size = 32;
  put_u32(container.data() + 16, inner_offset);
  put_u32(container.data() + 20, inner_size);

  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  REQUIRE(core::zota::unwrap_vendor_container(container.data(), container.size(), offset, size));
  CHECK(offset == inner_offset);
  CHECK(size == inner_size);

  SUBCASE("a buffer that is not an NGIS container is rejected") {
    std::array<std::uint8_t, 128> other{};
    CHECK_FALSE(core::zota::unwrap_vendor_container(other.data(), other.size(), offset, size));
  }

  SUBCASE("an inner range that does not fit is rejected") {
    put_u32(container.data() + 20, static_cast<std::uint32_t>(container.size() + 1));
    CHECK_FALSE(core::zota::unwrap_vendor_container(container.data(), container.size(), offset, size));
  }
}

TEST_CASE("a real .ota file inside an NGIS container parses") {
  std::array<std::uint8_t, 128> inner{};
  const std::size_t inner_len = core::zota::build_file(inner.data(), inner.size(), kPayload,
                                                        sizeof(kPayload), 0x117C, 0x0100, 7u, 0);
  REQUIRE(inner_len > 0);

  std::array<std::uint8_t, 256> container{};
  std::memcpy(container.data(), "NGIS", 4);
  const std::uint32_t inner_offset = 24;
  put_u32(container.data() + 16, inner_offset);
  put_u32(container.data() + 20, static_cast<std::uint32_t>(inner_len));
  std::memcpy(container.data() + inner_offset, inner.data(), inner_len);

  std::uint32_t offset = 0;
  std::uint32_t size = 0;
  REQUIRE(core::zota::unwrap_vendor_container(container.data(), container.size(), offset, size));
  CHECK(offset == inner_offset);
  CHECK(size == inner_len);

  FileHeader hdr;
  REQUIRE(core::zota::parse_header(container.data() + offset, size, hdr));
  CHECK(hdr.manufacturer_code == 0x117C);
  CHECK(hdr.image_type == 0x0100);
  CHECK(hdr.payload_size == size - core::zota::kBaseHeaderSize);
}

TEST_CASE("build_file refuses a buffer that is too small") {
  std::array<std::uint8_t, 64> buf{};
  CHECK(core::zota::build_file(buf.data(), buf.size(), kPayload, sizeof(kPayload), 0x117C, 0x0100,
                               1u, 0) == 0);

  // An empty payload is not an upgrade file.
  CHECK(core::zota::build_file(buf.data(), buf.size(), kPayload, 0, 0x117C, 0x0100, 1u, 0) == 0);
}
