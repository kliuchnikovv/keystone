// OtaJob: downloads a firmware image over HTTP(S) and streams it to a
// co-processor with core::OtaSender (docs/ota.md). One job at a time, run in its
// own task so the console and the main loop are never blocked.
//
// Threading: the sender is driven from the job task (poll) while acks arrive on
// the UART RX tasks. A mutex serializes both; the HTTP read happens *outside* it
// (into a one-chunk prefetch buffer) so a slow download never stalls the RX path.
#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "core/bus.hpp"
#include "core/ota_sender.hpp"

namespace fw {

class OtaJob {
 public:
  explicit OtaJob(core::Bus& bus) noexcept;

  // Start updating `chip_id` from `url`. `sha256_hex` is the 64-hex-digit digest
  // of the image, obtained out of band. On failure returns false and writes the
  // reason to `err`.
  bool start(std::uint8_t chip_id, const char* url, const char* sha256_hex,
             char* err, std::size_t err_cap) noexcept;

  void abort() noexcept;

  // Called from the job task when an image was accepted by the chip: the chip is
  // about to reboot into it. `sha256` is the image digest.
  using AcceptedFn = void (*)(void* ctx, std::uint8_t chip_id, const std::uint8_t* sha256);
  void on_accepted(AcceptedFn fn, void* ctx) noexcept { accepted_fn_ = fn; accepted_ctx_ = ctx; }

  bool running() const noexcept { return running_; }

  // One human-readable line: idle / progress / last result.
  void describe(char* out, std::size_t cap) noexcept;

 private:
  // Delivers inbound messages to the sender under the job mutex.
  class LockedBus final : public core::Bus {
   public:
    LockedBus(core::Bus& inner, SemaphoreHandle_t mtx) noexcept : inner_(inner), mtx_(mtx) {}
    bool publish(const hubproto::Header& h, const std::uint8_t* p, std::size_t n) noexcept override {
      return inner_.publish(h, p, n);
    }
    bool subscribe(MessageFn fn, void* ctx) noexcept override {
      fn_ = fn;
      ctx_ = ctx;
      return inner_.subscribe(&LockedBus::tramp, this);
    }

   private:
    static void tramp(void* self, const hubproto::Header& h, const std::uint8_t* p,
                      std::size_t n) noexcept {
      auto* b = static_cast<LockedBus*>(self);
      xSemaphoreTake(b->mtx_, portMAX_DELAY);
      b->fn_(b->ctx_, h, p, n);
      xSemaphoreGive(b->mtx_);
    }
    core::Bus& inner_;
    SemaphoreHandle_t mtx_;
    MessageFn fn_ = nullptr;
    void* ctx_ = nullptr;
  };

  static void task_entry(void* arg);
  static std::size_t read_cb(void* ctx, std::uint8_t* buf, std::size_t cap);
  static void wake_cb(void* ctx);
  void run() noexcept;
  void finish(bool ok, const char* msg) noexcept;

  SemaphoreHandle_t mtx_;
  SemaphoreHandle_t wake_;
  LockedBus bus_;
  core::OtaSender sender_;

  volatile bool running_ = false;
  volatile bool abort_ = false;

  std::uint8_t chip_id_ = 0;
  char url_[256] = {};
  std::uint8_t sha256_[32] = {};
  std::uint32_t total_ = 0;
  std::uint32_t started_ms_ = 0;

  // One chunk read ahead from HTTP, handed to the sender by read_cb.
  std::uint8_t pre_[hubproto::kOtaMaxChunk] = {};
  std::size_t pre_len_ = 0;
  std::size_t pre_pos_ = 0;

  char result_[96] = "no update run since boot";
  AcceptedFn accepted_fn_ = nullptr;
  void* accepted_ctx_ = nullptr;
};

}  // namespace fw
