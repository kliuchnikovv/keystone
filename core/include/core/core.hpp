// Brain core — portable, radio-free (SPEC §7, Invariants 3 & 5).
//
// Phase 0 placeholder. The real components (Bus, EntityRegistry, StateMachine,
// RuleEngine, CommandDispatcher) are added in Phases 3-5.
#pragma once

namespace core {

// Returns true once the core module is linkable and initialised. Exists only so
// Phase 0 has something to compile, link, and unit-test end to end.
bool ready() noexcept;

}  // namespace core
