#include "hubproto/frame.hpp"

#include "hubproto/crc16.hpp"

namespace hubproto {

EncodeResult encode_frame(const Header& header, const std::uint8_t* payload,
                          std::size_t payload_len, std::uint8_t* out,
                          std::size_t cap) noexcept {
  if (payload_len > kMaxPayloadSize) return {false, 0};

  // Assemble HEADER || PAYLOAD || CRC16 in a scratch buffer.
  std::uint8_t scratch[kMaxDecodedSize];
  encode_header(header, scratch);
  for (std::size_t i = 0; i < payload_len; ++i) scratch[kHeaderSize + i] = payload[i];

  const std::size_t body_len = kHeaderSize + payload_len;
  const std::uint16_t crc = crc16_ccitt(scratch, body_len);
  scratch[body_len] = static_cast<std::uint8_t>(crc & 0xFF);
  scratch[body_len + 1] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
  const std::size_t total = body_len + kCrcSize;

  // COBS-encode the block, then append the 0x00 delimiter.
  if (cap == 0) return {false, 0};
  CobsResult enc = cobs_encode(scratch, total, out, cap - 1);
  if (!enc.ok) return {false, 0};
  out[enc.len] = 0x00;
  return {true, enc.len + 1};
}

namespace detail {

bool decode_block(const std::uint8_t* block, std::size_t block_len,
                  std::uint8_t* scratch, std::size_t scratch_cap, Header& hdr,
                  std::size_t& payload_len) noexcept {
  CobsResult dec = cobs_decode(block, block_len, scratch, scratch_cap);
  if (!dec.ok) return false;
  if (dec.len < kHeaderSize + kCrcSize) return false;  // no room for header+CRC

  const std::size_t body_len = dec.len - kCrcSize;
  const std::uint16_t want = static_cast<std::uint16_t>(
      scratch[body_len] | (scratch[body_len + 1] << 8));
  if (crc16_ccitt(scratch, body_len) != want) return false;

  if (!decode_header(scratch, hdr)) return false;
  payload_len = body_len - kHeaderSize;
  return true;
}

}  // namespace detail

}  // namespace hubproto
