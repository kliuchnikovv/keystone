// CommandDispatcher: sends COMMAND to the chip owning an entity and matches the
// CMD_ACK by seq (SPEC §7, §4.4). Critical commands rely on the ACK: no ACK
// within a timeout triggers retries, then escalation (reported as Unreachable).
//
// Time is injected (now_ms on send/poll) so the core stays clock-free and the
// logic is deterministically testable; firmware feeds esp_timer time.
// Templated on the registry type; radio-free.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "hubproto/version.hpp"

namespace core {

template <typename Registry, std::size_t MaxPending = 8>
class CommandDispatcher {
 public:
  using ResultFn = void (*)(void* ctx, std::uint16_t seq, std::uint16_t entity_id,
                            std::uint8_t attr_id, hubproto::CmdAckStatus status);

  struct Config {
    std::uint32_t timeout_ms = 1000;
    int max_retries = 2;  // resends after the first send
  };

  CommandDispatcher(Bus& bus, Registry& reg, Config cfg = {}) noexcept
      : bus_(bus), reg_(reg), cfg_(cfg) {
    bus_.subscribe(&CommandDispatcher::on_msg, this);
  }

  void on_result(ResultFn fn, void* ctx) noexcept {
    result_fn_ = fn;
    result_ctx_ = ctx;
  }

  // Optional liveness filter (e.g. ChipMonitor::alive_thunk). A command for a
  // chip it reports dead completes at once as Unreachable — no retries into the
  // void — and pending ones fail on the next poll().
  using AliveFn = bool (*)(void* ctx, std::uint8_t chip_id);
  void set_alive_filter(AliveFn fn, void* ctx) noexcept {
    alive_fn_ = fn;
    alive_ctx_ = ctx;
  }

  // Called for every command the hub sends, before it goes out (and even if the
  // chip turns out to be dead) — the hook for "remember what we asked for".
  using SendObserverFn = void (*)(void* ctx, std::uint16_t entity_id,
                                  std::uint8_t attr_id, const hubproto::Value& v);
  void on_send(SendObserverFn fn, void* ctx) noexcept {
    send_obs_ = fn;
    send_obs_ctx_ = ctx;
  }

  // Send a COMMAND for entity_id/attr_id with value `v`. Returns false if the
  // entity is unknown, the attribute is unknown or the value type mismatches, or
  // the pending table is full. On success `seq_out` receives the assigned seq.
  bool send(std::uint16_t entity_id, std::uint8_t attr_id,
            const hubproto::Value& v, std::uint32_t now_ms,
            std::uint16_t& seq_out) noexcept;

  // Advance timeouts: resend timed-out commands up to max_retries, then report
  // Unreachable and drop.
  void poll(std::uint32_t now_ms) noexcept;

  // Number of commands awaiting an ACK.
  std::size_t pending_count() const noexcept;

 private:
  struct Pending {
    bool in_use = false;
    std::uint16_t seq = 0;
    std::uint16_t entity_id = 0;
    std::uint8_t attr_id = 0;
    std::uint8_t chip_id = 0;
    hubproto::Value value{};
    std::uint32_t sent_ms = 0;
    int attempts = 0;  // sends performed so far
  };

  static void on_msg(void* ctx, const hubproto::Header& h,
                     const std::uint8_t* payload, std::size_t len) noexcept {
    static_cast<CommandDispatcher*>(ctx)->handle(h, payload, len);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* payload,
              std::size_t len) noexcept;
  void transmit(Pending& p, std::uint32_t now_ms) noexcept;
  std::uint16_t next_seq() noexcept;

  Bus& bus_;
  Registry& reg_;
  Config cfg_;
  Pending pending_[MaxPending];
  std::uint16_t seq_counter_ = 1;
  ResultFn result_fn_ = nullptr;
  void* result_ctx_ = nullptr;
  AliveFn alive_fn_ = nullptr;
  void* alive_ctx_ = nullptr;
  SendObserverFn send_obs_ = nullptr;
  void* send_obs_ctx_ = nullptr;
};

// ---- implementation -------------------------------------------------------

template <typename Registry, std::size_t MaxPending>
std::uint16_t CommandDispatcher<Registry, MaxPending>::next_seq() noexcept {
  const std::uint16_t s = seq_counter_;
  seq_counter_ = (seq_counter_ == 0xFFFF) ? 1 : static_cast<std::uint16_t>(seq_counter_ + 1);
  return s;
}

template <typename Registry, std::size_t MaxPending>
void CommandDispatcher<Registry, MaxPending>::transmit(Pending& p,
                                                       std::uint32_t now_ms) noexcept {
  hubproto::Command c{p.entity_id, p.attr_id, p.value};
  std::uint8_t payload[3 + 4];
  auto r = hubproto::encode_command(c, payload, sizeof(payload));
  if (!r.ok) return;

  hubproto::Header h{};
  h.version = hubproto::kProtocolVersion;
  h.type = hubproto::MsgType::Command;
  h.chip_id = p.chip_id;
  h.boot_id = 0;
  h.seq = p.seq;
  h.entity_id = p.entity_id;
  bus_.publish(h, payload, r.len);

  p.sent_ms = now_ms;
  ++p.attempts;
}

template <typename Registry, std::size_t MaxPending>
bool CommandDispatcher<Registry, MaxPending>::send(std::uint16_t entity_id,
                                                   std::uint8_t attr_id,
                                                   const hubproto::Value& v,
                                                   std::uint32_t now_ms,
                                                   std::uint16_t& seq_out) noexcept {
  const auto* e = reg_.find(entity_id);
  if (e == nullptr) return false;
  hubproto::ValueType vt{};
  if (!reg_.value_type_of(entity_id, attr_id, vt)) return false;
  if (v.type != vt) return false;  // type safety

  Pending* slot = nullptr;
  for (auto& p : pending_) {
    if (!p.in_use) { slot = &p; break; }
  }
  if (slot == nullptr) return false;

  if (send_obs_ != nullptr) send_obs_(send_obs_ctx_, entity_id, attr_id, v);

  slot->in_use = true;
  slot->seq = next_seq();
  slot->entity_id = entity_id;
  slot->attr_id = attr_id;
  slot->chip_id = e->chip_id;
  slot->value = v;
  slot->attempts = 0;
  const std::uint16_t seq = slot->seq;

  if (alive_fn_ != nullptr && !alive_fn_(alive_ctx_, slot->chip_id)) {
    seq_out = seq;
    if (result_fn_ != nullptr) {
      result_fn_(result_ctx_, seq, entity_id, attr_id, hubproto::CmdAckStatus::Unreachable);
    }
    slot->in_use = false;
    return true;
  }

  transmit(*slot, now_ms);  // an ACK may arrive synchronously and free the slot
  seq_out = seq;
  return true;
}

template <typename Registry, std::size_t MaxPending>
void CommandDispatcher<Registry, MaxPending>::handle(const hubproto::Header& h,
                                                     const std::uint8_t* payload,
                                                     std::size_t len) noexcept {
  if (h.type != hubproto::MsgType::CmdAck) return;
  hubproto::CmdAckStatus status{};
  if (!hubproto::decode_cmd_ack(payload, len, status)) return;

  for (auto& p : pending_) {
    if (p.in_use && p.seq == h.seq) {
      if (result_fn_ != nullptr) {
        result_fn_(result_ctx_, p.seq, p.entity_id, p.attr_id, status);
      }
      p.in_use = false;
      return;
    }
  }
}

template <typename Registry, std::size_t MaxPending>
void CommandDispatcher<Registry, MaxPending>::poll(std::uint32_t now_ms) noexcept {
  for (auto& p : pending_) {
    if (!p.in_use) continue;
    if (alive_fn_ != nullptr && !alive_fn_(alive_ctx_, p.chip_id)) {
      if (result_fn_ != nullptr) {
        result_fn_(result_ctx_, p.seq, p.entity_id, p.attr_id,
                   hubproto::CmdAckStatus::Unreachable);
      }
      p.in_use = false;
      continue;
    }
    if ((now_ms - p.sent_ms) < cfg_.timeout_ms) continue;

    if (p.attempts < 1 + cfg_.max_retries) {
      transmit(p, now_ms);  // retry
    } else {
      if (result_fn_ != nullptr) {
        result_fn_(result_ctx_, p.seq, p.entity_id, p.attr_id,
                   hubproto::CmdAckStatus::Unreachable);
      }
      p.in_use = false;
    }
  }
}

template <typename Registry, std::size_t MaxPending>
std::size_t CommandDispatcher<Registry, MaxPending>::pending_count() const noexcept {
  std::size_t n = 0;
  for (const auto& p : pending_) {
    if (p.in_use) ++n;
  }
  return n;
}

}  // namespace core
