// Frame layer (SPEC §4.1): the on-the-wire framing.
//
//   frame = COBS( HEADER || PAYLOAD || CRC16 )  0x00
//
// CRC16 (CRC-16/CCITT-FALSE) is computed over HEADER||PAYLOAD before COBS. The
// trailing 0x00 is the delimiter, which COBS guarantees never appears inside the
// block, so a receiver self-resynchronizes after any glitch.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/cobs.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"  // EncodeResult

namespace hubproto {

inline constexpr std::size_t kCrcSize = 2;
inline constexpr std::size_t kMaxPayloadSize = 255;
inline constexpr std::size_t kMaxDecodedSize = kHeaderSize + kMaxPayloadSize + kCrcSize;
inline constexpr std::size_t kMaxFrameSize = cobs_encode_bound(kMaxDecodedSize) + 1;

// Build a complete frame (COBS block + trailing 0x00) from header and payload.
// Fails if payload_len > kMaxPayloadSize or `out` is too small.
EncodeResult encode_frame(const Header& header, const std::uint8_t* payload,
                          std::size_t payload_len, std::uint8_t* out,
                          std::size_t cap) noexcept;

namespace detail {
// Decode one COBS block (bytes between delimiters, no trailing 0x00): COBS-decode
// into `scratch`, verify length and CRC, decode the header. On success `hdr` is
// filled, payload is scratch[kHeaderSize .. kHeaderSize+payload_len), and true is
// returned. Any failure (COBS, short block, CRC mismatch, unknown type) → false.
bool decode_block(const std::uint8_t* block, std::size_t block_len,
                  std::uint8_t* scratch, std::size_t scratch_cap, Header& hdr,
                  std::size_t& payload_len) noexcept;
}  // namespace detail

// Streaming receiver. Feed incoming bytes one at a time; push() returns true
// exactly when a complete, CRC-valid frame is ready, after which header(),
// payload() and payload_len() describe it (valid until the next push()).
// Malformed frames are dropped and the reader resyncs on the next 0x00.
template <std::size_t Cap = kMaxFrameSize>
class FrameReader {
 public:
  bool push(std::uint8_t b) noexcept {
    if (b == 0x00) {
      const bool had_bytes = accum_len_ > 0 || overflow_;
      const bool ok = !overflow_ && accum_len_ > 0 &&
                      detail::decode_block(accum_, accum_len_, decoded_,
                                           sizeof(decoded_), header_, payload_len_);
      accum_len_ = 0;
      overflow_ = false;
      if (had_bytes && !ok) ++bad_frames_;
      return ok;
    }
    if (accum_len_ < sizeof(accum_)) {
      accum_[accum_len_++] = b;
    } else {
      overflow_ = true;  // frame too big; drop until the next delimiter
    }
    return false;
  }

  const Header& header() const noexcept { return header_; }
  const std::uint8_t* payload() const noexcept { return decoded_ + kHeaderSize; }
  std::size_t payload_len() const noexcept { return payload_len_; }

  // Number of non-empty blocks dropped since construction (bad COBS, short
  // block, CRC mismatch, unknown type, or an overlong frame). A link-quality
  // signal, surfaced through the chip self entity's crc_errors.
  std::size_t bad_frames() const noexcept { return bad_frames_; }

 private:
  std::uint8_t accum_[Cap];
  std::uint8_t decoded_[Cap];
  std::size_t accum_len_ = 0;
  std::size_t payload_len_ = 0;
  std::size_t bad_frames_ = 0;
  Header header_{};
  bool overflow_ = false;
};

}  // namespace hubproto