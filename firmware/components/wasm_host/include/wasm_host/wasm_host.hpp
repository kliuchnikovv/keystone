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

}  // namespace wasm_host
