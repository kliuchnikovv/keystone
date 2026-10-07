// OtaVerifier: after an OTA transfer the brain still doesn't know whether the new
// image STAYED — a bad image is reverted by the bootloader and the chip comes back
// on the old firmware, which looks exactly like a successful update. The verifier
// asks the chip (OTA_INFO_REQ) which image it runs and compares with what was sent:
//
//   - runs the slot + sha we sent, and has confirmed it  -> Verified
//   - runs a different slot / the last OTA sha differs   -> RolledBack
//   - runs the right slot but is still pending verify    -> keep waiting, and
//                                                          RunningUnconfirmed at the deadline
//   - no usable answer by the deadline                   -> NoAnswer
//
// Answers from before the chip rebooted are ignored (boot_id unchanged), so the
// old image answering in the second before it restarts is not mistaken for a
// rollback. Time is injected. Radio-free and portable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace core {

class OtaVerifier {
 public:
  enum class Verdict : std::uint8_t {
    Idle, Waiting, Verified, RunningUnconfirmed, RolledBack, NoAnswer
  };
  struct Config {
    std::uint32_t request_interval_ms = 5000;
    std::uint32_t deadline_ms = 120000;
  };
  using DoneFn = void (*)(void* ctx, std::uint8_t chip_id, Verdict v);

  OtaVerifier(Bus& bus, Config cfg) noexcept : bus_(bus), cfg_(cfg) {
    bus_.subscribe(&OtaVerifier::on_msg, this);
  }
  explicit OtaVerifier(Bus& bus) noexcept : OtaVerifier(bus, Config{}) {}

  void on_done(DoneFn fn, void* ctx) noexcept { done_fn_ = fn; done_ctx_ = ctx; }

  // Begin watching `chip_id` after it accepted an image with `sha256`.
  // `boot_id_before` is the boot_id it had when the transfer finished (0 = unknown).
  void start(std::uint8_t chip_id, const std::uint8_t* sha256,
             std::uint8_t boot_id_before, std::uint32_t now_ms) noexcept {
    chip_ = chip_id;
    std::memcpy(sha_, sha256, hubproto::kOtaSha256Size);
    boot_before_ = boot_id_before;
    started_ms_ = now_ms;
    last_req_ms_ = now_ms - cfg_.request_interval_ms;  // ask on the first poll
    unconfirmed_ = false;
    verdict_ = Verdict::Waiting;
  }

  void poll(std::uint32_t now_ms) noexcept {
    if (verdict_ != Verdict::Waiting) return;
    if (now_ms - started_ms_ >= cfg_.deadline_ms) {
      finish(unconfirmed_ ? Verdict::RunningUnconfirmed : Verdict::NoAnswer);
      return;
    }
    if (now_ms - last_req_ms_ >= cfg_.request_interval_ms) {
      last_req_ms_ = now_ms;
      hubproto::Header h{};
      h.version = hubproto::kProtocolVersion;
      h.type = hubproto::MsgType::OtaInfoReq;
      h.chip_id = chip_;
      bus_.publish(h, nullptr, 0);
    }
  }

  Verdict verdict() const noexcept { return verdict_; }
  std::uint8_t chip() const noexcept { return chip_; }
  bool waiting() const noexcept { return verdict_ == Verdict::Waiting; }

 private:
  static void on_msg(void* ctx, const hubproto::Header& h, const std::uint8_t* p,
                     std::size_t n) noexcept {
    static_cast<OtaVerifier*>(ctx)->handle(h, p, n);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* p, std::size_t n) noexcept {
    if (verdict_ != Verdict::Waiting || h.type != hubproto::MsgType::OtaInfo ||
        h.chip_id != chip_) {
      return;
    }
    if (boot_before_ != 0 && h.boot_id == boot_before_) return;  // not rebooted yet
    hubproto::OtaInfo info{};
    if (!hubproto::decode_ota_info(p, n, info)) return;

    const bool same_image =
        std::memcmp(info.last_ota_sha256, sha_, hubproto::kOtaSha256Size) == 0;
    const bool running_it = same_image && info.running_slot == info.last_ota_slot &&
                            info.running_slot != hubproto::kOtaSlotUnknown;
    if (!running_it) {
      finish(Verdict::RolledBack);
    } else if (info.confirmed) {
      finish(Verdict::Verified);
    } else {
      unconfirmed_ = true;  // new image running but not yet confirmed: ask again
    }
  }

  void finish(Verdict v) noexcept {
    verdict_ = v;
    if (done_fn_ != nullptr) done_fn_(done_ctx_, chip_, v);
  }

  Bus& bus_;
  Config cfg_;
  std::uint8_t chip_ = 0;
  std::uint8_t sha_[hubproto::kOtaSha256Size] = {};
  std::uint8_t boot_before_ = 0;
  std::uint32_t started_ms_ = 0;
  std::uint32_t last_req_ms_ = 0;
  bool unconfirmed_ = false;
  volatile Verdict verdict_ = Verdict::Idle;
  DoneFn done_fn_ = nullptr;
  void* done_ctx_ = nullptr;
};

}  // namespace core
