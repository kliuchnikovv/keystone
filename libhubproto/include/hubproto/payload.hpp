// Payload layer: capability model (SPEC §6) and payload codecs (SPEC §5).
//
// The model is Matter-cluster-shaped (SPEC Invariant 4): one descriptor feeds
// both the internal registry and the outward Matter bridge. All multi-byte
// integers are little-endian.
//
// Scope note: DECLARE is self-describing and fully decodable here. STATE and
// COMMAND carry raw value bytes whose length depends on each attribute's
// value_type, which is NOT on the wire — the caller (holding the registry)
// supplies it. Those codecs are added in a later step.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hubproto {

// Device type (DECLARE byte 2). Maps 1:1 to a Matter Device Type. SPEC §6.1.
enum class DeviceType : std::uint8_t {
  Light     = 0x01,
  Sensor    = 0x02,
  Switch    = 0x03,
  Cover     = 0x04,
  Lock      = 0x05,
  Climate   = 0x06,
  Occupancy = 0x07,
  Contact   = 0x08,
  Button    = 0x09,
};

// Value encoding of an attribute (SPEC §6.2).
enum class ValueType : std::uint8_t {
  Bool    = 0x01,  // 1 byte 0/1
  U8      = 0x02,  // 1 byte
  I16     = 0x03,  // 2 bytes LE
  U16     = 0x04,  // 2 bytes LE
  F32     = 0x05,  // 4 bytes IEEE754
  XyColor = 0x06,  // 4 bytes: u16 x, u16 y
  Enum8   = 0x07,  // 1 byte
};

// Attribute flags (DECLARE, per-attribute byte 2). SPEC §5.
inline constexpr std::uint8_t kFlagReadable   = 0x01;
inline constexpr std::uint8_t kFlagWritable   = 0x02;
inline constexpr std::uint8_t kFlagReportable = 0x04;

// CMD_ACK status (SPEC §5).
enum class CmdAckStatus : std::uint8_t {
  Ok          = 0,
  Fail        = 1,
  Unreachable = 2,
  Unsupported = 3,
};

// One attribute descriptor inside a DECLARE.
struct AttrDecl {
  std::uint8_t attr_id;
  ValueType    value_type;
  std::uint8_t flags;
};

struct EncodeResult {
  bool ok;
  std::size_t len;
};

// Wire size in bytes of a value of type `vt`. Returns 0 for an unrecognized type.
std::size_t value_size(ValueType vt) noexcept;

// ---- DECLARE (SPEC §5) ----------------------------------------------------
// Layout: entity_id(u16) device_type(u8) attr_count(u8) then count x
//         { attr_id(u8) value_type(u8) flags(u8) }.

// Encode a DECLARE payload for `count` attributes into `out` (capacity `cap`).
EncodeResult encode_declare(std::uint16_t entity_id, DeviceType device_type,
                            const AttrDecl* attrs, std::uint8_t count,
                            std::uint8_t* out, std::size_t cap) noexcept;

// Zero-copy view over a decoded DECLARE payload. `attrs` points into the input
// buffer, which must outlive the view.
struct DeclareView {
  std::uint16_t entity_id;
  DeviceType    device_type;
  std::uint8_t  attr_count;
  const std::uint8_t* attrs;  // count x 3 bytes
};

// Parse a DECLARE payload. Returns false if truncated or an attribute carries an
// unrecognized value_type.
bool decode_declare(const std::uint8_t* in, std::size_t len, DeclareView& out) noexcept;

// Read the i-th attribute of a decoded DECLARE (i < view.attr_count).
AttrDecl declare_attr_at(const DeclareView& view, std::uint8_t index) noexcept;

}  // namespace hubproto
