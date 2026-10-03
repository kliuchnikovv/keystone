#include "keystone_bridged_device.hpp"

#include <cstring>

#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter_mem.h>
#include <esp_matter.h>
#include <nvs.h>

#include <lib/support/CodeUtils.h>
#include <platform/ESP32/ScopedNvsHandle.h>

#define TAG "keystone-bridge"

namespace keystone {

BridgedAddr BridgedDevice::addr() const noexcept {
  BridgedAddr a{};
  if (m_dev_addr_ctx) std::memcpy(&a, m_dev_addr_ctx, sizeof(BridgedAddr));
  return a;
}

esp_err_t BridgedDevice::set_dev_addr(const void* addr) {
  if (!addr) return ESP_ERR_INVALID_ARG;
  if (m_dev_addr_ctx) esp_matter_mem_free(m_dev_addr_ctx);
  m_dev_addr_ctx = esp_matter_mem_calloc(1, sizeof(BridgedAddr));
  if (!m_dev_addr_ctx) return ESP_ERR_NO_MEM;
  std::memcpy(m_dev_addr_ctx, addr, sizeof(BridgedAddr));
  return ESP_OK;
}

bool BridgedDevice::check_dev_addr(const void* addr) {
  if (addr && m_dev_addr_ctx) {
    const auto* a = static_cast<const BridgedAddr*>(addr);
    const auto* b = static_cast<const BridgedAddr*>(m_dev_addr_ctx);
    return a->chip_id == b->chip_id && a->entity_id == b->entity_id;
  }
  return false;
}

esp_err_t BridgedDevice::delete_dev_addr() {
  if (m_dev_addr_ctx) {
    esp_matter_mem_free(m_dev_addr_ctx);
    m_dev_addr_ctx = nullptr;
  }
  return ESP_OK;
}

esp_err_t BridgedDevice::store_dev_addr() {
  VerifyOrReturnValue(m_dev, ESP_ERR_INVALID_STATE);
  chip::DeviceLayer::Internal::ScopedNvsHandle h;
  CHIP_ERROR err = h.Open(ESP_MATTER_BRIDGE_NAMESPACE, NVS_READWRITE,
                          CONFIG_ESP_MATTER_BRIDGE_INFO_PART_NAME);
  if (err != CHIP_NO_ERROR) return map_matter_error(err);
  const std::uint16_t ep = esp_matter::endpoint::get_id(m_dev->endpoint);
  ESP_RETURN_ON_ERROR(
      nvs_set_blob(h,
                   esp_matter_bridge::nvs_key_allocator::endpoint_dev_addr(ep).KeyName(),
                   m_dev_addr_ctx, sizeof(BridgedAddr)),
      TAG, "store dev_addr");
  ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
  return ESP_OK;
}

esp_err_t BridgedDevice::restore_dev_addr() {
  VerifyOrReturnValue(m_dev, ESP_ERR_INVALID_STATE);
  chip::DeviceLayer::Internal::ScopedNvsHandle h;
  CHIP_ERROR err = h.Open(ESP_MATTER_BRIDGE_NAMESPACE, NVS_READONLY,
                          CONFIG_ESP_MATTER_BRIDGE_INFO_PART_NAME);
  if (err != CHIP_NO_ERROR) return map_matter_error(err);
  if (m_dev_addr_ctx) esp_matter_mem_free(m_dev_addr_ctx);
  m_dev_addr_ctx = esp_matter_mem_calloc(1, sizeof(BridgedAddr));
  if (!m_dev_addr_ctx) return ESP_ERR_NO_MEM;
  const std::uint16_t ep = esp_matter::endpoint::get_id(m_dev->endpoint);
  size_t sz = sizeof(BridgedAddr);
  ESP_RETURN_ON_ERROR(
      nvs_get_blob(h,
                   esp_matter_bridge::nvs_key_allocator::endpoint_dev_addr(ep).KeyName(),
                   m_dev_addr_ctx, &sz),
      TAG, "read dev_addr");
  return ESP_OK;
}

esp_err_t BridgedDevice::erase_dev_addr() {
  VerifyOrReturnValue(m_dev, ESP_ERR_INVALID_STATE);
  chip::DeviceLayer::Internal::ScopedNvsHandle h;
  CHIP_ERROR err = h.Open(ESP_MATTER_BRIDGE_NAMESPACE, NVS_READWRITE,
                          CONFIG_ESP_MATTER_BRIDGE_INFO_PART_NAME);
  if (err != CHIP_NO_ERROR) return map_matter_error(err);
  const std::uint16_t ep = esp_matter::endpoint::get_id(m_dev->endpoint);
  ESP_RETURN_ON_ERROR(
      nvs_erase_key(h,
                    esp_matter_bridge::nvs_key_allocator::endpoint_dev_addr(ep).KeyName()),
      TAG, "erase dev_addr");
  ESP_RETURN_ON_ERROR(nvs_commit(h), TAG, "commit");
  return ESP_OK;
}

}  // namespace keystone
