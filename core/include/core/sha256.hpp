// Portable incremental SHA-256 (FIPS 180-4). No dependencies, so the same code
// verifies OTA images on a co-processor and in host tests. Not on a hot path:
// the OTA link is ~11 KB/s, so the software implementation is plenty.
#pragma once

#include <cstddef>
#include <cstdint>

namespace core {

class Sha256 {
 public:
  static constexpr std::size_t kDigestSize = 32;

  Sha256() noexcept { reset(); }

  void reset() noexcept;
  void update(const std::uint8_t* data, std::size_t len) noexcept;
  // Finish and write the digest. The object must be reset() before reuse.
  void finish(std::uint8_t out[kDigestSize]) noexcept;

 private:
  void compress(const std::uint8_t block[64]) noexcept;

  std::uint32_t h_[8];
  std::uint8_t  buf_[64];
  std::size_t   buf_len_;
  std::uint64_t total_;
};

}  // namespace core
