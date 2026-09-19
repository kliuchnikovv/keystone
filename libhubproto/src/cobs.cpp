#include "hubproto/cobs.hpp"

namespace hubproto {

CobsResult cobs_encode(const std::uint8_t* in, std::size_t in_len,
                       std::uint8_t* out, std::size_t out_cap) noexcept {
  std::size_t read = 0;
  std::size_t write = 0;
  bool more = true;

  // Each iteration emits exactly one COBS group: a code byte followed by up to
  // 253 non-zero data bytes. A group ends on a zero byte (consumed as the
  // implicit separator) or after 254 non-zero bytes (code 0xFF, no separator).
  while (more) {
    if (write >= out_cap) return {false, 0};
    const std::size_t code_pos = write++;
    std::uint8_t code = 1;

    while (read < in_len && in[read] != 0x00 && code < 0xFF) {
      if (write >= out_cap) return {false, 0};
      out[write++] = in[read++];
      ++code;
    }
    out[code_pos] = code;

    if (read < in_len && in[read] == 0x00) {
      ++read;         // consume separator; another group must follow
      more = true;
    } else if (read < in_len) {
      more = true;    // stopped at the 0xFF boundary, data remains
    } else {
      more = false;   // input exhausted
    }
  }

  return {true, write};
}

CobsResult cobs_decode(const std::uint8_t* in, std::size_t in_len,
                       std::uint8_t* out, std::size_t out_cap) noexcept {
  std::size_t read = 0;
  std::size_t write = 0;

  while (read < in_len) {
    const std::uint8_t code = in[read++];
    if (code == 0x00) return {false, 0};  // a block must not contain 0x00

    for (std::uint8_t i = 1; i < code; ++i) {
      if (read >= in_len) return {false, 0};  // code overruns the block
      if (write >= out_cap) return {false, 0};
      out[write++] = in[read++];
    }

    // A non-0xFF group represents a removed separator zero, except for the
    // final group in the block.
    if (code < 0xFF && read < in_len) {
      if (write >= out_cap) return {false, 0};
      out[write++] = 0x00;
    }
  }

  return {true, write};
}

}  // namespace hubproto
