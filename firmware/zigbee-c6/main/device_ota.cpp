#include "device_ota.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"

extern "C" {
#include "zboss_api.h"
}
#include "esp_zigbee_core.h"
#include "esp_zigbee_ota.h"

#include "core/zigbee_ota.hpp"

namespace fw {

namespace {

const char* kTag = "zb-ota";

constexpr const char* kPartitionName = "zb_ota";
constexpr std::uint8_t kServerEndpoint = 1;  // the coordinator endpoint
constexpr std::uint8_t kFileIndex = 0;
constexpr std::uint32_t kCrcChunk = 1024;

// The stack calls this when it needs the next block of the file.
ZigbeeOtaServer* g_server = nullptr;

extern "C" esp_err_t zb_ota_next_data_cb(esp_zb_ota_zcl_information_t message, uint16_t index,
                                         uint8_t size, uint8_t** data) {
  (void)message;
  if (index != kFileIndex || g_server == nullptr) {
    ESP_LOGW(kTag, "OTA data callback for an unknown file index %u", static_cast<unsigned>(index));
    return ESP_FAIL;
  }
  return g_server->serve(size, data);
}

}  // namespace

ZigbeeOtaServer::ZigbeeOtaServer() noexcept { g_server = this; }

void ZigbeeOtaServer::report(hubproto::DevOtaPhase phase, hubproto::DevOtaStatus status,
                              std::uint8_t percent) noexcept {
  if (phase_fn_ != nullptr) phase_fn_(phase_ctx_, phase, status, percent);
}

hubproto::DevOtaStatus ZigbeeOtaServer::begin(std::uint16_t entity_id,
                                               std::uint32_t total_size) noexcept {
  if (serving_) {
    ESP_LOGW(kTag, "an update is already running");
    return hubproto::DevOtaStatus::Busy;
  }
  part_ = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                    kPartitionName);
  if (part_ == nullptr) {
    ESP_LOGE(kTag, "no '%s' partition", kPartitionName);
    return hubproto::DevOtaStatus::Unsupported;
  }
  if (total_size > part_->size) {
    ESP_LOGE(kTag, "image is %u B, the partition holds %u B", static_cast<unsigned>(total_size),
             static_cast<unsigned>(part_->size));
    return hubproto::DevOtaStatus::TooLarge;
  }
  target_ = (resolve_fn_ != nullptr) ? resolve_fn_(resolve_ctx_, entity_id) : 0;
  if (target_ == 0) {
    ESP_LOGE(kTag, "entity 0x%04x is not a known device", static_cast<unsigned>(entity_id));
    return hubproto::DevOtaStatus::Mismatch;
  }

  // The partition is erased up front: the image arrives in one piece and the
  // device pulls it later, so a partial file must never look complete. Only the
  // part we need is erased (sector aligned) - a full 512 KB erase takes seconds.
  const std::uint32_t erase_len = (total_size + 4095u) & ~4095u;
  const esp_err_t err = esp_partition_erase_range(part_, 0, erase_len);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "cannot erase the image partition: %s", esp_err_to_name(err));
    part_ = nullptr;
    return hubproto::DevOtaStatus::BadChecksum;
  }
  stored_ = 0;
  offset_ = 0;
  image_size_ = 0;
  manufacturer_ = 0;
  image_type_ = 0;
  file_version_ = 0;
  last_percent_ = 0;
  ESP_LOGI(kTag, "ready for %u B of firmware for device 0x%04x",
           static_cast<unsigned>(total_size), static_cast<unsigned>(target_));
  return hubproto::DevOtaStatus::Ok;
}

bool ZigbeeOtaServer::write(const std::uint8_t* data, std::size_t len) noexcept {
  if (part_ == nullptr || data == nullptr) return false;
  if (stored_ + len > part_->size) return false;
  const esp_err_t err = esp_partition_write(part_, stored_, data, static_cast<std::size_t>(len));
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "flash write failed at %u: %s", static_cast<unsigned>(stored_),
             esp_err_to_name(err));
    return false;
  }
  stored_ += static_cast<std::uint32_t>(len);
  return true;
}

hubproto::DevOtaStatus ZigbeeOtaServer::finish() noexcept {
  if (part_ == nullptr || stored_ < core::zota::kBaseHeaderSize + core::zota::kCrcSize) {
    return hubproto::DevOtaStatus::BadFormat;
  }

  // 1. the Zigbee OTA header
  std::uint8_t head[core::zota::kBaseHeaderSize + 64];
  const std::size_t head_len = (stored_ < sizeof(head)) ? stored_ : sizeof(head);
  if (esp_partition_read(part_, 0, head, head_len) != ESP_OK) {
    ESP_LOGE(kTag, "cannot read the image header");
    return hubproto::DevOtaStatus::BadFormat;
  }
  core::zota::FileHeader hdr{};
  if (!core::zota::parse_header(head, head_len, hdr)) {
    ESP_LOGE(kTag, "not a Zigbee OTA file");
    return hubproto::DevOtaStatus::BadFormat;
  }
  if (hdr.image_size > stored_) {
    ESP_LOGE(kTag, "the header says %u B but we have %u B", static_cast<unsigned>(hdr.image_size),
             static_cast<unsigned>(stored_));
    return hubproto::DevOtaStatus::BadFormat;
  }

  // 2. the CRC32 over everything but the last four bytes
  core::zota::Crc32 crc;
  crc.reset();
  std::uint8_t buf[kCrcChunk];
  std::uint32_t left = stored_ - core::zota::kCrcSize;
  std::uint32_t at = 0;
  while (left > 0) {
    const std::size_t n = (left < kCrcChunk) ? left : kCrcChunk;
    if (esp_partition_read(part_, at, buf, n) != ESP_OK) {
      ESP_LOGE(kTag, "cannot read the image back");
      return hubproto::DevOtaStatus::BadChecksum;
    }
    crc.update(buf, n);
    at += static_cast<std::uint32_t>(n);
    left -= static_cast<std::uint32_t>(n);
  }
  std::uint8_t tail[core::zota::kCrcSize];
  if (esp_partition_read(part_, stored_ - core::zota::kCrcSize, tail, sizeof(tail)) != ESP_OK) {
    return hubproto::DevOtaStatus::BadChecksum;
  }
  const std::uint32_t want = static_cast<std::uint32_t>(tail[0]) |
                             (static_cast<std::uint32_t>(tail[1]) << 8) |
                             (static_cast<std::uint32_t>(tail[2]) << 16) |
                             (static_cast<std::uint32_t>(tail[3]) << 24);
  if (crc.value() != want) {
    ESP_LOGE(kTag, "CRC32 mismatch (file %08x, computed %08x)", static_cast<unsigned>(want),
             static_cast<unsigned>(crc.value()));
    return hubproto::DevOtaStatus::BadChecksum;
  }

  manufacturer_ = hdr.manufacturer_code;
  image_type_ = hdr.image_type;
  file_version_ = hdr.file_version;
  image_size_ = hdr.image_size;
  offset_ = 0;
  last_percent_ = 0;
  serving_ = true;

  ESP_LOGI(kTag, "firmware ok: manufacturer 0x%04x, image type 0x%04x, version %u, %u B",
           static_cast<unsigned>(manufacturer_), static_cast<unsigned>(image_type_),
           static_cast<unsigned>(file_version_), static_cast<unsigned>(image_size_));

  // 3. hand it to the stack and tell the device that an image is available
  esp_zb_ota_upgrade_server_notify_req_t req = {};
  req.endpoint = kServerEndpoint;
  req.index = kFileIndex;
  req.notify_on = 1;
  req.ota_upgrade_time = 0;  // upgrade as soon as the transfer is done
  req.ota_file_header.manufacturer_code = manufacturer_;
  req.ota_file_header.image_type = image_type_;
  req.ota_file_header.file_version = file_version_;
  req.ota_file_header.image_size = image_size_;
  req.ota_file_header.field_control = 0;
  req.next_data_cb = &zb_ota_next_data_cb;

  esp_err_t err = ESP_FAIL;
  if (esp_zb_lock_acquire(portMAX_DELAY)) {
    err = esp_zb_ota_upgrade_server_notify_req(&req);
    esp_zb_lock_release();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "the stack refused the image: %s", esp_err_to_name(err));
    serving_ = false;
    return hubproto::DevOtaStatus::BadFormat;
  }
  report(hubproto::DevOtaPhase::Notifying, hubproto::DevOtaStatus::Ok, 0);
  return hubproto::DevOtaStatus::Ok;
}

void ZigbeeOtaServer::abort() noexcept {
  serving_ = false;
  offset_ = 0;
  stored_ = 0;
  ESP_LOGW(kTag, "the firmware transfer was dropped");
}

esp_err_t ZigbeeOtaServer::serve(std::uint8_t size, std::uint8_t** data) noexcept {
  if (!serving_ || part_ == nullptr || data == nullptr) return ESP_FAIL;
  // size is a uint8_t, so it always fits the 256-byte block buffer.
  if (offset_ >= image_size_) {
    // The device got everything: start over for the next pull.
    offset_ = 0;
    return ESP_FAIL;
  }
  std::uint32_t left = image_size_ - offset_;
  if (left < size) size = static_cast<std::uint8_t>(left);
  if (esp_partition_read(part_, offset_, block_, size) != ESP_OK) {
    ESP_LOGE(kTag, "cannot read the block at %u", static_cast<unsigned>(offset_));
    return ESP_FAIL;
  }
  *data = block_;
  offset_ += size;

  const std::uint8_t percent = static_cast<std::uint8_t>(
      (static_cast<std::uint64_t>(offset_) * 100) / (image_size_ == 0 ? 1 : image_size_));
  if (percent != last_percent_ && (percent % 5 == 0 || percent == 100)) {
    last_percent_ = percent;
    report(hubproto::DevOtaPhase::Transferring, hubproto::DevOtaStatus::Ok, percent);
  }
  return ESP_OK;
}

void ZigbeeOtaServer::on_query_image(std::uint16_t src_short_addr, std::uint16_t manufacturer,
                                      std::uint16_t image_type, bool accepted) noexcept {
  if (accepted) {
    offset_ = 0;
    last_percent_ = 0;
    ESP_LOGI(kTag, "device 0x%04x asked for the image: serving it", static_cast<unsigned>(src_short_addr));
    report(hubproto::DevOtaPhase::Transferring, hubproto::DevOtaStatus::Ok, 0);
  } else {
    ESP_LOGW(kTag,
             "device 0x%04x asked for manufacturer 0x%04x / image type 0x%04x - we have "
             "0x%04x / 0x%04x: no image for it",
             static_cast<unsigned>(src_short_addr), static_cast<unsigned>(manufacturer),
             static_cast<unsigned>(image_type), static_cast<unsigned>(manufacturer_),
             static_cast<unsigned>(image_type_));
  }
}

void ZigbeeOtaServer::on_upgrade_end(std::uint16_t src_short_addr, bool success) noexcept {
  if (success) {
    ESP_LOGI(kTag, "device 0x%04x finished the update", static_cast<unsigned>(src_short_addr));
    report(hubproto::DevOtaPhase::Done, hubproto::DevOtaStatus::Ok, 100);
  } else {
    ESP_LOGW(kTag, "device 0x%04x aborted the update", static_cast<unsigned>(src_short_addr));
    report(hubproto::DevOtaPhase::Failed, hubproto::DevOtaStatus::Mismatch, 0);
  }
  serving_ = false;
  offset_ = 0;
}

}  // namespace fw
