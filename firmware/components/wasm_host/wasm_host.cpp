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
#include "wasm_host/test_plugin_abi2_wasm.hpp"
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

// The plugins the image carries, each with its MANIFEST as JSON.
//
// The manifest is the plugin's declaration of what it is and what it wants;
// `core::plugin::parse_manifest` (portable, host-tested) validates it —
// the id's charset and length, the version, the ABI (it must equal the
// hub's), the capability names (an unknown one is refused) and the
// allow-list. The firmware used to hand-build the manifest in code, which
// meant the validation never ran on the real path.
//
// `test.switch` is the good one (a Switch that echoes COMMANDs as STATEs);
// `test.crash` is the same shape but its on_command never returns — the
// watchdog's target (design §10); `test.abi2` reports a FOREIGN ABI version
// and must be refused outright (design §3).
struct BundledPlugin {
  const char* id;       // the name the console knows it by
  const char* manifest; // its declaration (parsed and validated)
  const std::uint8_t* wasm;
  std::size_t len;
};
constexpr BundledPlugin kBundled[] = {
    {"test.switch",
     R"({"id":"test.switch","version":1,"abi":1,)"
     R"("caps":["entity_declare","log","timer"]})",
     kTestPluginWasm, kTestPluginWasmLen},
    {"test.crash",
     R"({"id":"test.crash","version":1,"abi":1,)"
     R"("caps":["entity_declare","log","timer"]})",
     kTestPluginCrashWasm, kTestPluginCrashWasmLen},
    {"test.abi2",
     R"({"id":"test.abi2","version":1,"abi":1,)"
     R"("caps":["entity_declare","log","timer"]})",
     kTestPluginAbi2Wasm, kTestPluginAbi2WasmLen},
    // A manifest the parser must REFUSE: it names a capability the hub does
    // not know ("teleport"). The plugin binary is fine — the point is that
    // the manifest is rejected before the plugin ever loads.
    {"test.badcap",
     R"({"id":"test.badcap","version":1,"abi":1,)"
     R"("caps":["entity_declare","teleport"]})",
     kTestPluginWasm, kTestPluginWasmLen},
    // A manifest that declares an ABI the hub does not speak. The parser
    // refuses it (AbiIncompatible) — the same refusal the binary's own
    // export would earn, but caught earlier, before the plugin is loaded.
    {"test.abi9",
     R"({"id":"test.abi9","version":1,"abi":9,)"
     R"("caps":["entity_declare"]})",
     kTestPluginWasm, kTestPluginWasmLen},
};
constexpr std::size_t kBundledCount = sizeof(kBundled) / sizeof(kBundled[0]);
// A compile-time bound for the per-plugin id buffers (the table is small
// and fixed, but the buffers are indexed by it).
constexpr std::size_t kMaxBundled = 8;

// The manager is a template on the Registry type, so it cannot be a plain
// static here; `g_view` holds a type-erased view of the bits `status()`
// needs.
struct ManagerView {
  bool valid = false;
};
ManagerView g_view;

// Which of the bundled plugins the hub actually holds. `PluginManager::state()`
// answers "Installed" for an id it does NOT know (the core's API is frozen,
// design §11), so the firmware keeps its own list — otherwise a plugin the
// hub refused (a foreign ABI) would look installed in `plugins`.
struct InstalledPlugin {
  char id[core::plugin::Manifest::kMaxId] = {};
  bool in_use = false;
};
constexpr std::size_t kMaxInstalled = 8;
InstalledPlugin g_installed[kMaxInstalled]{};

bool is_installed(const char* id) {
  for (const auto& p : g_installed) {
    if (p.in_use && std::strcmp(p.id, id) == 0) return true;
  }
  return false;
}

void mark_installed(const char* id) {
  for (auto& p : g_installed) {
    if (!p.in_use) {
      std::strncpy(p.id, id, sizeof(p.id) - 1);
      p.in_use = true;
      return;
    }
  }
}

// The id of the plugin the runtime installed, in storage that outlives the
// install frame.
char g_plugin_id[core::plugin::Manifest::kMaxId] = {};

// The manager's type-erased entry points. The manager is a template on the
// Registry type, so these are function pointers filled in by start().
using InstallFn = InstallResult (*)(const char* manifest_json,
                                    const std::uint8_t* wasm, std::size_t len);
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

const char* manifest_error_name(core::plugin::ManifestError e) {
  switch (e) {
    case core::plugin::ManifestError::Ok: return "ok";
    case core::plugin::ManifestError::BadJson: return "malformed JSON";
    case core::plugin::ManifestError::MissingField: return "a required field is missing";
    case core::plugin::ManifestError::AbiIncompatible: return "the ABI is not this hub's";
    case core::plugin::ManifestError::BadCapability: return "an unknown capability";
    case core::plugin::ManifestError::TooLong: return "a field is too long";
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
  g_install = [](const char* manifest_json, const std::uint8_t* wasm,
                 std::size_t len) {
    // The manifest is PARSED, not hand-built: core::plugin::parse_manifest
    // validates the id, the version, the ABI, the capability names and the
    // allow-list, and refuses anything malformed. The firmware used to
    // assemble the manifest in code, which meant that validation never ran
    // on the real path.
    core::plugin::Manifest m{};
    const core::plugin::ManifestError e =
        core::plugin::parse_manifest(manifest_json, std::strlen(manifest_json), m);
    if (e != core::plugin::ManifestError::Ok) {
      ESP_LOGE(kTag, "manifest: %s", manifest_error_name(e));
      return InstallResult::kRefused;
    }
    if (is_installed(m.id)) {
      // The hub already holds it — not an error, just nothing to do.
      ESP_LOGI(kTag, "'%s' is already installed", m.id);
      return InstallResult::kAlreadyThere;
    }
    if (!manager.install(m, wasm, len)) {
      // The manager refused: a bad manifest, an id already taken, no free
      // slot, or a binary the engine would not load (a foreign ABI, a
      // module that does not parse). The engine has already logged why.
      ESP_LOGE(kTag, "install '%s' failed", m.id);
      return InstallResult::kRefused;
    }
    mark_installed(m.id);
    return InstallResult::kOk;
  };
  g_start = [](const char* id) { return manager.start(id); };
  g_state = [](const char* id) { return manager.state(id); };

  // The first bundled plugin: the well-behaved Switch.
  const BundledPlugin& p = kBundled[0];
  const InstallResult ir = g_install(p.manifest, p.wasm, p.len);
  if (ir == InstallResult::kRefused) {
    ESP_LOGE(kTag, "install of the bundled plugin failed");
    return false;
  }
  log_heap("wasm_host::start: after install");

  const char* id = kBundled[0].id;
  const core::plugin::KsErr r = g_start(id);
  if (r != core::plugin::KsErr::Ok) {
    ESP_LOGE(kTag, "start %s: %d", id, (int)r);
    return false;
  }
  log_heap("wasm_host::start: after start()");
  ESP_LOGI(kTag, "plugin '%s' started", id);

  std::strncpy(g_plugin_id, id, sizeof(g_plugin_id) - 1);
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
  // The manager's own view of each plugin the hub HOLDS. A plugin the hub
  // refused (a foreign ABI, a bad manifest) is not listed — the manager's
  // state() answers "Installed" for an id it does not know, so the list has
  // to come from the firmware's own record of what it installed.
  std::size_t off = 0;
  for (std::size_t i = 0; i < kBundledCount && off + 1 < cap; ++i) {
    const char* id = kBundled[i].id;
    if (!is_installed(id)) continue;
    const char* state =
        g_state != nullptr ? state_name(g_state(id)) : "?";
    const int n = std::snprintf(out + off, cap - off, "%s: %s\n", id, state);
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
//
// The outcome is reported in detail, because the three cases are very
// different and the console must not confuse them:
//   - kOk            the plugin is in and running;
//   - kRefused       the manifest or the binary was refused — the plugin
//                    never runs (a foreign ABI, an unknown capability, a
//                    manifest that does not parse);
//   - kAlreadyThere  the hub already holds it — nothing to do;
//   - kNoSuchPlugin  the name is not one of the bundled ones.
InstallResult install_bundled(const char* id) {
  if (g_install == nullptr || g_start == nullptr) return InstallResult::kRefused;
  for (std::size_t i = 0; i < kBundledCount; ++i) {
    const char* pid = kBundled[i].id;
    if (std::strcmp(pid, id) != 0) continue;
    const InstallResult ir =
        g_install(kBundled[i].manifest, kBundled[i].wasm, kBundled[i].len);
    if (ir != InstallResult::kOk) return ir;
    const core::plugin::KsErr r = g_start(pid);
    if (r != core::plugin::KsErr::Ok) {
      ESP_LOGE(kTag, "start %s: %d", id, (int)r);
      return InstallResult::kRefused;
    }
    ESP_LOGI(kTag, "plugin '%s' installed and started", id);
    return InstallResult::kOk;
  }
  ESP_LOGE(kTag, "no bundled plugin named '%s'", id);
  return InstallResult::kNoSuchPlugin;
}

// The names of the plugins the image carries.
std::size_t bundled_count() { return kBundledCount; }
const char* bundled_id(std::size_t i) {
  return i < kBundledCount ? kBundled[i].id : "";
}

std::size_t poll_timers(std::uint32_t now_ms) {
  return g_engine.poll_timers(now_ms, nullptr);
}

}  // namespace wasm_host
