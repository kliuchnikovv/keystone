// The Zigbee OTA server of the coordinator (cluster 0x0019).
//
// The brain pushes a vendor .ota file to the chip (core::DeviceOtaReceiver); this
// sink stores it in a flash partition, checks it (header + CRC32) and registers it
// with the ZBOSS OTA server. The device then pulls the image block by block and we
// serve the blocks straight out of flash.
//
// The file is never held in RAM: the blocks are read from the partition into a
// small buffer when the stack asks for them.
#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"

#include "core/device_ota_receiver.hpp"
#include "hubproto/payload.hpp"

namespace fw {

class ZigbeeOtaServer final : public core::DeviceOtaSink {
 public:
  ZigbeeOtaServer() noexcept;

  // core::DeviceOtaSink
  hubproto::DevOtaStatus begin(std::uint16_t entity_id, std::uint32_t total_size) noexcept override;
  bool write(const std::uint8_t* data, std::size_t len) noexcept override;
  hubproto::DevOtaStatus finish() noexcept override;
  void abort() noexcept override;

  // Resolve an entity to the short address of the device it belongs to. Returns
  // 0 when the entity is unknown.
  using ResolveFn = std::uint16_t (*)(void* ctx, std::uint16_t entity_id);
  void set_resolver(ResolveFn fn, void* ctx) noexcept {
    resolve_fn_ = fn;
    resolve_ctx_ = ctx;
  }

  // Report the device session (the receiver turns this into DEV_OTA_STATUS).
  using PhaseFn = void (*)(void* ctx, hubproto::DevOtaPhase phase,
                            hubproto::DevOtaStatus status, std::uint8_t percent);
  void set_phase_sink(PhaseFn fn, void* ctx) noexcept {
    phase_fn_ = fn;
    phase_ctx_ = ctx;
  }

  // A device asked for an image. `accepted` is true when the request matches the
  // file we hold (manufacturer + image type + the device we update).
  void on_query_image(std::uint16_t src_short_addr, std::uint16_t manufacturer,
                      std::uint16_t image_type, bool accepted) noexcept;

  // The device finished (success) or gave up.
  void on_upgrade_end(std::uint16_t src_short_addr, bool success) noexcept;

  // Serve the next block of the file to the stack (called by the ZBOSS callback).
  esp_err_t serve(std::uint8_t size, std::uint8_t** data) noexcept;

  std::uint16_t manufacturer_code() const noexcept { return manufacturer_; }
  std::uint16_t image_type() const noexcept { return image_type_; }
  std::uint32_t file_version() const noexcept { return file_version_; }
  std::uint16_t target_short_addr() const noexcept { return target_; }

 private:
  void report(hubproto::DevOtaPhase phase, hubproto::DevOtaStatus status,
              std::uint8_t percent) noexcept;

  const esp_partition_t* part_ = nullptr;
  std::uint32_t stored_ = 0;      // bytes written to the partition
  std::uint32_t image_size_ = 0;  // bytes the device receives (the file header's image size)
  std::uint32_t offset_ = 0;      // the next byte to serve
  std::uint16_t manufacturer_ = 0;
  std::uint16_t image_type_ = 0;
  std::uint32_t file_version_ = 0;
  std::uint16_t target_ = 0;      // the device the image is for
  bool serving_ = false;
  std::uint8_t last_percent_ = 0;

  ResolveFn resolve_fn_ = nullptr;
  void* resolve_ctx_ = nullptr;
  PhaseFn phase_fn_ = nullptr;
  void* phase_ctx_ = nullptr;

  std::uint8_t block_[256] = {};
};

}  // namespace fw
