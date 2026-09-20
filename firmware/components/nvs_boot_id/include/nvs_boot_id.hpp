// A persistent reboot counter in NVS (SPEC §4.2, Phase 5).
//
// Each call reads the stored boot_id, increments it (wrapping 0xFF -> 1, so the
// value stays in 1..255), persists it, and returns the new value. Call once at
// startup and put the result in every outgoing header's boot_id field; the brain
// uses a changed boot_id as its resync trigger.
#pragma once

#include <cstdint>

namespace fw {

// Returns the incremented, persisted boot_id. Returns 0 only if NVS is
// unavailable (caller may treat 0 as "unknown").
std::uint8_t next_boot_id() noexcept;

}  // namespace fw
