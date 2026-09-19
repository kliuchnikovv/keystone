// CRC-16/CCITT-FALSE over a byte range.
//
// Parameters: poly=0x1021, init=0xFFFF, no input/output reflection, xorout=0x0000.
// This is the CRC used on the wire (SPEC §4.1), computed over HEADER || PAYLOAD
// BEFORE COBS encoding. Canonical check value: crc16_ccitt("123456789") == 0x29B1.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hubproto {

std::uint16_t crc16_ccitt(const std::uint8_t* data, std::size_t len) noexcept;

}  // namespace hubproto
