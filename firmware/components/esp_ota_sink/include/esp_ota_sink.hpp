// EspOtaSink: writes an OTA image from the brain into the inactive app slot.
//
// Rollback model: the new image boots in "pending verify" state and the
// bootloader reverts to the old one on the next reset unless the app confirms
// itself. See OtaConfirmGate for when we do.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_ota_ops.h"

#include "core/ota_receiver.hpp"

namespace fw {

class EspOtaSink final : public core::OtaSink {
 public:
  hubproto::OtaStatus begin(std::uint32_t total_size) noexcept override;
  bool write(const std::uint8_t* data, std::size_t len) noexcept override;
  hubproto::OtaStatus finish() noexcept override;
  void abort() noexcept override;

 private:
  const esp_partition_t* part_ = nullptr;
  esp_ota_handle_t handle_ = 0;
  bool open_ = false;
};

// Mark the running image valid. A no-op unless it is still pending verification.
// Prefer OtaConfirmGate over calling this directly. Returns true if it confirmed.
bool ota_confirm_running_image() noexcept;

// Decides WHEN to confirm a freshly updated image. Confirming the moment the
// brain first talks to us is too early: a crash a moment later (seen in
// practice: a boot-time race) happens after the image was already marked valid,
// so the bootloader cannot roll it back and the chip crash-loops. Instead the
// image is confirmed only after the link to the brain worked AND the app has
// stayed up for `grace_ms`; a crash-looping image never gets there, so the
// bootloader reverts to the previous one on its own.
class OtaConfirmGate {
 public:
  explicit OtaConfirmGate(std::uint32_t grace_ms = 30000) noexcept : grace_ms_(grace_ms) {}
  void note_link_ok() noexcept { link_ok_ = true; }  // e.g. the brain's QUERY arrived
  // Call periodically with milliseconds since boot.
  void tick(std::uint32_t uptime_ms) noexcept {
    if (!done_ && link_ok_ && uptime_ms >= grace_ms_) {
      done_ = true;
      ota_confirm_running_image();
    }
  }

 private:
  std::uint32_t grace_ms_;
  volatile bool link_ok_ = false;
  bool done_ = false;
};

// Let the UART drain, then restart into the new image. Does not return.
[[noreturn]] void ota_reboot_into_new_image(int uart_port) noexcept;

}  // namespace fw
