// Top-level facade: bring up the plugin runtime for the brain.
//
// Hides the engine type (WAMR) and the manager type (template instantiated
// with the brain's Registry) behind a plain C++ function. One call does
// everything: engine up, manager wired to Bus+Registry, the bundled test
// plugin installed and started.
//
// The test plugin is a Switch(on_off) that echoes COMMANDs back as STATEs.
// The entity it declares appears in the brain's registry under plugin
// pseudo-chip 0xC0 + slot, with entity id starting at 0xE000.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/bus.hpp"
#include "core/entity_registry.hpp"

namespace wasm_host {

// Start the plugin runtime: WAMR engine, PluginManager, one bundled plugin.
// Called once from app_main AFTER the hub's bus/registry are up. Returns
// false if any step fails; the hub keeps running without plugins.
//
// Takes the brain's actual Registry type as a template arg so the manager's
// Registry template matches the one `reg` was declared with.
template <typename Registry>
bool start(core::Bus& bus, Registry& reg);

// A one-line summary of the plugin runtime, for the console `plugins`
// command: the manager's view of the plugin (state, fault count) and the
// engine's counters (marshalled host calls, last callback duration, the
// callback budget). Returns false if the runtime is not up.
bool status(char* out, std::size_t cap);

// The id of the plugin the runtime installed (e.g. "test.switch"). Empty if
// the runtime is not up.
const char* plugin_id();

// Install and start one of the plugins the image carries, by name. The
// console `plugin <name>` command uses this to bring up the crash plugin
// (the watchdog's target) next to the good one.
bool install_bundled(const char* id);

// The names of the plugins the image carries.
std::size_t bundled_count();
const char* bundled_id(std::size_t i);

// Deliver the plugin timers that are due. Called from the hub's main loop —
// the hub's clock is the main loop's, and the plugin's on_timer(id) is
// delivered under the same callback budget as every other plugin callback.
// Returns how many timers fired.
std::size_t poll_timers(std::uint32_t now_ms);

}  // namespace wasm_host
