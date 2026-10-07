// EspOtaSink: writes an OTA image from the brain into the inactive app slot.
//
// Rollback model: the new image boots in "pending verify" state and the
// bootloader reverts to the old one on the next reset unless the app confirms
// itself. We confirm only once the brain has talked to us (its QUERY), which
// proves the UART link and the app's core loop work on the new image.
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

// Call once the app has proven itself (see header comment). A no-op unless the
// running image is still pending verification. Returns true if it confirmed.
bool ota_confirm_running_image() noexcept;

// Let the UART drain, then restart into the new image. Does not return.
[[noreturn]] void ota_reboot_into_new_image(int uart_port) noexcept;

}  // namespace fw
