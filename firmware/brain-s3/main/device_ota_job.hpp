// DeviceOtaJob: pushes the firmware of a Zigbee *device* to the coordinator,
// which serves it over the OTA Upgrade cluster (docs/ota.md). One job at a time,
// in its own task so the console and the main loop are never blocked.
//
// Threading: the sender is driven from the job task (poll) while the chip's
// answers arrive on the UART RX tasks. A mutex serializes both; the HTTP read
// happens *outside* it (into a one-chunk prefetch buffer) so a slow download
// never stalls the RX path.
#pragma once

#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "core/bus.hpp"
#include "core/device_ota_sender.hpp"

namespace fw {

class DeviceOtaJob {
 public:
  explicit DeviceOtaJob(core::Bus& bus) noexcept;

  // Start updating the device `entity_id` (on chip `chip_id`) from `url`.
  // `sha256_hex` is the 64-hex-digit digest of the .ota file. On failure
  // returns false and writes the reason to `err`.
  bool start(std::uint8_t chip_id, std::uint16_t entity_id, const char* url,
             const char* sha256_hex, char* err, std::size_t err_cap) noexcept;

  void abort() noexcept;

  // The device session as the chip reports it (waiting / transferring / done /
  // failed). Called from the bus-callback context.
  using PhaseFn = void (*)(void* ctx, std::uint16_t entity_id, hubproto::DevOtaPhase phase,
                           hubproto::DevOtaStatus status, std::uint8_t percent);
  void on_phase(PhaseFn fn, void* ctx) noexcept {
    phase_fn_ = fn;
    phase_ctx_ = ctx;
  }

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
  static void phase_cb(void* ctx, std::uint16_t entity_id, hubproto::DevOtaPhase phase,
                       hubproto::DevOtaStatus status, std::uint8_t percent);
  void run() noexcept;
  void finish(bool ok, const char* msg) noexcept;

  SemaphoreHandle_t mtx_;
  SemaphoreHandle_t wake_;
  LockedBus bus_;
  core::DeviceOtaSender sender_;

  volatile bool running_ = false;
  volatile bool abort_ = false;

  std::uint8_t chip_id_ = 0;
  std::uint16_t entity_id_ = 0;
  char url_[256] = {};
  std::uint8_t sha256_[32] = {};
  std::uint32_t total_ = 0;
  std::uint32_t started_ms_ = 0;

  // The device session, as the chip reports it (written by the bus callback).
  volatile hubproto::DevOtaPhase phase_ = hubproto::DevOtaPhase::Idle;
  volatile hubproto::DevOtaStatus phase_status_ = hubproto::DevOtaStatus::Ok;
  volatile std::uint8_t percent_ = 0;
  volatile bool session_done_ = false;

  // One chunk read ahead from HTTP, handed to the sender by read_cb.
  std::uint8_t pre_[hubproto::kDevOtaMaxChunk] = {};
  std::size_t pre_len_ = 0;
  std::size_t pre_pos_ = 0;

  char result_[128] = "no device update run since boot";
  PhaseFn phase_fn_ = nullptr;
  void* phase_ctx_ = nullptr;
};

}  // namespace fw
