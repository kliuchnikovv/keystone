// PluginManager: the plugin lifecycle and the glue between a plugin and the
// hub's bus/registry. A plugin is just another participant on the bus (design
// §5.2): it declares virtual entities, publishes their STATE, and receives
// COMMANDs addressed to them — so rules, scenes, history and the app work over
// plugin devices unchanged.
//
// Portable, header-only, zero radio/ESP-IDF deps. The manager drives an
// abstract PluginEngine (WAMR in firmware, a fake in tests) and talks to the
// Bus and EntityRegistry. It never trusts a plugin: every entity it declares
// is owned by a plugin pseudo-chip, and every host call is capability-checked.
//
// Threading note (Plan 1b): host callbacks arrive from the plugin task; the
// firmware marshals them onto the main loop before they reach this manager, so
// the single-threaded core bus/registry stay single-threaded (design §5.5).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/bus.hpp"
#include "core/plugin_capabilities.hpp"
#include "core/plugin_engine.hpp"
#include "core/plugin_host_api.hpp"
#include "core/plugin_manifest.hpp"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"

namespace core::plugin {

enum class PluginState { Installed, Started, Stopped, Faulted };

template <typename Registry, std::size_t MaxPlugins = 4,
          std::size_t MaxEntitiesPerPlugin = 16>
class PluginManager {
 public:
  // Plugin-owned entities live above every chip id and every real entity id,
  // in their own ranges, so they can never collide with Zigbee/Matter/phones.
  static constexpr std::uint8_t kChipBase = 0xC0;
  static constexpr std::uint16_t kEntityBase = 0xE000;
  static constexpr std::size_t kMaxAttrs = 8;
  // Lifetime faults before a plugin is quarantined (no restart until it is
  // reinstalled). Counted over the plugin's life and NOT reset by a lucky
  // start, so a fault/recover/fault loop still reaches the gate. (A windowed
  // count like ChipMonitor needs an injected clock; the core has none, so v1
  // uses a lifetime count — see the design doc.)
  static constexpr int kQuarantineThreshold = 3;

  PluginManager(PluginEngine& engine, Bus& bus, Registry& reg) noexcept
      : engine_(engine), bus_(bus), reg_(reg) {
    PluginHostCallbacks cb{};
    cb.ctx = this;
    cb.declare_entity = &PluginManager::cb_declare;
    cb.publish_state = &PluginManager::cb_publish_state;
    cb.send_command = &PluginManager::cb_send_command;
    cb.on_fault = &PluginManager::cb_on_fault;
    engine_.set_host_callbacks(cb);
    bus_.subscribe(&PluginManager::on_msg, this);
  }

  // Load a plugin into a free slot. False if already installed, no slot, or the
  // module does not load.
  bool install(const Manifest& m, const std::uint8_t* wasm,
               std::size_t len) noexcept {
    if (find(m.id) != nullptr) return false;
    Slot* s = free_slot();
    if (s == nullptr) return false;
    PluginInstance* inst = engine_.load(wasm, len);
    if (inst == nullptr) return false;

    *s = Slot{};
    s->in_use = true;
    std::strncpy(s->id, m.id, sizeof(s->id) - 1);
    s->state = PluginState::Installed;
    s->inst = inst;
    s->caps = m.caps;
    s->chip_id = static_cast<std::uint8_t>(kChipBase + index_of(s));
    // allow_n is clamped even though parse_manifest bounds it — install is a
    // public entry point and a hand-built Manifest must not read past the array.
    const std::size_t an =
        m.allow_n < Manifest::kMaxAllow ? m.allow_n : Manifest::kMaxAllow;
    for (std::size_t i = 0; i < an; ++i) s->allow_store[i] = m.allow[i];
    s->access.allow = s->allow_store;
    s->access.allow_n = an;
    return true;
  }

  KsErr start(const char* id) noexcept {
    Slot* s = find(id);
    if (s == nullptr) return KsErr::NotFound;
    if (s->fault_count >= kQuarantineThreshold) return KsErr::Denied;  // quarantined
    if (s->state == PluginState::Started) return KsErr::Denied;        // already running
    active_ = s;
    call_faulted_ = false;
    const KsErr r = engine_.start(s->inst);
    active_ = nullptr;
    if (call_faulted_) {
      return KsErr::NoMem;  // faulted during on_start; already Faulted & counted
    }
    if (r == KsErr::Ok) {
      s->state = PluginState::Started;  // clean start (recovers a prior Faulted)
    } else {
      fault(s);  // engine error that on_fault did not already report
    }
    return r;
  }

  void stop(const char* id) noexcept {
    Slot* s = find(id);
    if (s == nullptr) return;
    engine_.stop(s->inst);
    // A stopped plugin leaves no ghost devices behind: withdraw its entities.
    for (std::size_t i = 0; i < s->entity_n; ++i) reg_.undeclare(s->entities[i]);
    s->entity_n = 0;
    // A quarantine is not lifted by stopping — that stays Faulted until the
    // plugin is reinstalled. Only a running/installed plugin becomes Stopped.
    if (s->state != PluginState::Faulted) s->state = PluginState::Stopped;
  }

  PluginState state(const char* id) const noexcept {
    const Slot* s = find(id);
    return s != nullptr ? s->state : PluginState::Installed;
  }

  // Fault a plugin by id from outside an engine call (e.g. a firmware watchdog
  // that caught a callback over its time budget).
  void on_plugin_fault(const char* id, const char* reason) noexcept {
    (void)reason;
    Slot* s = find(id);
    if (s != nullptr) fault(s);
  }

 private:
  struct Slot {
    bool in_use = false;
    char id[Manifest::kMaxId] = {};
    PluginState state = PluginState::Installed;
    PluginInstance* inst = nullptr;
    std::uint8_t chip_id = 0;
    CapSet caps;
    std::uint16_t allow_store[Manifest::kMaxAllow] = {};
    EntityAccess access;
    std::uint16_t entities[MaxEntitiesPerPlugin] = {};
    std::size_t entity_n = 0;
    int fault_count = 0;
  };

  // ---- host callbacks (a plugin's imports call OUT through these) ----------

  static std::uint16_t cb_declare(void* ctx, const std::uint8_t* desc,
                                  std::size_t len) {
    return static_cast<PluginManager*>(ctx)->declare(desc, len);
  }
  static KsErr cb_publish_state(void* ctx, std::uint16_t e, std::uint16_t a,
                                const std::uint8_t* v, std::size_t n) {
    return static_cast<PluginManager*>(ctx)->publish_state(e, a, v, n);
  }
  static KsErr cb_send_command(void* ctx, std::uint16_t e, std::uint16_t a,
                               const std::uint8_t* v, std::size_t n,
                               std::uint16_t seq) {
    return static_cast<PluginManager*>(ctx)->send_command(e, a, v, n, seq);
  }
  static void cb_on_fault(void* ctx, const char* reason) {
    static_cast<PluginManager*>(ctx)->on_fault(reason);
  }

  std::uint16_t declare(const std::uint8_t* desc, std::size_t len) noexcept {
    if (active_ == nullptr) return 0;
    if (active_->state == PluginState::Faulted) return 0;  // no acting after a fault
    if (!active_->caps.has(Cap::EntityDeclare)) return 0;
    if (active_->entity_n >= MaxEntitiesPerPlugin) return 0;

    hubproto::DeclareView view{};
    if (!hubproto::decode_declare(desc, len, view)) return 0;
    if (view.attr_count > kMaxAttrs) return 0;

    // Pick a free id WITHOUT committing, so a failed declare does not burn an id
    // (which would let a plugin exhaust the shared plugin id space).
    std::uint16_t cand = next_entity_id_;
    while (cand < 0xFFFE && reg_.find(cand) != nullptr) ++cand;
    if (cand >= 0xFFFE) return 0;

    hubproto::AttrDecl attrs[kMaxAttrs];
    for (std::uint8_t i = 0; i < view.attr_count; ++i) {
      attrs[i] = hubproto::declare_attr_at(view, i);
    }
    if (!reg_.declare_virtual(active_->chip_id, cand, view.device_type, attrs,
                              view.attr_count)) {
      return 0;  // id not committed; next_entity_id_ unchanged
    }
    next_entity_id_ = static_cast<std::uint16_t>(cand + 1);  // commit on success
    active_->entities[active_->entity_n++] = cand;
    return cand;
  }

  KsErr publish_state(std::uint16_t entity, std::uint16_t attr,
                      const std::uint8_t* v, std::size_t n) noexcept {
    if (active_ == nullptr) return KsErr::Denied;
    if (active_->state == PluginState::Faulted) return KsErr::Denied;
    if (attr > 0xFF) return KsErr::BadArg;  // attr id is a u8; 0x0170 must not alias 0x70
    if (!owns(active_, entity)) return KsErr::Denied;  // only its own entities
    hubproto::ValueType vt{};
    if (!reg_.value_type_of(entity, static_cast<std::uint8_t>(attr), vt)) {
      return KsErr::NotFound;
    }
    hubproto::Value val{};
    if (!hubproto::read_value(v, n, vt, val)) return KsErr::BadArg;
    if (!reg_.set_value(entity, static_cast<std::uint8_t>(attr), val)) {
      return KsErr::BadArg;
    }
    return KsErr::Ok;
  }

  KsErr send_command(std::uint16_t entity, std::uint16_t attr,
                     const std::uint8_t* v, std::size_t n,
                     std::uint16_t seq) noexcept {
    (void)entity;
    (void)attr;
    (void)v;
    (void)n;
    (void)seq;
    // Inert in v1. A plugin commanding OTHER hub entities (acting as a
    // controller) must go through CommandDispatcher — retries, CMD_ACK matching,
    // the alive filter, desired-state recording and source tagging — not a raw
    // bus.publish that forges a header and bypasses all of it, and never a path
    // to a chip self-entity's pair/unpair meta. Routing it safely is deferred;
    // until then a plugin acts only on its own device (through its sockets) and
    // its own entities. See the design doc.
    return KsErr::Denied;
  }

  void on_fault(const char* reason) noexcept {
    (void)reason;
    if (active_ != nullptr) fault(active_);
  }

  void fault(Slot* s) noexcept {
    if (s == active_) call_faulted_ = true;  // a fault happened in this engine call
    for (std::size_t i = 0; i < s->entity_n; ++i) reg_.undeclare(s->entities[i]);
    s->entity_n = 0;
    s->state = PluginState::Faulted;
    ++s->fault_count;
  }

  // ---- bus inbound (COMMANDs addressed to plugin entities) -----------------

  static void on_msg(void* ctx, const hubproto::Header& h,
                     const std::uint8_t* p, std::size_t n) {
    static_cast<PluginManager*>(ctx)->handle(h, p, n);
  }

  void handle(const hubproto::Header& h, const std::uint8_t* p,
              std::size_t n) noexcept {
    if (h.type != hubproto::MsgType::Command) return;
    Slot* s = owner_of(h.entity_id);
    if (s == nullptr || s->state != PluginState::Started) return;
    if (n < 3) return;  // entity(u16) attr(u8) ...
    const std::uint8_t attr = p[2];
    const std::uint8_t* val = p + 3;
    const std::size_t vlen = n - 3;
    // Save/restore the active slot so a synchronous, re-entrant delivery cannot
    // clobber an outer callback's context.
    Slot* const prev = active_;
    const bool prev_faulted = call_faulted_;
    active_ = s;
    call_faulted_ = false;
    const KsErr r = engine_.deliver_command(s->inst, h.entity_id, attr, val,
                                            vlen, h.seq);
    if (!call_faulted_ && r != KsErr::Ok) {
      fault(s);  // a handler error is a fault too (spec §5.3), counted once
    }
    active_ = prev;
    call_faulted_ = prev_faulted;
  }

  // ---- helpers -------------------------------------------------------------

  Slot* free_slot() noexcept {
    for (auto& s : slots_)
      if (!s.in_use) return &s;
    return nullptr;
  }
  std::size_t index_of(const Slot* s) const noexcept {
    return static_cast<std::size_t>(s - slots_);
  }
  Slot* find(const char* id) noexcept {
    for (auto& s : slots_)
      if (s.in_use && std::strcmp(s.id, id) == 0) return &s;
    return nullptr;
  }
  const Slot* find(const char* id) const noexcept {
    for (const auto& s : slots_)
      if (s.in_use && std::strcmp(s.id, id) == 0) return &s;
    return nullptr;
  }
  static bool owns(const Slot* s, std::uint16_t entity) noexcept {
    for (std::size_t i = 0; i < s->entity_n; ++i)
      if (s->entities[i] == entity) return true;
    return false;
  }
  Slot* owner_of(std::uint16_t entity) noexcept {
    for (auto& s : slots_)
      if (s.in_use && owns(&s, entity)) return &s;
    return nullptr;
  }
  // Entity ids are handed out MONOTONICALLY from next_entity_id_ (see declare):
  // ids freed by a fault are never reused (tombstoned), so a stale rule/app
  // reference to a removed plugin entity can never resolve to a different
  // device later.

  PluginEngine& engine_;
  Bus& bus_;
  Registry& reg_;
  Slot slots_[MaxPlugins];
  Slot* active_ = nullptr;
  bool call_faulted_ = false;  // a fault occurred inside the current engine call
  std::uint16_t next_entity_id_ = kEntityBase;  // monotonic, never rewinds
};

}  // namespace core::plugin
