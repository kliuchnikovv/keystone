// COBS (Consistent Overhead Byte Stuffing).
//
// Removes every 0x00 byte from a block so a single 0x00 can delimit frames on a
// byte stream (SPEC §4.1). These functions encode/decode the block only; the
// trailing 0x00 delimiter is added/consumed by the frame layer, not here.
//
// Buffer-based and allocation-free so the same code runs on an MCU. Each call
// reports success and the number of bytes written; a caller sizes the output
// with cobs_encode_bound() (encode) or `in_len` (decode, an upper bound).
#pragma once

#include <cstddef>
#include <cstdint>

namespace hubproto {

struct CobsResult {
  bool ok;           // false on output overflow or malformed input
  std::size_t len;   // bytes written to `out` when ok
};

// Worst-case encoded size for `len` input bytes: one overhead byte per 254-byte
// run, plus the leading code byte.
constexpr std::size_t cobs_encode_bound(std::size_t len) noexcept {
  return len + len / 254 + 1;
}

// Encode [in, in+in_len) into `out` (capacity out_cap). Output contains no 0x00.
CobsResult cobs_encode(const std::uint8_t* in, std::size_t in_len,
                       std::uint8_t* out, std::size_t out_cap) noexcept;

// Decode a COBS block [in, in+in_len) (no trailing delimiter) into `out`.
CobsResult cobs_decode(const std::uint8_t* in, std::size_t in_len,
                       std::uint8_t* out, std::size_t out_cap) noexcept;

}  // namespace hubproto
