// DeviceOtaSender: the brain side of a *device* firmware update. Streams a vendor
// .ota file to the Zigbee coordinator, which then serves it to the device with the
// ZBOSS OTA server (cluster 0x0019). See hubproto "device OTA" messages.
//
// The transfer itself is stop-and-wait, like the co-processor OTA: one CHUNK in
// flight, resent on timeout. The chip's replies are a single DEV_OTA_STATUS message
// that does double duty: while the file streams it acks the transfer (phase
// Receiving, next_offset = the byte the chip wants next); after DEV_OTA_END it
// reports the device session (verifying / notifying / transferring / done / failed),
// which is forwarded to the phase callback.
//
// Pump model: like OtaSender, reading from the source (an HTTP stream) happens in
// poll(), so it runs in the caller's task and never stalls the UART receive path.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace core {

class DeviceOtaSender {
 public:
  // Fill `buf` with up to `cap` bytes of the .ota file and return how many. Return
  // 0 on end-of-data or error; the sender then fails the transfer if bytes are still
  // missing (the file length is announced in start()).
  using ReadFn = std::size_t (*)(void* ctx, std::uint8_t* buf, std::size_t cap);
  using WakeFn = void (*)(void* ctx);

  enum class State : std::uint8_t {
    Idle,       // nothing started
    Beginning,  // BEGIN sent, waiting for the chip to accept
    Sending,    // streaming chunks
    Ending,     // END sent, waiting for the chip to verify the file
    Done,       // the chip took the file and is now talking to the device
    Failed,     // see failure()
  };

  enum class Failure : std::uint8_t {
    None,
    Refused,      // the chip answered with a non-Ok status (see status())
    Timeout,      // no answer after all retries
    SourceError,  // the file source ran dry before total_size bytes
    Aborted,      // abort() was called
  };

  struct Config {
    std::uint32_t begin_timeout_ms = 30000;  // the chip erases its image partition
    std::uint32_t ack_timeout_ms = 1000;     // per chunk
    std::uint32_t end_timeout_ms = 30000;    // the chip validates the whole file
    int max_retries = 5;                     // resends after the first send
  };

  explicit DeviceOtaSender(Bus& bus) noexcept : DeviceOtaSender(bus, Config{}) {}
  DeviceOtaSender(Bus& bus, Config cfg) noexcept;

  // Wake `fn` whenever an answer arrives (from the bus-callback context).
  void on_wake(WakeFn fn, void* ctx) noexcept {
    wake_fn_ = fn;
    wake_ctx_ = ctx;
  }

  // The chip reports the device session (after the file was accepted). Called from
  // the bus-callback context, so keep it short.
  using PhaseFn = void (*)(void* ctx, std::uint16_t entity_id, hubproto::DevOtaPhase phase,
                           hubproto::DevOtaStatus status, std::uint8_t percent);
  void on_phase(PhaseFn fn, void* ctx) noexcept {
    phase_fn_ = fn;
    phase_ctx_ = ctx;
  }

  // Start pushing the .ota file for `entity_id` (the device) to `chip_id`. The file
  // is `total_size` bytes long and its SHA-256 is `sha256`. Returns false if a
  // transfer is already running or the size is 0.
  bool start(std::uint8_t chip_id, std::uint16_t entity_id, std::uint32_t total_size,
             const std::uint8_t sha256[hubproto::kDevOtaSha256Size], ReadFn read,
             void* read_ctx, std::uint32_t now_ms) noexcept;

  // Drive the transfer: send the next chunk / END, resend on timeout.
  void poll(std::uint32_t now_ms) noexcept;

  // Tell the chip to drop the transfer and stop.
  void abort() noexcept;

  State state() const noexcept { return state_; }
  bool finished() const noexcept { return state_ == State::Done || state_ == State::Failed; }
  Failure failure() const noexcept { return failure_; }
  hubproto::DevOtaStatus status() const noexcept { return status_; }
  std::uint32_t acked() const noexcept { return acked_; }  // bytes the chip confirmed
  std::uint32_t total() const noexcept { return total_; }
  std::uint16_t entity_id() const noexcept { return entity_id_; }

  // The device session as last reported by the chip.
  hubproto::DevOtaPhase phase() const noexcept { return phase_; }
  std::uint8_t percent() const noexcept { return percent_; }

 private:
  static void on_msg(void* ctx, const hubproto::Header& h, const std::uint8_t* payload,
                     std::size_t len) noexcept {
    static_cast<DeviceOtaSender*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void send(hubproto::MsgType type, std::uint16_t seq, const std::uint8_t* payload,
            std::size_t len) noexcept;
  void transmit(std::uint32_t now_ms) noexcept;  // (re)send the outstanding request
  void fail(Failure f, hubproto::DevOtaStatus s) noexcept;
  std::uint16_t next_seq() noexcept;
  std::uint32_t timeout_for_state() const noexcept;

  Bus& bus_;
  Config cfg_;
  State state_ = State::Idle;
  Failure failure_ = Failure::None;
  hubproto::DevOtaStatus status_ = hubproto::DevOtaStatus::Ok;
  hubproto::DevOtaPhase phase_ = hubproto::DevOtaPhase::Idle;
  std::uint8_t percent_ = 0;

  std::uint8_t chip_id_ = 0;
  std::uint16_t entity_id_ = hubproto::kSelfEntityId;
  std::uint32_t total_ = 0;
  std::uint8_t sha256_[hubproto::kDevOtaSha256Size] = {};
  ReadFn read_ = nullptr;
  void* read_ctx_ = nullptr;
  std::uint16_t chunk_size_ = hubproto::kDevOtaMaxChunk;

  std::uint32_t acked_ = 0;  // offset the chip has confirmed

  // The one request in flight.
  bool outstanding_ = false;
  hubproto::MsgType out_type_ = hubproto::MsgType::DevOtaBegin;
  std::uint16_t out_seq_ = 0;
  std::uint8_t out_buf_[255] = {};
  std::size_t out_len_ = 0;
  std::uint32_t out_offset_ = 0;  // chunk offset (when out_type_ == DevOtaChunk)
  std::size_t out_data_len_ = 0;
  std::uint32_t sent_ms_ = 0;
  int attempts_ = 0;

  std::uint16_t seq_counter_ = 1;
  WakeFn wake_fn_ = nullptr;
  void* wake_ctx_ = nullptr;
  PhaseFn phase_fn_ = nullptr;
  void* phase_ctx_ = nullptr;
};

}  // namespace core
