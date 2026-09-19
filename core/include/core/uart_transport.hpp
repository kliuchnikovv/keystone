// UartTransport: a Bus over a byte stream, using COBS+CRC framing (libhubproto).
//
// It owns the framing/dispatch logic and is fully portable — the raw byte I/O is
// injected as a ByteLink, so the same class is host-tested with InMemoryLink and
// runs on hardware with an ESP-IDF UART link. The SPI/USB transports reuse this
// engine with a different ByteLink; only the medium changes (SPEC §7).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/frame.hpp"

namespace core {

class UartTransport final : public Bus {
 public:
  explicit UartTransport(ByteLink& link) noexcept;

  bool publish(const hubproto::Header& header, const std::uint8_t* payload,
               std::size_t payload_len) noexcept override;
  bool subscribe(MessageFn fn, void* ctx) noexcept override;

  static constexpr int kMaxSubscribers = 4;

 private:
  static void on_bytes(void* ctx, const std::uint8_t* data, std::size_t len) noexcept;
  void handle_bytes(const std::uint8_t* data, std::size_t len) noexcept;

  struct Subscriber {
    MessageFn fn = nullptr;
    void* ctx = nullptr;
  };

  ByteLink& link_;
  hubproto::FrameReader<> reader_;
  Subscriber subs_[kMaxSubscribers];
  int sub_count_ = 0;
  std::uint8_t out_[hubproto::kMaxFrameSize];
};

}  // namespace core
