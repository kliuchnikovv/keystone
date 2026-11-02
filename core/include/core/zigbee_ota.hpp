// Portable Zigbee OTA upgrade file (.ota) parser / validator / builder.
//
// The Zigbee Cluster Library (ZCL r8 §5.6) defines the upgrade file format: a
// fixed 56-byte header, optional fields behind a field-control bitmask, one or
// more sub-elements (the upgrade image is tag 0x0000) and a CRC-32 over
// everything that precedes it. All multi-byte fields are little-endian, which
// is what zigpy (ota/image.py) and real devices use.
//
// IKEA ships the very same file wrapped in a proprietary "NGIS" container;
// unwrap_vendor_container() peels it off.
//
// No dependencies, no allocation, no exceptions: the hub can check a candidate
// image before it is pushed to a co-processor, and the host tests compare the
// CRC against python's zlib.crc32.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace core {
namespace zota {

inline constexpr std::uint32_t kFileMagic = 0x0BEEF11E;
inline constexpr std::uint16_t kHeaderVersion = 0x0100;
inline constexpr std::size_t   kHeaderStringSize = 32;
inline constexpr std::size_t   kBaseHeaderSize = 56;      // fixed part of the header
inline constexpr std::size_t   kSubElementHeaderSize = 6;  // tag(u16) + length(u32)
inline constexpr std::uint16_t kTagUpgradeImage = 0x0000;
inline constexpr std::size_t   kCrcSize = 4;
inline constexpr std::size_t   kFileOverhead = kBaseHeaderSize + kSubElementHeaderSize + kCrcSize;

struct FileHeader {
  std::uint16_t header_version = 0;
  std::uint16_t header_length = 0;     // total header size incl. optional fields
  std::uint16_t field_control = 0;
  std::uint16_t manufacturer_code = 0;
  std::uint16_t image_type = 0;
  std::uint32_t file_version = 0;
  std::uint16_t stack_version = 0;
  std::uint32_t image_size = 0;        // bytes the device receives (header + payload + crc)
  std::uint32_t payload_offset = 0;    // first byte after the header
  std::uint32_t payload_size = 0;      // image_size - header_length
};

namespace detail {

// The CRC-32 table (reflected, polynomial 0xEDB88320) built at compile time so
// the header stays dependency-free.
struct Crc32Table {
  std::uint32_t v[256];
};

constexpr Crc32Table make_crc32_table() noexcept {
  Crc32Table t{};
  for (std::size_t i = 0; i < 256; ++i) {
    std::uint32_t c = static_cast<std::uint32_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      c = ((c & 1u) != 0) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
    t.v[i] = c;
  }
  return t;
}

inline constexpr Crc32Table kCrc32Table = make_crc32_table();

inline std::uint16_t read_u16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8));
}

inline std::uint32_t read_u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void write_u16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

inline void write_u32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFFu);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

// Bytes the optional header fields take, given the field-control bitmask:
// bit0 security_credential_version (u8), bit1 device_specific_file (EUI64),
// bit2 hardware versions (min + max, 2 x u16).
inline std::size_t optional_bytes(std::uint16_t field_control) noexcept {
  std::size_t n = 0;
  if ((field_control & 0x0001u) != 0) n += 1;  // security_credential_version
  if ((field_control & 0x0002u) != 0) n += 8;  // device_specific_file
  if ((field_control & 0x0004u) != 0) n += 4;  // min/max hardware version
  return n;
}

// The vendor container header: "NGIS" + the inner file's offset (u32 at byte 16)
// and size (u32 at byte 20).
inline constexpr std::size_t kVendorContainerHeaderSize = 24;

// The CRC state is kept without the final xor; the one-shot and the incremental
// versions share this fold so they cannot drift apart.
inline constexpr std::uint32_t kCrc32Init = 0xFFFFFFFFu;

inline std::uint32_t crc32_fold(std::uint32_t state, const std::uint8_t* data,
                                std::size_t len) noexcept {
  for (std::size_t i = 0; i < len; ++i) {
    state = kCrc32Table.v[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
  }
  return state;
}

inline std::uint32_t crc32_finish(std::uint32_t state) noexcept { return state ^ 0xFFFFFFFFu; }

}  // namespace detail

// CRC-32 (IEEE 802.3 / zlib): reflected, polynomial 0xEDB88320, init 0xFFFFFFFF,
// final xor 0xFFFFFFFF. Matches python's zlib.crc32 byte for byte.
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t len) noexcept {
  return detail::crc32_finish(detail::crc32_fold(detail::kCrc32Init, data, len));
}

// Incremental CRC-32 (IEEE). reset() starts a new digest; update() folds more
// bytes in; value() returns the final digest (same as zlib.crc32 over the whole
// input). The one-shot crc32(data, len) must equal reset()+update(data,len)+value().
//
// A file that lives in flash is read in chunks, so the CRC cannot be taken over
// the whole buffer at once.
class Crc32 {
 public:
  void reset() noexcept { state_ = detail::kCrc32Init; }

  void update(const std::uint8_t* data, std::size_t len) noexcept {
    state_ = detail::crc32_fold(state_, data, len);
  }

  std::uint32_t value() const noexcept { return detail::crc32_finish(state_); }

 private:
  std::uint32_t state_ = detail::kCrc32Init;
};

// Parse the OTA file header from `data` (at least kBaseHeaderSize bytes, more if the
// header has optional fields). Returns false when: the magic is wrong, header_version
// is not kHeaderVersion, header_length < kBaseHeaderSize, header_length > len,
// image_size < header_length, or the optional-field flags in field_control do not fit
// inside header_length. On success fills `out` (payload_offset = header_length,
// payload_size = image_size - header_length).
inline bool parse_header(const std::uint8_t* data, std::size_t len, FileHeader& out) noexcept {
  if (data == nullptr || len < kBaseHeaderSize) return false;
  if (detail::read_u32(data) != kFileMagic) return false;

  const std::uint16_t version = detail::read_u16(data + 4);
  if (version != kHeaderVersion) return false;

  const std::uint16_t header_length = detail::read_u16(data + 6);
  if (static_cast<std::size_t>(header_length) < kBaseHeaderSize) return false;
  if (static_cast<std::size_t>(header_length) > len) return false;

  const std::uint16_t field_control = detail::read_u16(data + 8);
  if (kBaseHeaderSize + detail::optional_bytes(field_control) > static_cast<std::size_t>(header_length)) {
    return false;
  }

  const std::uint32_t image_size = detail::read_u32(data + 52);
  if (image_size < static_cast<std::uint32_t>(header_length)) return false;

  out.header_version = version;
  out.header_length = header_length;
  out.field_control = field_control;
  out.manufacturer_code = detail::read_u16(data + 10);
  out.image_type = detail::read_u16(data + 12);
  out.file_version = detail::read_u32(data + 14);
  out.stack_version = detail::read_u16(data + 18);
  out.image_size = image_size;
  out.payload_offset = header_length;
  out.payload_size = image_size - static_cast<std::uint32_t>(header_length);
  return true;
}

// True when the last 4 bytes of `data` are the CRC32 of all preceding bytes
// (little-endian on the wire). `len` must be >= kBaseHeaderSize + kCrcSize.
inline bool verify_crc(const std::uint8_t* data, std::size_t len) noexcept {
  if (data == nullptr || len < kBaseHeaderSize + kCrcSize) return false;
  const std::uint32_t want = detail::read_u32(data + len - kCrcSize);
  return crc32(data, len - kCrcSize) == want;
}

// True when a device with this manufacturer code / image type would accept the image
// (Zigbee OTA rule: both must match the file header).
inline bool device_accepts(const FileHeader& hdr, std::uint16_t device_manufacturer,
                           std::uint16_t device_image_type) noexcept {
  return hdr.manufacturer_code == device_manufacturer && hdr.image_type == device_image_type;
}

// Build a complete .ota file into `out`: base header + one upgrade-image sub-element
// (tag 0x0000) holding `payload`, then the CRC32 of everything before it.
// `image_size` in the header counts header + sub-element + crc.
// Returns the number of bytes written, or 0 when `cap` is too small or payload_len
// is 0. The manufacturer/image/file version/stack version go into the header.
inline std::size_t build_file(std::uint8_t* out, std::size_t cap, const std::uint8_t* payload,
                              std::size_t payload_len, std::uint16_t manufacturer_code,
                              std::uint16_t image_type, std::uint32_t file_version,
                              std::uint16_t stack_version) noexcept {
  if (out == nullptr || payload == nullptr) return 0;
  if (payload_len == 0) return 0;
  if (payload_len > 0xFFFFFFFFu) return 0;

  const std::size_t total = kFileOverhead + payload_len;
  if (cap < total) return 0;

  detail::write_u32(out + 0, kFileMagic);
  detail::write_u16(out + 4, kHeaderVersion);
  detail::write_u16(out + 6, static_cast<std::uint16_t>(kBaseHeaderSize));
  detail::write_u16(out + 8, 0);  // field_control: no optional fields
  detail::write_u16(out + 10, manufacturer_code);
  detail::write_u16(out + 12, image_type);
  detail::write_u32(out + 14, file_version);
  detail::write_u16(out + 18, stack_version);
  std::memset(out + 20, 0, kHeaderStringSize);  // header_string, zero padded
  detail::write_u32(out + 52, static_cast<std::uint32_t>(total));

  // The upgrade image sub-element: tag 0x0000, length, then the payload.
  detail::write_u16(out + kBaseHeaderSize, kTagUpgradeImage);
  detail::write_u32(out + kBaseHeaderSize + 2, static_cast<std::uint32_t>(payload_len));
  std::memcpy(out + kBaseHeaderSize + kSubElementHeaderSize, payload, payload_len);

  // The CRC covers everything that comes before it.
  const std::uint32_t crc = crc32(out, total - kCrcSize);
  detail::write_u32(out + total - kCrcSize, crc);
  return total;
}

// IKEA ships its .ota files inside a proprietary container whose first 4 bytes are
// "NGIS"; the inner Zigbee OTA file starts at the u32 offset at byte 16 and is
// `size` bytes long (both little-endian). Returns false when the container is not an
// NGIS one or the inner range does not fit. On success `out_offset`/`out_size` are the
// inner file's range (the caller then calls parse_header on data + out_offset).
inline bool unwrap_vendor_container(const std::uint8_t* data, std::size_t len,
                                    std::uint32_t& out_offset, std::uint32_t& out_size) noexcept {
  if (data == nullptr || len < detail::kVendorContainerHeaderSize) return false;
  if (std::memcmp(data, "NGIS", 4) != 0) return false;

  const std::uint32_t offset = detail::read_u32(data + 16);
  const std::uint32_t size = detail::read_u32(data + 20);
  if (static_cast<std::size_t>(offset) < detail::kVendorContainerHeaderSize) return false;
  const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(size);
  if (end > static_cast<std::uint64_t>(len)) return false;

  out_offset = offset;
  out_size = size;
  return true;
}

}  // namespace zota
}  // namespace core
