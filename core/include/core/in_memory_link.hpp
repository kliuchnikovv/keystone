// InMemoryLink: an in-memory ByteLink for host tests (no hardware).
//
// Two links are cross-connected into a loopback pair; a write() on one side is
// delivered synchronously to the other side's receiver. It models the wire, not
// its timing: use it to test framing and dispatch, not real UART latency.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"

namespace core {

class InMemoryLink final : public ByteLink {
 public:
  bool write(const std::uint8_t* data, std::size_t len) noexcept override {
    if (peer_ != nullptr && peer_->rx_ != nullptr) {
      peer_->rx_(peer_->rx_ctx_, data, len);
    }
    return true;
  }

  void set_receiver(ReceiveFn fn, void* ctx) noexcept override {
    rx_ = fn;
    rx_ctx_ = ctx;
  }

  // Wire two links together so each delivers to the other.
  static void connect(InMemoryLink& a, InMemoryLink& b) noexcept {
    a.peer_ = &b;
    b.peer_ = &a;
  }

 private:
  InMemoryLink* peer_ = nullptr;
  ReceiveFn rx_ = nullptr;
  void* rx_ctx_ = nullptr;
};

}  // namespace core
