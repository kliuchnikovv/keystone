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
  // The co-processor itself, as a first-class entity (entity_id = its chip_id): its
  // telemetry (uptime, heap, link counters) reaches the brain as ordinary
  // STATE updates. Not a Matter accessory — the bridge skips it.
  Chip      = 0x0A,
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

// ---- Chip self entity (device_type=Chip, entity_id 0x0000) ----------------
// Every co-processor declares one self entity and periodically reports its
// health as a single STATE. attr_id semantics are scoped to device_type (SPEC
// §5), so these ids don't clash with a light's on_off(0x01) etc. The brain gets
// chip health through the ordinary registry/STATE plumbing — no new message.
namespace chip_attr {
inline constexpr std::uint8_t kUptimeS   = 0x70;  // u32 seconds since boot
inline constexpr std::uint8_t kHeapFree  = 0x71;  // u32 bytes of free heap
inline constexpr std::uint8_t kBootId    = 0x72;  // u8  reboot counter (NVS)
inline constexpr std::uint8_t kRxFrames  = 0x73;  // u32 frames received from the brain
inline constexpr std::uint8_t kTxFrames  = 0x74;  // u32 frames sent to the brain
inline constexpr std::uint8_t kCrcErrors = 0x75;  // u16 frames dropped (bad CRC/COBS)
}  // namespace chip_attr

// Canonical telemetry carried by the self entity.
struct ChipSelf {
  std::uint32_t uptime_s;
  std::uint32_t heap_free;
  std::uint8_t  boot_id;
  std::uint32_t rx_frames;
  std::uint32_t tx_frames;
  std::uint16_t crc_errors;
};

// Encode the self entity's DECLARE (device_type=Chip, six attrs).
EncodeResult encode_chip_self_declare(std::uint16_t self_entity_id, std::uint8_t* out,
                                      std::size_t cap) noexcept;

// Encode a STATE update for the self entity carrying all six attributes at once.
EncodeResult encode_chip_self_state(std::uint16_t self_entity_id, const ChipSelf& s,
                                    std::uint8_t* out, std::size_t cap) noexcept;

// ---- OTA (firmware update of a co-processor) ------------------------------
// Stop-and-wait transfer driven by the brain. The header `seq` of each request
// is echoed in its ack; `entity_id` is 0 (the message is about the chip).
//
//   OTA_BEGIN                        : total_size(u32) sha256[32]
//   OTA_BEGIN_ACK                    : status(u8) chunk_size(u16)
//   OTA_CHUNK                        : offset(u32) data[1..kOtaMaxChunk]
//   OTA_CHUNK_ACK                    : status(u8) next_offset(u32)
//   OTA_END                          : (empty)
//   OTA_END_ACK                      : status(u8)
//   OTA_ABORT                        : (empty)
//
// The chip writes a chunk only at its `next_offset`. A chunk it already has (the
// brain resent it because the ack was lost) is confirmed with Ok and not written
// again; a chunk that skips ahead gets BadOffset + the offset the chip expects.
// A resent END after the image was accepted is confirmed with Ok as well.

inline constexpr std::size_t kOtaSha256Size = 32;
inline constexpr std::size_t kOtaChunkHeader = 4;                 // offset(u32)
inline constexpr std::size_t kOtaMaxChunk = 240;                  // data bytes
static_assert(kOtaChunkHeader + kOtaMaxChunk <= 255, "must fit one frame (kMaxPayloadSize)");

enum class OtaStatus : std::uint8_t {
  Ok         = 0,
  Busy       = 1,  // another transfer is in progress
  TooLarge   = 2,  // image does not fit the inactive slot
  BadState   = 3,  // message not valid in the current state
  BadOffset  = 4,  // chunk is not at next_offset (next_offset tells where)
  WriteFail  = 5,  // flash write failed
  VerifyFail = 6,  // size / sha256 / image validation failed
  Unsupported = 7, // this chip cannot be updated over the link
};

struct OtaBegin {
  std::uint32_t total_size;
  std::uint8_t  sha256[kOtaSha256Size];
};
EncodeResult encode_ota_begin(const OtaBegin& in, std::uint8_t* out,
                              std::size_t cap) noexcept;
bool decode_ota_begin(const std::uint8_t* in, std::size_t len, OtaBegin& out) noexcept;

struct OtaBeginAck {
  OtaStatus     status;
  std::uint16_t chunk_size;  // data bytes per chunk the chip wants (<= kOtaMaxChunk)
};
EncodeResult encode_ota_begin_ack(const OtaBeginAck& in, std::uint8_t* out,
                                  std::size_t cap) noexcept;
bool decode_ota_begin_ack(const std::uint8_t* in, std::size_t len,
                          OtaBeginAck& out) noexcept;

// Zero-copy view of a decoded OTA_CHUNK; `data` points into the input buffer.
struct OtaChunkView {
  std::uint32_t       offset;
  const std::uint8_t* data;
  std::size_t         len;
};
EncodeResult encode_ota_chunk(std::uint32_t offset, const std::uint8_t* data,
                              std::size_t data_len, std::uint8_t* out,
                              std::size_t cap) noexcept;
bool decode_ota_chunk(const std::uint8_t* in, std::size_t len,
                      OtaChunkView& out) noexcept;

struct OtaChunkAck {
  OtaStatus     status;
  std::uint32_t next_offset;  // the byte the chip wants next
};
EncodeResult encode_ota_chunk_ack(const OtaChunkAck& in, std::uint8_t* out,
                                  std::size_t cap) noexcept;
bool decode_ota_chunk_ack(const std::uint8_t* in, std::size_t len,
                          OtaChunkAck& out) noexcept;

EncodeResult encode_ota_end_ack(OtaStatus status, std::uint8_t* out,
                                std::size_t cap) noexcept;
bool decode_ota_end_ack(const std::uint8_t* in, std::size_t len,
                        OtaStatus& out) noexcept;

// ---- OTA verification (did the new image stay?) -----------------------------
//   OTA_INFO_REQ : (empty)
//   OTA_INFO     : running_slot(u8) flags(u8) last_ota_slot(u8) last_ota_sha256[32]
// The chip remembers the slot and sha256 of the last image it accepted over OTA.
// After the reboot the brain asks: if the chip runs that slot, the new image
// stayed; if not, the bootloader rolled it back. Slot 0xFF = unknown / none.
inline constexpr std::uint8_t kOtaSlotUnknown = 0xFF;
inline constexpr std::uint8_t kOtaInfoFlagConfirmed = 0x01;  // running image is not pending verify
inline constexpr std::size_t  kOtaInfoSize = 3 + kOtaSha256Size;

struct OtaInfo {
  std::uint8_t running_slot;
  bool         confirmed;
  std::uint8_t last_ota_slot;
  std::uint8_t last_ota_sha256[kOtaSha256Size];
};
EncodeResult encode_ota_info(const OtaInfo& in, std::uint8_t* out, std::size_t cap) noexcept;
bool decode_ota_info(const std::uint8_t* in, std::size_t len, OtaInfo& out) noexcept;

// ---- Device OTA (the firmware of a Zigbee device, via the coordinator) ---------

inline constexpr std::size_t kDevOtaSha256Size = 32;
inline constexpr std::size_t kDevOtaChunkHeader = 4;
inline constexpr std::size_t kDevOtaMaxChunk = 240;
static_assert(kDevOtaChunkHeader + kDevOtaMaxChunk <= 255,
              "must fit one frame (kMaxPayloadSize)");

enum class DevOtaPhase : std::uint8_t {
  Idle         = 0,
  Receiving    = 1,
  Verifying    = 2,
  Notifying    = 3,
  Transferring = 4,
  Done         = 5,
  Failed       = 6,
};

enum class DevOtaStatus : std::uint8_t {
  Ok          = 0,
  Busy        = 1,
  TooLarge    = 2,
  BadChecksum = 3,
  BadFormat   = 4,
  Unsupported = 5,
  Timeout     = 6,
  Mismatch    = 7,
  Aborted     = 8,
};

struct DevOtaBegin {
  std::uint32_t total_size;
  std::uint8_t  sha256[kDevOtaSha256Size];
};
EncodeResult encode_dev_ota_begin(const DevOtaBegin& in, std::uint8_t* out,
                                   std::size_t cap) noexcept;
bool decode_dev_ota_begin(const std::uint8_t* in, std::size_t len, DevOtaBegin& out) noexcept;

struct DevOtaChunkView {
  std::uint32_t       offset;
  const std::uint8_t* data;
  std::size_t         len;
};
EncodeResult encode_dev_ota_chunk(std::uint32_t offset, const std::uint8_t* data,
                                   std::size_t data_len, std::uint8_t* out,
                                   std::size_t cap) noexcept;
bool decode_dev_ota_chunk(const std::uint8_t* in, std::size_t len,
                           DevOtaChunkView& out) noexcept;

struct DevOtaStatusMsg {
  DevOtaPhase  phase = DevOtaPhase::Idle;
  DevOtaStatus status = DevOtaStatus::Ok;
  std::uint8_t percent = 0;
  std::uint32_t next_offset = 0;
};
EncodeResult encode_dev_ota_status(const DevOtaStatusMsg& in, std::uint8_t* out,
                                    std::size_t cap) noexcept;
bool decode_dev_ota_status(const std::uint8_t* in, std::size_t len,
                            DevOtaStatusMsg& out) noexcept;

// ---- Matter commissioning (the hub adds a Matter device to the home) ----------

inline constexpr std::uint8_t kMatterKindService      = 0;
inline constexpr std::uint8_t kMatterKindManufacturer = 1;
inline constexpr std::uint8_t kMatterKindQr           = 2;
inline constexpr std::size_t  kMatterMaxPayload       = 64;
inline constexpr std::size_t  kMatterPairHeader       = 2;
static_assert(kMatterPairHeader + kMatterMaxPayload <= 255, "must fit one frame (kMaxPayloadSize)");

enum class MatterState : std::uint8_t {
  Idle          = 0,
  Scanning      = 1,
  Commissioning = 2,
  Adding        = 3,
  Done          = 4,
  Failed        = 5,
};

enum class MatterCode : std::uint8_t {
  Ok          = 0,
  NoDevice    = 1,
  BadPayload  = 2,
  NoSetupHash = 3,
  Busy        = 4,
  Unsupported = 5,
  Timeout     = 6,
  Failed      = 7,
};

struct MatterPairView {
  std::uint8_t       kind;
  const std::uint8_t* bytes;
  std::size_t         len;
};
EncodeResult encode_matter_pair(std::uint8_t kind, const std::uint8_t* bytes,
                                std::size_t len, std::uint8_t* out,
                                std::size_t cap) noexcept;
bool decode_matter_pair(const std::uint8_t* in, std::size_t len,
                        MatterPairView& out) noexcept;

struct MatterResult {
  MatterState   state;
  MatterCode    code;
  std::uint16_t entity_id;
};
EncodeResult encode_matter_result(const MatterResult& in, std::uint8_t* out,
                                  std::size_t cap) noexcept;
bool decode_matter_result(const std::uint8_t* in, std::size_t len,
                           MatterResult& out) noexcept;

// ---- The Thread operational dataset -----------------------------------------
// The brain has no 802.15.4 radio, so the Matter commissioner has no Thread
// network of its own to hand to a device. The co-processor does have a radio
// and a formed network, so it sends its active operational dataset (Thread
// TLVs, the same bytes CHIP's SetThreadOperationalDataset expects) over the
// hub link. 254 bytes is the Thread maximum for a dataset and the size of
// CHIP's own buffer.
inline constexpr std::size_t kThreadDatasetMax = 254;

struct ThreadDatasetView {
  const std::uint8_t* bytes;
  std::size_t         len;
};
EncodeResult encode_thread_dataset(const std::uint8_t* bytes, std::size_t len,
                                    std::uint8_t* out, std::size_t cap) noexcept;
bool decode_thread_dataset(const std::uint8_t* in, std::size_t len,
                           ThreadDatasetView& out) noexcept;

}  // namespace hubproto
