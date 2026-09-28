// KeystoneBridgedDevice: the app_bridged_device_t subclass we hand to
// app_bridge_* so it can persist "which brain entity is behind Matter
// endpoint N" in NVS. Address = (chip_id, entity_id) of the underlying
// hubproto entity the endpoint mirrors.
#pragma once

#include <cstdint>
#include <app_bridged_device.h>

namespace keystone {

struct BridgedAddr {
  std::uint8_t  chip_id;
  std::uint16_t entity_id;
};

class BridgedDevice : public app_bridged_device_t {
 public:
  esp_err_t set_dev_addr(const void* addr) override;
  bool      check_dev_addr(const void* addr) override;
  esp_err_t delete_dev_addr() override;
  esp_err_t store_dev_addr() override;
  esp_err_t restore_dev_addr() override;
  esp_err_t erase_dev_addr() override;

  BridgedAddr addr() const noexcept;
};

}  // namespace keystone
