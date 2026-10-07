#include "esp_ota_sink.hpp"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace fw {

namespace {
const char* kTag = "esp_ota_sink";
}

hubproto::OtaStatus EspOtaSink::begin(std::uint32_t total_size) noexcept {
  abort();  // a transfer restarted by the brain replaces the old one
  part_ = esp_ota_get_next_update_partition(nullptr);
  if (part_ == nullptr) {
    ESP_LOGE(kTag, "no OTA partition (partition table without ota_0/ota_1?)");
    return hubproto::OtaStatus::Unsupported;
  }
  if (total_size > part_->size) {
    ESP_LOGW(kTag, "image %u B does not fit slot '%s' (%u B)",
             static_cast<unsigned>(total_size), part_->label,
             static_cast<unsigned>(part_->size));
    return hubproto::OtaStatus::TooLarge;
  }
  ESP_LOGI(kTag, "begin: %u B into '%s' @0x%x (erasing...)",
           static_cast<unsigned>(total_size), part_->label,
           static_cast<unsigned>(part_->address));
  const esp_err_t err = esp_ota_begin(part_, total_size, &handle_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_begin: %s", esp_err_to_name(err));
    return hubproto::OtaStatus::WriteFail;
  }
  open_ = true;
  return hubproto::OtaStatus::Ok;
}

bool EspOtaSink::write(const std::uint8_t* data, std::size_t len) noexcept {
  if (!open_) return false;
  const esp_err_t err = esp_ota_write(handle_, data, len);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_write: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

hubproto::OtaStatus EspOtaSink::finish() noexcept {
  if (!open_) return hubproto::OtaStatus::BadState;
  open_ = false;
  esp_err_t err = esp_ota_end(handle_);  // validates the image header + checksum
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_end: %s", esp_err_to_name(err));
    return hubproto::OtaStatus::VerifyFail;
  }
  err = esp_ota_set_boot_partition(part_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
    return hubproto::OtaStatus::WriteFail;
  }
  ESP_LOGI(kTag, "image accepted; next boot from '%s'", part_->label);
  return hubproto::OtaStatus::Ok;
}

void EspOtaSink::abort() noexcept {
  if (!open_) return;
  esp_ota_abort(handle_);
  open_ = false;
}

bool ota_confirm_running_image() noexcept {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state{};
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    return false;
  }
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "mark_app_valid: %s", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(kTag, "new image '%s' confirmed (brain is talking to us)", running->label);
  return true;
}

[[noreturn]] void ota_reboot_into_new_image(int uart_port) noexcept {
  ESP_LOGI(kTag, "rebooting into the new image");
  uart_wait_tx_done(static_cast<uart_port_t>(uart_port), pdMS_TO_TICKS(500));
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  for (;;) {}
}

}  // namespace fw
