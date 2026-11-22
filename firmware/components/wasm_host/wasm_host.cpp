#include "wasm_host/wasm_host.hpp"

#include <cstdio>
#include <cstring>

#include "core/entity_registry.hpp"
#include "core/plugin_capabilities.hpp"
#include "core/plugin_manager.hpp"
#include "core/plugin_manifest.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hubproto/message.hpp"
#include "hubproto/payload.hpp"
#include "wasm_host/test_plugin_crash_wasm.hpp"
#include "wasm_host/test_plugin_wasm.hpp"
#include "wasm_host/wamr_engine.hpp"

namespace wasm_host {

namespace {
constexpr const char* kTag = "wasm-host";

void log_heap(const char* where) {
  const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  ESP_LOGI(kTag, "%s: internal free=%u largest=%u", where, (unsigned)free_int,
           (unsigned)largest);
}

// A real co-processor answers every COMMAND with a CMD_ACK whose seq matches —
// that is how CommandDispatcher learns the command landed. A plugin's chip is
// loopback (ChipRouter fan_out, not a real UART), so nothing on the wire sends
// an ACK. This tiny subscriber fills that gap: for any COMMAND addressed to
// a plugin pseudo-chip (0xC0+), publish a CMD_ACK Ok back into the bus.
//
// Subscribe order matters here: we are added AFTER the manager, so by the
// time this runs the engine has already delivered the command synchronously
// (deliver_command blocks on its executor's completion semaphore). A smarter
// engine would propagate the real status, but for Plan 1b's first wiring Ok
// is the right default: `on_command` returned without a trap.
core::Bus* g_plugin_ack_bus = nullptr;

void plugin_ack_on_msg(void* /*ctx*/, const hubproto::Header& h,
                       const std::uint8_t* /*p*/, std::size_t /*n*/) {
  if (h.type != hubproto::MsgType::Command) return;
  if (h.chip_id < 0xC0) return;
  if (g_plugin_ack_bus == nullptr) return;
  std::uint8_t buf[2];
  auto r = hubproto::encode_cmd_ack(hubproto::CmdAckStatus::Ok, buf, sizeof(buf));
  if (!r.ok) return;
  hubproto::Header ack = h;
  ack.type = hubproto::MsgType::CmdAck;
  g_plugin_ack_bus->publish(ack, buf, r.len);
}

// The facade keeps one engine + one manager for the hub's lifetime. They
// outlive `start()` because the manager stays subscribed to the bus; the
// static holds only the template-parameter-free view of the pair.
WAMRPluginEngine g_engine;

// The plugins the image carries. `test.switch` is the good one (a Switch
// that echoes COMMANDs as STATEs); `test.crash` is the same shape but its
// on_command never returns — the watchdog's target (design §10).
struct BundledPlugin {
  const char* id;
  const std::uint8_t* wasm;
  std::size_t len;
};
constexpr BundledPlugin kBundled[] = {
    {"test.switch", kTestPluginWasm, kTestPluginWasmLen},
    {"test.crash", kTestPluginCrashWasm, kTestPluginCrashWasmLen},
};
constexpr std::size_t kBundledCount = sizeof(kBundled) / sizeof(kBundled[0]);

// The manager is a template on the Registry type, so it cannot be a plain
// static here; `g_view` holds a type-erased view of the bits `status()`
// needs (the plugin's state and its fault count).
struct ManagerView {
  bool valid = false;
};
ManagerView g_view;

// The id of the plugin the runtime installed, in storage that outlives the
// install frame.
char g_plugin_id[core::plugin::Manifest::kMaxId] = {};

// The manager's type-erased entry points. The manager is a template on the
// Registry type, so these are function pointers filled in by start().
using InstallFn = bool (*)(const char* id, const std::uint8_t* wasm, std::size_t len);
using StartFn = core::plugin::KsErr (*)(const char* id);
using StateFn = core::plugin::PluginState (*)(const char* id);
InstallFn g_install = nullptr;
StartFn g_start = nullptr;
StateFn g_state = nullptr;

const char* state_name(core::plugin::PluginState s) {
  switch (s) {
    case core::plugin::PluginState::Installed: return "Installed";
    case core::plugin::PluginState::Started: return "Started";
    case core::plugin::PluginState::Stopped: return "Stopped";
    case core::plugin::PluginState::Faulted: return "Faulted";
  }
  return "?";
}
}  // namespace

template <typename Registry>
bool start(core::Bus& bus, Registry& reg) {
  log_heap("wasm_host::start: before engine");
  if (!g_engine.begin()) {
    ESP_LOGE(kTag, "engine begin failed");
    return false;
  }
  log_heap("wasm_host::start: after engine");

  // One per lifetime; a static so it survives this function's scope. The
  // manager's constructor registers host callbacks and subscribes to bus.
  static core::plugin::PluginManager<Registry> manager(g_engine, bus, reg);
  // ACK responder — must subscribe AFTER the manager so the manager's handle
  // runs first (synchronous command delivery, then we ack Ok).
  g_plugin_ack_bus = &bus;
  bus.subscribe(&plugin_ack_on_msg, nullptr);
  log_heap("wasm_host::start: after manager");

  // The manager's type-erased entry points (see the note on g_install).
  g_install = [](const char* id, const std::uint8_t* wasm, std::size_t len) {
    core::plugin::Manifest m{};
    std::strncpy(m.id, id, sizeof(m.id) - 1);
    m.version = 1;
    m.abi = core::plugin::kAbiVersion;
    m.caps.add(core::plugin::Cap::EntityDeclare);
    m.caps.add(core::plugin::Cap::Log);
    m.caps.add(core::plugin::Cap::Timer);
    return manager.install(m, wasm, len);
  };
  g_start = [](const char* id) { return manager.start(id); };
  g_state = [](const char* id) { return manager.state(id); };

  // Hand-built manifest — Plan 1b will parse one from JSON; for the first
  // wiring the fields come from here. Must declare EntityDeclare to let the
  // plugin call ks_entity_declare at all.
  const BundledPlugin& p = kBundled[0];
  if (!g_install(p.id, p.wasm, p.len)) {
    ESP_LOGE(kTag, "install %s failed", p.id);
    return false;
  }
  log_heap("wasm_host::start: after install");

  const core::plugin::KsErr r = g_start(p.id);
  if (r != core::plugin::KsErr::Ok) {
    ESP_LOGE(kTag, "start %s: %d", p.id, (int)r);
    return false;
  }
  log_heap("wasm_host::start: after start()");
  ESP_LOGI(kTag, "plugin '%s' started", p.id);

  std::strncpy(g_plugin_id, p.id, sizeof(g_plugin_id) - 1);
  g_view.valid = true;
  return true;
}

// Explicit instantiation for the brain's Registry type. The brain's main.cpp
// uses `core::EntityRegistry<>` with the defaults — if that changes, update
// this instantiation to match.
template bool start<core::EntityRegistry<>>(core::Bus&, core::EntityRegistry<>&);

bool status(char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return false;
  if (!g_view.valid) {
    std::snprintf(out, cap, "plugin runtime: not up");
    return false;
  }
  // The manager's own view of each plugin (a plugin may have faulted since
  // it started — the watchdog's whole point).
  std::size_t off = 0;
  for (std::size_t i = 0; i < kBundledCount && off + 1 < cap; ++i) {
    const char* state = g_state != nullptr ? state_name(g_state(kBundled[i].id))
                                           : "?";
    const int n = std::snprintf(out + off, cap - off, "%s: %s\n",
                                kBundled[i].id, state);
    if (n <= 0) break;
    off += static_cast<std::size_t>(n);
  }
  if (off + 1 < cap) {
    std::snprintf(out + off, cap - off,
                  "engine: marshalled host calls=%u | last callback %u ms "
                  "(budget %u ms, instruction cap %d)",
                  (unsigned)g_engine.host_call_count(),
                  (unsigned)g_engine.last_callback_ms(),
                  (unsigned)g_engine.callback_budget_ms(),
                  (int)g_engine.instruction_limit());
  }
  return true;
}

const char* plugin_id() { return g_view.valid ? g_plugin_id : ""; }

// Install and start one of the bundled plugins by name. Used by the console
// `plugin <name>` command to bring up the crash plugin (the watchdog's
// target) next to the good one.
bool install_bundled(const char* id) {
  if (g_install == nullptr || g_start == nullptr) return false;
  for (std::size_t i = 0; i < kBundledCount; ++i) {
    if (std::strcmp(kBundled[i].id, id) != 0) continue;
    if (!g_install(kBundled[i].id, kBundled[i].wasm, kBundled[i].len)) {
      ESP_LOGE(kTag, "install %s failed", id);
      return false;
    }
    const core::plugin::KsErr r = g_start(kBundled[i].id);
    if (r != core::plugin::KsErr::Ok) {
      ESP_LOGE(kTag, "start %s: %d", id, (int)r);
      return false;
    }
    ESP_LOGI(kTag, "plugin '%s' installed and started", id);
    return true;
  }
  ESP_LOGE(kTag, "no bundled plugin named '%s'", id);
  return false;
}

// The names of the plugins the image carries.
std::size_t bundled_count() { return kBundledCount; }
const char* bundled_id(std::size_t i) {
  return i < kBundledCount ? kBundled[i].id : "";
}

}  // namespace wasm_host
