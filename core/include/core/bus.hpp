// Bus and ByteLink: the transport boundary (SPEC Invariant 1).
//
// Core logic talks only to `Bus` (publish a message / subscribe to inbound
// messages), never to a physical link. A `Bus` implementation frames messages
// over a `ByteLink`, which is the raw byte medium (UART now, SPI/USB later).
// Neither type has any radio or ESP-IDF dependency, so the core stays portable
// (SPEC Invariants 3 & 5).
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/message.hpp"

namespace core {

// Raw, unframed byte medium. Output goes through write(); inbound bytes are
// delivered to a single receiver the transport registers. Implementations:
// InMemoryLink (tests), an ESP-IDF UART link (firmware).
class ByteLink {
 public:
  using ReceiveFn = void (*)(void* ctx, const std::uint8_t* data, std::size_t len);

  virtual ~ByteLink() = default;

  // Send raw bytes. Returns false if they could not be handed to the medium.
  virtual bool write(const std::uint8_t* data, std::size_t len) noexcept = 0;

  // Register the callback invoked when bytes arrive from the medium.
  virtual void set_receiver(ReceiveFn fn, void* ctx) noexcept = 0;
};

// Message-level transport boundary. Deals in decoded messages (header + payload
// bytes), not frames or wire bytes.
class Bus {
 public:
  // Inbound message handler. `payload`/`len` are valid only for the duration of
  // the call (they point into the transport's buffer).
  using MessageFn = void (*)(void* ctx, const hubproto::Header& header,
                             const std::uint8_t* payload, std::size_t len);

  virtual ~Bus() = default;

  // Send a message to the peer. Returns false if it could not be encoded or
  // written (e.g. payload too large, or the link rejected it).
  virtual bool publish(const hubproto::Header& header,
                       const std::uint8_t* payload,
                       std::size_t payload_len) noexcept = 0;

  // Register an inbound handler. Returns false if the subscriber table is full.
  virtual bool subscribe(MessageFn fn, void* ctx) noexcept = 0;
};

}  // namespace core
