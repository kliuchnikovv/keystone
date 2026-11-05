// The Thread operational dataset, on its way from the co-processor to the brain.
//
// Why: the brain has no 802.15.4 radio, so the Matter commissioner has no Thread network of its
// own to hand to a device over BLE. The co-processor has a radio and a formed network, so it
// sends its active operational dataset (Thread TLVs — the very bytes CHIP's SetThreadOperationalDataset
// expects) over the hub link, and the brain passes them on.
//
// The check here is not decoration: a dataset that is truncated or that lost its network key would
// make the device attach to nothing and the commissioning fail with a message that says nothing
// about the cause. So the TLVs have to parse exactly, and the three things a device cannot do
// without — the channel, the PAN id and the network key — have to be in there.
//
// Portable: no radio, no ESP-IDF, host-testable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace core {

// The Thread TLV types of an operational dataset (Thread spec 5.4). The MeshCoP types (which OT
// also names) are a different numbering — do not mix them up.
inline constexpr std::uint8_t kTlvActiveTimestamp = 0x01;
inline constexpr std::uint8_t kTlvChannel         = 0x02;
inline constexpr std::uint8_t kTlvPanId           = 0x03;
inline constexpr std::uint8_t kTlvExtPanId        = 0x04;
inline constexpr std::uint8_t kTlvMeshLocalPrefix = 0x05;
inline constexpr std::uint8_t kTlvNetworkKey      = 0x07;
inline constexpr std::uint8_t kTlvSecurityPolicy  = 0x0C;
inline constexpr std::uint8_t kTlvNetworkName     = 0x0E;

class ThreadDataset {
 public:
  // 254 bytes: the Thread maximum for an operational dataset, and the size of CHIP's own buffer.
  static constexpr std::size_t kMaxBytes = 254;
  static constexpr std::size_t kMaxNameLen = 16;

  /** Takes the dataset. False when it is empty, too long, or not a dataset we can use — the
   *  bytes are left untouched in that case. */
  bool set(const std::uint8_t* bytes, std::size_t len) noexcept {
    if (bytes == nullptr || len == 0 || len > kMaxBytes) return false;
    if (!tlvs_walkable(bytes, len)) return false;
    if (!tlv_present(bytes, len, kTlvChannel)) return false;
    if (!tlv_present(bytes, len, kTlvPanId)) return false;
    if (!tlv_present(bytes, len, kTlvNetworkKey)) return false;
    std::memcpy(buf_, bytes, len);
    len_ = len;
    channel_ = 0;
    name_[0] = '\0';
    read_channel();
    read_name();
    return true;
  }

  void clear() noexcept {
    len_ = 0;
    channel_ = 0;
    name_[0] = '\0';
  }

  bool valid() const noexcept { return len_ > 0; }
  const std::uint8_t* data() const noexcept { return buf_; }
  /** The same bytes, writable: CHIP wants a mutable pointer to the dataset. */
  std::uint8_t* mutable_data() noexcept { return buf_; }
  std::size_t size() const noexcept { return len_; }

  /** The channel the network runs on (0 when the dataset has none). */
  std::uint8_t channel() const noexcept { return channel_; }

  /** The network name, if the dataset carries one ("" otherwise). */
  const char* network_name() const noexcept { return name_; }

  /** True when the dataset holds the TLV of that type. */
  bool has(std::uint8_t tlv_type) const noexcept { return tlv_present(buf_, len_, tlv_type); }

 private:
  // Walks the TLVs: type, length, value. The walk has to end exactly at the end of the buffer.
  static bool tlvs_walkable(const std::uint8_t* p, std::size_t len) noexcept {
    std::size_t i = 0;
    while (i < len) {
      if (i + 2 > len) return false;              // the header itself is cut off
      const std::size_t vlen = p[i + 1];
      i += 2;
      if (i + vlen > len) return false;           // the value is cut off
      i += vlen;
    }
    return i == len;
  }

  static bool tlv_present(const std::uint8_t* p, std::size_t len, std::uint8_t type) noexcept {
    std::size_t i = 0;
    while (i + 2 <= len) {
      const std::uint8_t t = p[i];
      const std::size_t vlen = p[i + 1];
      i += 2;
      if (i + vlen > len) return false;
      if (t == type) return true;
      i += vlen;
    }
    return false;
  }

  // The channel TLV holds three bytes: the channel, the page and the offset.
  void read_channel() noexcept {
    std::size_t i = 0;
    while (i + 2 <= len_) {
      const std::uint8_t t = buf_[i];
      const std::size_t vlen = buf_[i + 1];
      i += 2;
      if (i + vlen > len_) return;
      if (t == kTlvChannel && vlen >= 1) {
        channel_ = buf_[i];
        return;
      }
      i += vlen;
    }
  }

  // The network name TLV holds the name as UTF-8, up to 16 bytes.
  void read_name() noexcept {
    std::size_t i = 0;
    while (i + 2 <= len_) {
      const std::uint8_t t = buf_[i];
      const std::size_t vlen = buf_[i + 1];
      i += 2;
      if (i + vlen > len_) return;
      if (t == kTlvNetworkName) {
        const std::size_t n = vlen < kMaxNameLen ? vlen : kMaxNameLen;
        std::memcpy(name_, buf_ + i, n);
        name_[n] = '\0';
        return;
      }
      i += vlen;
    }
  }

  std::uint8_t buf_[kMaxBytes] = {0};
  std::size_t  len_ = 0;
  std::uint8_t channel_ = 0;
  char         name_[kMaxNameLen + 1] = {0};
};

}  // namespace core
