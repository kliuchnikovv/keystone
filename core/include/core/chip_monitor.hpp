// ChipMonitor: the brain's own view of each co-processor's liveness (SPEC
// Invariant: health is judged from the inbound stream, never from the chip's
// word). Any message from a chip counts as a sign of life; the chips send a
// HEARTBEAT every few seconds so an idle chip still shows up.
//
//   - Silent: nothing heard for `silent_after_ms` -> Lost event, alive() false
//     (CommandDispatcher uses that to fail commands fast instead of retrying
//     into the void). Every `probe_interval_ms` a silent chip is sent a QUERY,
//     which it answers with DECLAREs, so a chip that recovered on its own is
//     noticed promptly.
//   - Resumed: a silent chip speaks again.
//   - Rebooted: its boot_id changed. Flapping: `flap_threshold` reboots within
//     `flap_window_ms` (a crash loop, or an OTA image the bootloader keeps
//     reverting).
//
// Time is injected, the core stays clock-free. Radio-free and portable.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/version.hpp"

namespace core {

class ChipMonitor {
 public:
  static constexpr std::size_t kMaxChips = 16;   // indexed by chip_id
  static constexpr std::size_t kBootHistory = 8;

  struct Config {
    std::uint32_t silent_after_ms = 20000;    // 4 missed heartbeats at 5 s
    std::uint32_t probe_interval_ms = 10000;  // QUERY to a silent chip
    std::uint32_t flap_window_ms = 300000;
    unsigned      flap_threshold = 3;
  };

  enum class Event : std::uint8_t { Lost, Resumed, Rebooted, Flapping };
  // value: Lost -> ms since last rx; Resumed -> ms it was silent;
  //        Rebooted -> total reboots seen; Flapping -> reboots in the window.
  using EventFn = void (*)(void* ctx, std::uint8_t chip_id, Event ev, std::uint32_t value);
  using NowFn = std::uint32_t (*)(void* ctx);

  struct Info {
    bool          seen = false;
    bool          silent = false;
    std::uint8_t  boot_id = 0;
    std::uint32_t rx_frames = 0;
    std::uint32_t last_rx_ms = 0;
    std::uint32_t reboots = 0;            // boot_id changes observed
    std::uint32_t silent_since_ms = 0;
    std::uint32_t last_probe_ms = 0;
    std::uint32_t boot_times[kBootHistory] = {};
    std::size_t   boot_count_in_ring = 0;
    std::size_t   boot_next = 0;
  };

  ChipMonitor(Bus& bus, NowFn now_fn, void* now_ctx, Config cfg) noexcept
      : bus_(bus), now_fn_(now_fn), now_ctx_(now_ctx), cfg_(cfg) {
    bus_.subscribe(&ChipMonitor::on_msg, this);
  }
  ChipMonitor(Bus& bus, NowFn now_fn, void* now_ctx) noexcept
      : ChipMonitor(bus, now_fn, now_ctx, Config{}) {}

  void on_event(EventFn fn, void* ctx) noexcept { ev_fn_ = fn; ev_ctx_ = ctx; }

  // Usable as CommandDispatcher's alive filter. Chips never heard from are not
  // blocked: the registry already refuses commands for unknown entities.
  bool alive(std::uint8_t chip_id) const noexcept {
    if (chip_id >= kMaxChips) return true;
    const Info& c = chips_[chip_id];
    return !(c.seen && c.silent);
  }
  static bool alive_thunk(void* ctx, std::uint8_t chip_id) noexcept {
    return static_cast<const ChipMonitor*>(ctx)->alive(chip_id);
  }

  const Info* info(std::uint8_t chip_id) const noexcept {
    return (chip_id < kMaxChips && chips_[chip_id].seen) ? &chips_[chip_id] : nullptr;
  }

  // Reboots of `chip_id` within the flap window ending at now.
  unsigned recent_reboots(std::uint8_t chip_id, std::uint32_t now_ms) const noexcept {
    if (chip_id >= kMaxChips) return 0;
    const Info& c = chips_[chip_id];
    unsigned n = 0;
    for (std::size_t i = 0; i < c.boot_count_in_ring; ++i) {
      if (now_ms - c.boot_times[i] <= cfg_.flap_window_ms) ++n;
    }
    return n;
  }

  // Drive from the main loop (about once a second is plenty).
  void poll(std::uint32_t now_ms) noexcept {
    for (std::size_t id = 1; id < kMaxChips; ++id) {
      Info& c = chips_[id];
      if (!c.seen) continue;
      if (!c.silent) {
        if (now_ms - c.last_rx_ms >= cfg_.silent_after_ms) {
          c.silent = true;
          c.silent_since_ms = now_ms;
          c.last_probe_ms = now_ms;
          emit(static_cast<std::uint8_t>(id), Event::Lost, now_ms - c.last_rx_ms);
        }
      } else if (now_ms - c.last_probe_ms >= cfg_.probe_interval_ms) {
        c.last_probe_ms = now_ms;
        hubproto::Header q{};
        q.version = hubproto::kProtocolVersion;
        q.type = hubproto::MsgType::Query;
        q.chip_id = static_cast<std::uint8_t>(id);
        bus_.publish(q, nullptr, 0);
      }
    }
  }

 private:
  static void on_msg(void* ctx, const hubproto::Header& h, const std::uint8_t*,
                     std::size_t) noexcept {
    static_cast<ChipMonitor*>(ctx)->handle(h);
  }

  void handle(const hubproto::Header& h) noexcept {
    if (h.chip_id == 0 || h.chip_id >= kMaxChips) return;
    const std::uint32_t now = now_fn_ != nullptr ? now_fn_(now_ctx_) : 0;
    Info& c = chips_[h.chip_id];
    const bool first = !c.seen;
    const std::uint32_t prev_rx = c.last_rx_ms;
    c.seen = true;
    ++c.rx_frames;
    c.last_rx_ms = now;

    if (c.silent) {
      c.silent = false;
      emit(h.chip_id, Event::Resumed, now - prev_rx);
    }
    if (h.boot_id == 0) return;  // not stamped (no information)
    if (first) {
      c.boot_id = h.boot_id;
    } else if (h.boot_id != c.boot_id) {
      c.boot_id = h.boot_id;
      ++c.reboots;
      c.boot_times[c.boot_next] = now;
      c.boot_next = (c.boot_next + 1) % kBootHistory;
      if (c.boot_count_in_ring < kBootHistory) ++c.boot_count_in_ring;
      emit(h.chip_id, Event::Rebooted, c.reboots);
      const unsigned recent = recent_reboots(h.chip_id, now);
      if (recent >= cfg_.flap_threshold) emit(h.chip_id, Event::Flapping, recent);
    }
  }

  void emit(std::uint8_t chip, Event ev, std::uint32_t value) noexcept {
    if (ev_fn_ != nullptr) ev_fn_(ev_ctx_, chip, ev, value);
  }

  Bus& bus_;
  NowFn now_fn_;
  void* now_ctx_;
  Config cfg_;
  Info chips_[kMaxChips];
  EventFn ev_fn_ = nullptr;
  void* ev_ctx_ = nullptr;
};

}  // namespace core
