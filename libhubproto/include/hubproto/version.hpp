// Version stamps for the inter-chip protocol library.
//
// kProtocolVersion is the on-the-wire `version` byte (SPEC §4.2, header offset
// 0). It starts at 0x01 and is bumped only on an incompatible wire change.
// kLibraryVersion tracks the source library and is independent of the wire.
#pragma once

#include <cstdint>

namespace hubproto {

// On-the-wire protocol version (header byte 0). SPEC §4.2.
inline constexpr std::uint8_t kProtocolVersion = 0x01;

// Human-readable version of this source library (semver of the repo module).
const char* library_version() noexcept;

}  // namespace hubproto
