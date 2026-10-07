// OtaSender: the brain side of a firmware update. Streams an image to one
// co-processor with a stop-and-wait protocol (one CHUNK in flight, resent on
// timeout; see hubproto OTA messages).
//
// Pump model: bus callbacks only record acks. Reading the next bytes from the
// source (an HTTP stream, possibly blocking) happens in poll(), so it runs in
// the caller's own task and never stalls the UART receive path. A wake callback
// tells that task an ack arrived so it can poll immediately instead of sleeping
// a tick per chunk. Time is injected; the core stays clock-free.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace core {

class OtaSender {
 public:
  // Fill `buf` with up to `cap` bytes of the image and return how many. Return
  // 0 on end-of-data or error; the sender then fails the transfer if bytes are
  // still missing (the image length is announced in start()).
  using ReadFn = std::size_t (*)(void* ctx, std::uint8_t* buf, std::size_t cap);
  using WakeFn = void (*)(void* ctx);

  enum class State : std::uint8_t {
    Idle,       // nothing started
    Beginning,  // BEGIN sent, waiting for the chip to erase + accept
    Sending,    // streaming chunks
    Ending,     // END sent, waiting for verification
    Done,       // chip accepted the image and is rebooting into it
    Failed,     // see failure()
  };

  enum class Failure : std::uint8_t {
    None,
    Refused,      // the chip answered with a non-Ok status (see status())
    Timeout,      // no ack after all retries. If this happens while Ending the
                  // chip may still have accepted the image and rebooted (its END
                  // ack lost, then the link went down): the caller should look
                  // for the chip's HELLO with a new boot_id before giving up.
    SourceError,  // the image source ran dry before total_size bytes
    Aborted,      // abort() was called
  };

  struct Config {
    std::uint32_t begin_timeout_ms = 30000;  // chip erases the slot before acking
    std::uint32_t ack_timeout_ms = 1000;     // per chunk
    std::uint32_t end_timeout_ms = 20000;    // chip validates the whole image
    int max_retries = 5;                     // resends after the first send
  };

  explicit OtaSender(Bus& bus) noexcept : OtaSender(bus, Config{}) {}
  OtaSender(Bus& bus, Config cfg) noexcept;

  // Wake `fn` whenever an ack arrives (from the bus-callback context).
  void on_wake(WakeFn fn, void* ctx) noexcept {
    wake_fn_ = fn;
    wake_ctx_ = ctx;
  }

  // Begin updating `chip_id` with an image of `total_size` bytes whose SHA-256 is
  // `sha256`. Returns false if a transfer is already running or the size is 0.
  bool start(std::uint8_t chip_id, std::uint32_t total_size,
             const std::uint8_t sha256[hubproto::kOtaSha256Size], ReadFn read,
             void* read_ctx, std::uint32_t now_ms) noexcept;

  // Drive the transfer: send the next chunk / END, resend on timeout.
  void poll(std::uint32_t now_ms) noexcept;

  // Tell the chip to drop the transfer and stop.
  void abort() noexcept;

  State state() const noexcept { return state_; }
  bool finished() const noexcept { return state_ == State::Done || state_ == State::Failed; }
  Failure failure() const noexcept { return failure_; }
  hubproto::OtaStatus status() const noexcept { return status_; }
  std::uint32_t acked() const noexcept { return acked_; }  // bytes the chip confirmed
  std::uint32_t total() const noexcept { return total_; }

 private:
  static void on_msg(void* ctx, const hubproto::Header& h, const std::uint8_t* payload,
                     std::size_t len) noexcept {
    static_cast<OtaSender*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void send(hubproto::MsgType type, std::uint16_t seq, const std::uint8_t* payload,
            std::size_t len) noexcept;
  void transmit(std::uint32_t now_ms) noexcept;  // (re)send the outstanding request
  void fail(Failure f, hubproto::OtaStatus s) noexcept;
  std::uint16_t next_seq() noexcept;
  std::uint32_t timeout_for_state() const noexcept;

  Bus& bus_;
  Config cfg_;
  State state_ = State::Idle;
  Failure failure_ = Failure::None;
  hubproto::OtaStatus status_ = hubproto::OtaStatus::Ok;

  std::uint8_t chip_id_ = 0;
  std::uint32_t total_ = 0;
  std::uint8_t sha256_[hubproto::kOtaSha256Size] = {};
  ReadFn read_ = nullptr;
  void* read_ctx_ = nullptr;
  std::uint16_t chunk_size_ = hubproto::kOtaMaxChunk;

  std::uint32_t acked_ = 0;  // offset the chip has confirmed

  // The one request in flight.
  bool outstanding_ = false;
  hubproto::MsgType out_type_ = hubproto::MsgType::OtaBegin;
  std::uint16_t out_seq_ = 0;
  std::uint8_t out_buf_[255] = {};
  std::size_t out_len_ = 0;
  std::uint32_t out_offset_ = 0;  // chunk offset (when out_type_ == OtaChunk)
  std::size_t out_data_len_ = 0;  // chunk data bytes
  std::uint32_t sent_ms_ = 0;
  int attempts_ = 0;

  std::uint16_t seq_counter_ = 1;
  WakeFn wake_fn_ = nullptr;
  void* wake_ctx_ = nullptr;
};

}  // namespace core
