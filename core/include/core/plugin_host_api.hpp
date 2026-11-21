// Plugin host-API contract: the single source of truth for the ABI that
// a WASM plugin and the hub share.
//
// Portable, header-only, zero radio/ESP-IDF deps (SPEC §3). The firmware
// WASM-host component (Plan 1b) implements the imports named by this ABI;
// the core plugin manager and the capability layer speak these same types.
//
// What lives here:
//   - kAbiVersion: the ABI a plugin must declare to be loadable
//   - KsErr: the error code every host call returns
//   - Cap / CapSet: the capability bits a plugin may be granted
#pragma once

#include <cstdint>

namespace core::plugin {

// The ABI version a plugin's manifest must match to be loadable. Bumped on
// any incompatible change to the host-import signatures below.
inline constexpr uint32_t kAbiVersion = 1;

// The result of a host call. Ok is 0 so a plugin can test truthiness.
enum class KsErr : uint8_t {
  Ok = 0,
  Denied,    // the capability was not granted, or the target is out of bounds
  BadArg,    // malformed arguments
  NoMem,     // out of buffer / heap
  NotFound,  // no such entity / socket / key
  Again,     // would block (non-blocking socket)
  Closed,    // socket/resource closed
};

// Capabilities a plugin may declare in its manifest and be granted. Every
// host call checks the relevant bit; a plugin never gets what it did not
// declare. Timer and Log are harmless and granted to all (see capabilities).
enum class Cap : uint16_t {
  NetLocal      = 1u << 0,  // outgoing sockets, local subnet only (v1)
  BusRead       = 1u << 1,  // observe STATE of entities it is allowed to see
  BusCommand    = 1u << 2,  // send COMMAND to entities it is allowed to touch
  EntityDeclare = 1u << 3,  // declare its own virtual entities
  Kv            = 1u << 4,  // its own key-value namespace
  Aes           = 1u << 5,  // host AES helpers
  Json          = 1u << 6,  // host JSON helpers
  Timer         = 1u << 7,  // set timers
  Log           = 1u << 8,  // write to the log
};

// A set of granted capabilities, packed into the Cap bit positions.
struct CapSet {
  uint16_t bits = 0;

  void add(Cap c) {
    bits = static_cast<uint16_t>(bits | static_cast<uint16_t>(c));
  }
  bool has(Cap c) const {
    return (bits & static_cast<uint16_t>(c)) != 0;
  }
};

}  // namespace core::plugin
