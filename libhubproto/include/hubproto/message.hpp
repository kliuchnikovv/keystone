// Message layer: the 8-byte header (SPEC §4.2) and message types (SPEC §4.3).
//
// The header is fixed-size, little-endian, and topology-independent: chip_id
// stays present even on point-to-point links (SPEC Invariant 2). Payload codecs
// live in payload.hpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hubproto {

// Message type (header byte 1). SPEC §4.3.
enum class MsgType : std::uint8_t {
  Hello      = 0x01,
  Declare    = 0x02,
  DeclareEnd = 0x03,
  State      = 0x10,
  Command    = 0x20,
  CmdAck     = 0x21,
  Query      = 0x30,
  Heartbeat  = 0x40,
  // Firmware update of a co-processor (see payload.hpp, OTA section).
  OtaBegin    = 0x50,  // brain→chip   size + sha256 of the image
  OtaBeginAck = 0x51,  // chip→brain   accepted / refused, chunk size
  OtaChunk    = 0x52,  // brain→chip   offset + data
  OtaChunkAck = 0x53,  // chip→brain   next expected offset
  OtaEnd      = 0x54,  // brain→chip   all bytes sent: verify + activate
  OtaEndAck   = 0x55,  // chip→brain   verify result (chip reboots after Ok)
  OtaAbort    = 0x56,  // either       drop the transfer
};

// Reserved identities (SPEC §4.2).
inline constexpr std::uint8_t  kBrainChipId     = 0x00;
inline constexpr std::uint8_t  kBroadcastChipId = 0xFF;
inline constexpr std::uint16_t kSelfEntityId    = 0x0000;

inline constexpr std::size_t kHeaderSize = 8;

struct Header {
  std::uint8_t  version;     // wire protocol version; see kProtocolVersion
  MsgType       type;
  std::uint8_t  chip_id;     // identity of the OTHER endpoint
  std::uint8_t  boot_id;     // sender's reboot counter
  std::uint16_t seq;         // request/response matching
  std::uint16_t entity_id;   // subject entity (0 = the chip itself)
};

// True if `t` is one of the defined message types.
bool is_known_type(std::uint8_t t) noexcept;

// Serialize `h` into exactly kHeaderSize little-endian bytes.
void encode_header(const Header& h, std::uint8_t out[kHeaderSize]) noexcept;

// Parse kHeaderSize bytes into `out`. Returns false if the type byte is not a
// known MsgType; `version` is stored verbatim for the caller to check.
bool decode_header(const std::uint8_t in[kHeaderSize], Header& out) noexcept;

}  // namespace hubproto
