// RadioCoordinator: the brain side of the declaration lifecycle (SPEC §4.4).
//
// Subscribes to a Bus and drives QUERY -> DECLARE -> DECLARE_END:
//   - on HELLO(chip): clear that chip's entities and send QUERY (a HELLO always
//     starts a fresh declaration cycle; this also covers boot_id resync, since a
//     reboot re-sends HELLO with a new boot_id).
//   - on DECLARE:     add/update the entity in the registry.
//   - on DECLARE_END: mark the chip's declaration complete.
//
// Templated on the registry type to avoid virtual dispatch; radio-free.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace core {

template <typename Registry, std::size_t MaxChips = 16>
class RadioCoordinator {
 public:
  RadioCoordinator(Bus& bus, Registry& reg) noexcept : bus_(bus), reg_(reg) {
    bus_.subscribe(&RadioCoordinator::on_msg, this);
  }

  // True once the chip has completed a DECLARE cycle (DECLARE_END seen) and has
  // not since re-announced with a new HELLO.
  bool chip_ready(std::uint8_t chip_id) const noexcept;

  // True once a HELLO from the chip has been seen.
  bool chip_known(std::uint8_t chip_id) const noexcept;

 private:
  struct ChipState {
    std::uint8_t chip_id;
    std::uint8_t boot_id;
    bool in_use;
    bool ready;
  };

  static void on_msg(void* ctx, const hubproto::Header& h,
                     const std::uint8_t* payload, std::size_t len) noexcept {
    static_cast<RadioCoordinator*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void send_query(std::uint8_t chip_id) noexcept;
  const ChipState* find_chip(std::uint8_t chip_id) const noexcept;
  ChipState* get_or_add(std::uint8_t chip_id) noexcept;

  Bus& bus_;
  Registry& reg_;
  ChipState chips_[MaxChips] = {};
};

// ---- implementation -------------------------------------------------------

template <typename Registry, std::size_t MaxChips>
auto RadioCoordinator<Registry, MaxChips>::find_chip(std::uint8_t chip_id)
    const noexcept -> const ChipState* {
  for (const auto& c : chips_) {
    if (c.in_use && c.chip_id == chip_id) return &c;
  }
  return nullptr;
}

template <typename Registry, std::size_t MaxChips>
auto RadioCoordinator<Registry, MaxChips>::get_or_add(std::uint8_t chip_id) noexcept
    -> ChipState* {
  for (auto& c : chips_) {
    if (c.in_use && c.chip_id == chip_id) return &c;
  }
  for (auto& c : chips_) {
    if (!c.in_use) {
      c.in_use = true;
      c.chip_id = chip_id;
      c.boot_id = 0;
      c.ready = false;
      return &c;
    }
  }
  return nullptr;
}

template <typename Registry, std::size_t MaxChips>
bool RadioCoordinator<Registry, MaxChips>::chip_ready(std::uint8_t chip_id)
    const noexcept {
  const ChipState* c = find_chip(chip_id);
  return c != nullptr && c->ready;
}

template <typename Registry, std::size_t MaxChips>
bool RadioCoordinator<Registry, MaxChips>::chip_known(std::uint8_t chip_id)
    const noexcept {
  return find_chip(chip_id) != nullptr;
}

template <typename Registry, std::size_t MaxChips>
void RadioCoordinator<Registry, MaxChips>::send_query(std::uint8_t chip_id) noexcept {
  hubproto::Header q{};
  q.version = hubproto::kProtocolVersion;
  q.type = hubproto::MsgType::Query;
  q.chip_id = chip_id;
  q.boot_id = 0;
  q.seq = 0;
  q.entity_id = hubproto::kSelfEntityId;
  bus_.publish(q, nullptr, 0);
}

template <typename Registry, std::size_t MaxChips>
void RadioCoordinator<Registry, MaxChips>::handle(const hubproto::Header& h,
                                                  const std::uint8_t* payload,
                                                  std::size_t len) noexcept {
  const std::uint8_t chip = h.chip_id;
  const ChipState* existing = find_chip(chip);

  // An unknown chip (e.g. after a brain reboot), or a chip whose boot_id
  // changed (a reboot, watchdog reset or OTA — SPEC §4.2), invalidates any
  // prior declarations: drop them and re-query. boot_id is the resync primitive,
  // so this triggers on ANY message, not just HELLO.
  if (existing == nullptr || existing->boot_id != h.boot_id) {
    ChipState* c = get_or_add(chip);
    if (c == nullptr) return;  // chip table full
    c->boot_id = h.boot_id;
    c->ready = false;
    reg_.clear_chip(chip);
    send_query(chip);
    return;
  }

  // Known chip, unchanged boot_id.
  switch (h.type) {
    case hubproto::MsgType::Hello: {
      ChipState* c = get_or_add(chip);
      c->ready = false;
      reg_.clear_chip(chip);  // a HELLO always starts a fresh cycle
      send_query(chip);
      break;
    }
    case hubproto::MsgType::Declare: {
      hubproto::DeclareView view{};
      if (hubproto::decode_declare(payload, len, view)) {
        reg_.declare(chip, view);
      }
      break;
    }
    case hubproto::MsgType::DeclareEnd: {
      ChipState* c = get_or_add(chip);
      c->ready = true;
      break;
    }
    default:
      break;  // STATE/COMMAND/CMD_ACK/HEARTBEAT need no coordinator action
  }
}

}  // namespace core
