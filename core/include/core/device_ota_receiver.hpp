// DeviceOtaReceiver: the coordinator side of a *device* firmware update (see
// hubproto "device OTA" messages). Receives a vendor .ota file from the brain,
// writes it through an abstract `DeviceOtaSink` (ESP-IDF: a flash partition; host
// tests: RAM), checks size + SHA-256, and then lets the sink register the image
// with the Zigbee OTA server so the device can pull it.
//
// Radio-free and clock-free (time is injected via poll()).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "core/sha256.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace core {

// Where the .ota file goes. All calls come from the thread that feeds the Bus.
class DeviceOtaSink {
 public:
  virtual ~DeviceOtaSink() = default;

  // Prepare storage for `total_size` bytes of a file for the device `entity_id`
  // (erase). Return Ok, Busy (another update is running), TooLarge (does not fit)
  // or Unsupported (this chip cannot serve OTA to devices).
  virtual hubproto::DevOtaStatus begin(std::uint16_t entity_id,
                                       std::uint32_t total_size) noexcept = 0;

  // Append the next bytes. False on a flash error.
  virtual bool write(const std::uint8_t* data, std::size_t len) noexcept = 0;

  // The size and SHA-256 are already checked. Validate the Zigbee OTA file (header,
  // CRC32), register it with the OTA server and notify the device. Return BadFormat
  // if it is not a Zigbee OTA file, BadChecksum if its CRC32 does not match.
  virtual hubproto::DevOtaStatus finish() noexcept = 0;

  // Drop a partially written file.
  virtual void abort() noexcept = 0;
};

class DeviceOtaReceiver {
 public:
  struct Config {
    std::uint8_t  own_chip_id = 0;                       // goes in every reply header
    std::uint8_t  boot_id = 0;                           // ditto
    std::uint16_t chunk_size = hubproto::kDevOtaMaxChunk;  // offered to the brain
    std::uint32_t idle_timeout_ms = 15000;               // abort a stalled transfer
  };

  DeviceOtaReceiver(Bus& bus, DeviceOtaSink& sink, Config cfg) noexcept;

  // Advance time; aborts a transfer that has been silent for idle_timeout_ms.
  // Call at least every ~100 ms. Activity is stamped with the latest poll time.
  void poll(std::uint32_t now_ms) noexcept;

  // A transfer is in progress (between a good BEGIN and END/abort).
  bool active() const noexcept { return state_ == State::Receiving; }

  // The file was accepted and the device was notified.
  bool accepted() const noexcept { return state_ == State::Finished; }

  std::uint32_t received() const noexcept { return expected_; }
  std::uint16_t entity_id() const noexcept { return entity_id_; }

  // The sink reports the device session (verifying / notifying / transferring /
  // done / failed) through this: it becomes a DEV_OTA_STATUS on the bus.
  void report_phase(hubproto::DevOtaPhase phase, hubproto::DevOtaStatus status,
                     std::uint8_t percent) noexcept;

 private:
  enum class State : std::uint8_t { Idle, Receiving, Finished };

  static void on_msg(void* ctx, const hubproto::Header& h, const std::uint8_t* payload,
                     std::size_t len) noexcept {
    static_cast<DeviceOtaReceiver*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void on_begin(const hubproto::Header& h, const std::uint8_t* p, std::size_t n) noexcept;
  void on_chunk(const hubproto::Header& h, const std::uint8_t* p, std::size_t n) noexcept;
  void on_end(const hubproto::Header& h) noexcept;
  void drop() noexcept;  // abort the sink, return to Idle
  void reply(const hubproto::Header& req, hubproto::DevOtaPhase phase,
             hubproto::DevOtaStatus status, std::uint8_t percent,
             std::uint32_t next_offset) noexcept;

  Bus& bus_;
  DeviceOtaSink& sink_;
  Config cfg_;
  State state_ = State::Idle;
  std::uint16_t entity_id_ = hubproto::kSelfEntityId;
  std::uint32_t total_ = 0;
  std::uint32_t expected_ = 0;  // next offset we want
  std::uint8_t sha_expected_[hubproto::kDevOtaSha256Size] = {};
  Sha256 sha_;
  std::uint32_t now_ms_ = 0;
  std::uint32_t last_activity_ms_ = 0;
};

}  // namespace core
