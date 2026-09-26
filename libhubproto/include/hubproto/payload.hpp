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
  U32     = 0x08,  // 4 bytes LE — e.g. cumulative energy in Wh
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

// ---- Value ----------------------------------------------------------------
// A single typed attribute value. `raw` holds the exact little-endian wire
// bytes (SPEC §6.2), so on-wire (de)serialization is a copy and the accessors
// reinterpret. POD, fits any value (max 4 bytes), no allocation.
struct Value {
  ValueType    type;
  std::uint8_t len;      // == value_size(type)
  std::uint8_t raw[4];

  static Value make_bool(bool v) noexcept;
  static Value make_u8(std::uint8_t v) noexcept;
  static Value make_i16(std::int16_t v) noexcept;
  static Value make_u16(std::uint16_t v) noexcept;
  static Value make_u32(std::uint32_t v) noexcept;
  static Value make_f32(float v) noexcept;
  static Value make_xy(std::uint16_t x, std::uint16_t y) noexcept;
  static Value make_enum8(std::uint8_t v) noexcept;

  bool          as_bool() const noexcept;
  std::uint8_t  as_u8() const noexcept;
  std::int16_t  as_i16() const noexcept;
  std::uint16_t as_u16() const noexcept;
  std::uint32_t as_u32() const noexcept;
  float         as_f32() const noexcept;
  void          as_xy(std::uint16_t& x, std::uint16_t& y) const noexcept;
  std::uint8_t  as_enum8() const noexcept;
};

// Two values are equal iff same type, same length, and equal wire bytes.
bool operator==(const Value& a, const Value& b) noexcept;
inline bool operator!=(const Value& a, const Value& b) noexcept { return !(a == b); }

// Interpret [in, in+value_size(vt)) as a Value of type `vt`. Returns false if
// `vt` is unknown or fewer than value_size(vt) bytes are available.
bool read_value(const std::uint8_t* in, std::size_t len, ValueType vt,
                Value& out) noexcept;

// ---- COMMAND (SPEC §5) ----------------------------------------------------
// Layout: entity_id(u16) attr_id(u8) then value bytes per value_type.
struct Command {
  std::uint16_t entity_id;
  std::uint8_t  attr_id;
  Value         value;
};

EncodeResult encode_command(const Command& cmd, std::uint8_t* out,
                            std::size_t cap) noexcept;

// Decode a COMMAND. `vt` is the attribute's value_type, resolved by the caller
// from the registry (it is not on the wire).
bool decode_command(const std::uint8_t* in, std::size_t len, ValueType vt,
                    Command& out) noexcept;

// ---- CMD_ACK (SPEC §5) ----------------------------------------------------
EncodeResult encode_cmd_ack(CmdAckStatus status, std::uint8_t* out,
                            std::size_t cap) noexcept;
bool decode_cmd_ack(const std::uint8_t* in, std::size_t len,
                    CmdAckStatus& out) noexcept;

// ---- STATE (SPEC §5) ------------------------------------------------------
// Layout: entity_id(u16) attr_count(u8) then count x { attr_id(u8) value bytes }.
struct StateEntry {
  std::uint8_t attr_id;
  Value        value;
};

EncodeResult encode_state(std::uint16_t entity_id, const StateEntry* entries,
                          std::uint8_t count, std::uint8_t* out,
                          std::size_t cap) noexcept;

// Pull-parser for a STATE payload. Value types are not on the wire, so the
// caller peeks the next attr_id, resolves its value_type from the registry,
// then consumes the value. `valid` goes false on truncation or a bad type;
// check it after the loop.
struct StateReader {
  const std::uint8_t* p;
  const std::uint8_t* end;
  std::uint16_t entity_id;
  std::uint8_t  remaining;
  bool          valid;
};

StateReader state_reader(const std::uint8_t* in, std::size_t len) noexcept;

// If another entry remains, sets `attr_id` to it (without consuming) and returns
// true. Returns false at a clean end; sets reader.valid=false on truncation.
bool state_next_attr(StateReader& reader, std::uint8_t& attr_id) noexcept;

// Consume the next entry's attr_id + value, interpreting the value as `vt`.
// Returns false (and clears reader.valid) on a bad type or truncation.
bool state_consume(StateReader& reader, ValueType vt, Value& out) noexcept;

}  // namespace hubproto
