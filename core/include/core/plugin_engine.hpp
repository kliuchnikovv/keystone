// The plugin engine boundary: the abstract seam between the portable plugin
// manager (core) and the WASM runtime (firmware, Plan 1b). The manager drives
// an engine; the firmware's WAMR-backed engine realizes it. Tests use a fake.
//
// Same role as Bus/ByteLink elsewhere in core: core owns the policy, the
// firmware owns the machine. Header-only, zero radio/ESP-IDF deps.
//
// Two directions:
//   - manager → engine: load/start/stop/deliver/unload (calls INTO a plugin)
//   - engine → manager: PluginHostCallbacks (a plugin's host imports call OUT)
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/plugin_host_api.hpp"

namespace core::plugin {

// Opaque, engine-owned handle to one loaded plugin. The manager holds it but
// never looks inside — the engine decides what a plugin instance is.
struct PluginInstance;

// What a plugin's host imports call back into — wired by the manager so a
// plugin's declare/state/command land on the registry and bus, and a fault
// is reported. `ctx` is passed back to each callback (the manager's slot).
struct PluginHostCallbacks {
  void* ctx = nullptr;

  // Declare one virtual entity; returns the entity id the hub assigned (0 on
  // failure). The manager tags it with the plugin as owner.
  uint16_t (*declare_entity)(void* ctx, const uint8_t* desc, std::size_t len) = nullptr;

  // Publish STATE for one of the plugin's entities onto the bus.
  KsErr (*publish_state)(void* ctx, uint16_t entity, uint16_t attr,
                         const uint8_t* val, std::size_t len) = nullptr;

  // Send a COMMAND to an entity the plugin is allowed to touch (gated by caps).
  KsErr (*send_command)(void* ctx, uint16_t entity, uint16_t attr,
                        const uint8_t* val, std::size_t len, uint16_t seq) = nullptr;

  // Report that the plugin faulted (trap, OOM, or callback over budget).
  void (*on_fault)(void* ctx, const char* reason) = nullptr;
};

class PluginEngine {
 public:
  virtual ~PluginEngine() = default;

  // Install the callbacks a plugin's host imports call out through. Called
  // once by the manager before any plugin starts.
  virtual void set_host_callbacks(const PluginHostCallbacks& cb) = 0;

  // Load a WASM module; nullptr if it is not a valid/compatible module.
  virtual PluginInstance* load(const uint8_t* wasm, std::size_t len) = 0;

  // Run the plugin's on_start under the callback watchdog. Ok, or an error
  // (and the plugin should be considered faulted) on trap/over-budget.
  virtual KsErr start(PluginInstance* inst) = 0;

  // Stop the plugin (run on_stop); best-effort, no error path.
  virtual void stop(PluginInstance* inst) = 0;

  // Deliver a COMMAND to the plugin (its on_command), under the watchdog.
  virtual KsErr deliver_command(PluginInstance* inst, uint16_t entity,
                                uint16_t attr, const uint8_t* val,
                                std::size_t len, uint16_t seq) = 0;

  // Deliver a STATE the plugin subscribed to (its on_state), under the watchdog.
  virtual KsErr deliver_state(PluginInstance* inst, uint16_t entity,
                              uint16_t attr, const uint8_t* val,
                              std::size_t len) = 0;

  // Free the instance and its module heap.
  virtual void unload(PluginInstance* inst) = 0;
};

}  // namespace core::plugin
