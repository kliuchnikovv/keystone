// OtaReceiver: the co-processor side of a firmware update (see hubproto OTA
// messages). Writes the image through an abstract `OtaSink` (ESP-IDF: esp_ota_*;
// host tests: RAM), verifies size + SHA-256, and asks for a reboot — it never
// reboots by itself. Radio-free and clock-free (time is injected via poll()).
//
// Replies carry the chip's own chip_id and boot_id: RadioCoordinator treats a
// boot_id mismatch on ANY message as a reboot and wipes the chip's entities, so
// a wrong boot_id here would corrupt the brain's registry mid-update.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "core/sha256.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace core {

// Where the image bytes go. All calls come from the thread that feeds the Bus.
class OtaSink {
 public:
  virtual ~OtaSink() = default;

  // Prepare the inactive slot for `total_size` bytes (erase). Return TooLarge if
  // it does not fit, Unsupported if the chip cannot be updated, WriteFail on a
  // flash error, Ok otherwise.
  virtual hubproto::OtaStatus begin(std::uint32_t total_size) noexcept = 0;

  // Append the next bytes. False on a flash error.
  virtual bool write(const std::uint8_t* data, std::size_t len) noexcept = 0;

  // The size and SHA-256 are already checked. Validate the image (header,
  // checksum) and make it the next boot image. VerifyFail if it is not valid.
  virtual hubproto::OtaStatus finish() noexcept = 0;

  // Drop a partially written image.
  virtual void abort() noexcept = 0;

  // Called after finish() succeeded, with the image's SHA-256: remember it (and
  // which slot it went to) so the brain can later ask whether it is the image
  // that is running (see OtaInfo). Default: nothing to remember.
  virtual void record_image(const std::uint8_t* /*sha256*/) noexcept {}

  // Describe the running image for OTA_INFO. False = this sink cannot say.
  virtual bool info(hubproto::OtaInfo& /*out*/) const noexcept { return false; }
};

class OtaReceiver {
 public:
  struct Config {
    std::uint8_t  own_chip_id = 0;                       // goes in every reply header
    std::uint8_t  boot_id = 0;                           // ditto
    std::uint16_t chunk_size = hubproto::kOtaMaxChunk;   // offered to the brain
    std::uint32_t idle_timeout_ms = 15000;               // abort a stalled transfer
  };

  OtaReceiver(Bus& bus, OtaSink& sink, Config cfg) noexcept;

  // Advance time; aborts a transfer that has been silent for idle_timeout_ms.
  // Call at least every ~100 ms. Activity is stamped with the latest poll time.
  void poll(std::uint32_t now_ms) noexcept;

  // A transfer is in progress (between a good BEGIN and END/abort).
  bool active() const noexcept { return state_ == State::Receiving; }

  // The image was accepted and END_ACK(Ok) was sent: the firmware should let the
  // UART drain, then restart into the new image.
  bool reboot_pending() const noexcept { return state_ == State::Finished; }

  std::uint32_t received() const noexcept { return expected_; }

 private:
  enum class State : std::uint8_t { Idle, Receiving, Finished };

  static void on_msg(void* ctx, const hubproto::Header& h,
                     const std::uint8_t* payload, std::size_t len) noexcept {
    static_cast<OtaReceiver*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void on_begin(const hubproto::Header& h, const std::uint8_t* p, std::size_t n) noexcept;
  void on_chunk(const hubproto::Header& h, const std::uint8_t* p, std::size_t n) noexcept;
  void on_end(const hubproto::Header& h) noexcept;
  void on_info_req(const hubproto::Header& h) noexcept;
  void drop() noexcept;  // abort the sink, return to Idle
  void reply(hubproto::MsgType type, const hubproto::Header& req,
             const std::uint8_t* payload, std::size_t len) noexcept;
  void reply_chunk_ack(const hubproto::Header& req, hubproto::OtaStatus s) noexcept;
  void reply_end_ack(const hubproto::Header& req, hubproto::OtaStatus s) noexcept;

  Bus& bus_;
  OtaSink& sink_;
  Config cfg_;
  State state_ = State::Idle;
  std::uint32_t total_ = 0;
  std::uint32_t expected_ = 0;  // next offset we want
  std::uint8_t sha_expected_[hubproto::kOtaSha256Size] = {};
  Sha256 sha_;
  std::uint32_t now_ms_ = 0;
  std::uint32_t last_activity_ms_ = 0;
};

}  // namespace core
