// What a Matter controller may read about a bridged device — the identity the
// bridge publishes for every entity it mirrors.
//
// The Bridged Device Basic Information cluster (0x0039) is where the controller
// looks for a bridged device's name and for proof that it is the same device it
// saw before. Apple Home needs the second one: a controller that cannot match a
// device treats it as new, drops the old accessory record (with the room and the
// automations in it) and creates a fresh one in the Default Room.
//
// So the bridge publishes:
//   * `UniqueID` — a string derived from the hub's own address of the entity
//     (the chip that owns it + the entity id), which never changes;
//   * `NodeLabel` — the name a person gave the device in the hub;
//   * `ProductName` — what the device is, so the controller's device list reads
//     like a list of devices.
//
// Pure and host-tested: the firmware only puts the strings into the cluster.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/app_protocol.hpp"
#include "hubproto/payload.hpp"

namespace core::matter {

// The maximum length of the Bridged Device Basic Information `NodeLabel`
// (the Matter cluster allows 32 characters).
inline constexpr std::size_t kNodeLabelMax = 32;
// `UniqueID` allows 32 characters too.
inline constexpr std::size_t kUniqueIdMax = 32;
// `ProductName` allows 32 characters.
inline constexpr std::size_t kProductNameMax = 32;

// The stable identity of a bridged entity: "<chip>:<entity>", both in hex —
// "1:0100", "3:0300". It is derived from the hub's own addressing, so it
// survives a bridge restart, a re-pair and a change of the endpoint id.
//
// An entity can mirror to two Matter endpoints (temperature + humidity). The main
// one keeps the old string so an existing accessory is not dropped; the extra one
// gets ":h" appended. `channel` is the core::matter::Channel value; keeping it a
// plain integer here avoids coupling the identity helpers to the endpoint plan.
//
// Returns the number of characters written (not counting the terminating NUL).
inline std::size_t unique_id_for(std::uint8_t chip_id, std::uint16_t entity_id, char* out,
                                 std::size_t cap, std::uint8_t channel = 0) noexcept {
  static const char kHex[] = "0123456789abcdef";
  if (out == nullptr || cap < 9) return 0;  // "f:ffff:h" plus the NUL
  std::size_t w = 0;
  // The chip id: one hex digit (a chip id is 0..0x0A).
  out[w++] = kHex[chip_id & 0x0F];
  out[w++] = ':';
  // The entity id: always four hex digits, so "1:0100" and "3:0300" — the high
  // byte is the owner chip, the low byte the entity number.
  for (int shift = 12; shift >= 0; shift -= 4) {
    out[w++] = kHex[(entity_id >> shift) & 0x0F];
  }
  if (channel != 0) {
    out[w++] = ':';
    out[w++] = 'h';
  }
  out[w] = '\0';
  return w;
}

// The name a person gave the device. The hub's names are up to 31 characters
// (the registry's limit) and the Matter `NodeLabel` allows 32, so it fits — but
// the string has to be cut at a character boundary, and a UTF-8 continuation
// byte must not be left at the end.
//
// Returns the number of characters written (not counting the terminating NUL).
inline std::size_t node_label_for(const char* name, char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap == 0) return 0;
  out[0] = '\0';
  if (name == nullptr || name[0] == '\0') return 0;

  std::size_t len = 0;
  while (len < cap - 1 && name[len] != '\0') ++len;
  // Do not cut in the middle of a UTF-8 sequence. The copy ends at `len`, and
  // the character that ends there is complete only if all of its bytes are
  // inside the copy. Find where that character starts: the last bytes of a
  // multi-byte character are continuation bytes (10xxxxxx), and the byte
  // before them is its lead byte.
  if (len > 0) {
    // Walk back over the continuation bytes to the lead byte of the last
    // character; the character is complete only if its lead byte is inside
    // the copy as well.
    std::size_t last_start = len;
    while (last_start > 0 &&
           (static_cast<unsigned char>(name[last_start - 1]) & 0xC0) == 0x80) {
      --last_start;
    }
    if (last_start == 0) {
      // Only continuation bytes: not a character at all.
      len = 0;
    } else {
      const unsigned char lead = static_cast<unsigned char>(name[last_start - 1]);
      std::size_t width = 1;
      if ((lead & 0xE0) == 0xC0) {
        width = 2;
      } else if ((lead & 0xF0) == 0xE0) {
        width = 3;
      } else if ((lead & 0xF8) == 0xF0) {
        width = 4;
      }
      // The character occupies name[last_start - 1 .. last_start - 1 + width).
      // It is inside the copy only if it ends at or before `len`.
      if (last_start - 1 + width > len) len = last_start - 1;  // cut it whole
    }
  }
  for (std::size_t i = 0; i < len; ++i) out[i] = name[i];
  out[len] = '\0';
  return len;
}

// What the device is called in the controller's device list: the hub's device
// type as a word ("light", "sensor", ...). The hub's type names are the same
// ones the console prints.
inline const char* product_name_for(std::uint8_t device_type) noexcept {
  return core::app::device_name(static_cast<hubproto::DeviceType>(device_type));
}

// The same, but from the *Matter* device type of the endpoint (what the plan
// holds). The hub's own device type is not persisted with the endpoint, and
// the Matter type says just as much to a person reading the controller's
// device list: "on/off light", "dimmable light", "occupancy sensor"...
inline const char* product_name_for_kind(std::uint16_t matter_kind) noexcept {
  switch (matter_kind) {
    case 0x0100: return "on/off light";
    case 0x0101: return "dimmable light";
    case 0x010C: return "color temperature light";
    case 0x010D: return "extended color light";
    case 0x010A: return "plug-in unit";
    case 0x0106: return "light sensor";
    case 0x0107: return "occupancy sensor";
    case 0x0015: return "contact sensor";
    case 0x0301: return "thermostat";
    case 0x0302: return "temperature sensor";
    case 0x0307: return "humidity sensor";
    case 0x000F: return "switch";
    default: return nullptr;
  }
}

}  // namespace core::matter
