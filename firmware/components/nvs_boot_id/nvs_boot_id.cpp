#include "nvs_boot_id.hpp"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

namespace fw {

namespace {
const char* kTag = "nvs_boot_id";
const char* kNamespace = "keystone";
const char* kKey = "boot_id";
}  // namespace

std::uint8_t next_boot_id() noexcept {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGW(kTag, "nvs init failed (%d); boot_id unavailable", static_cast<int>(err));
    return 0;
  }

  nvs_handle_t h;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return 0;

  std::uint8_t bid = 0;
  nvs_get_u8(h, kKey, &bid);  // absent -> bid stays 0
  bid = (bid == 0xFF) ? 1 : static_cast<std::uint8_t>(bid + 1);  // wrap, skip 0
  nvs_set_u8(h, kKey, bid);
  nvs_commit(h);
  nvs_close(h);
  return bid;
}

}  // namespace fw
