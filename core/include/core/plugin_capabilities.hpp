// Capability enforcement: the host side decides, on every call, whether a
// plugin may do a thing. The plugin is never trusted — it only gets what
// its manifest declared and the hub granted (SPEC §3, design §5.4).
//
// Portable, header-only, zero radio/ESP-IDF deps. The firmware WASM-host
// (Plan 1b) calls these before opening a socket or touching an entity.
//
// v1 network policy: outgoing sockets are allowed to the local subnet only;
// outgoing internet is a separate capability deferred to Project 2.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/plugin_host_api.hpp"

namespace core::plugin {

// The hub's own IPv4 and netmask, host byte order. Supplied at runtime by
// the firmware; the decision itself (below) is pure and host-testable.
struct Subnet {
  uint32_t ip = 0;
  uint32_t mask = 0;
};

// A contiguous netmask is 1s then 0s. ~mask is then 0s then 1s, i.e. one
// less than a power of two, so (inv & (inv+1)) == 0.
inline bool is_contiguous_mask(uint32_t mask) {
  const uint32_t inv = ~mask;
  return (inv & (inv + 1u)) == 0u;
}

inline int prefix_len(uint32_t mask) {
  int n = 0;
  while ((mask & 0x80000000u) != 0u) {
    ++n;
    mask <<= 1;
  }
  return n;
}

// True when target_ip is a routable peer on the hub's own subnet. Fails
// CLOSED: an unset subnet (ip or mask 0), a non-contiguous mask, or a mask
// broader than /8 denies everything. Even inside the subnet it denies the
// hub itself, the network and broadcast addresses, and the special ranges
// (loopback, this-network, multicast, limited broadcast) — so a sandboxed
// plugin cannot turn "local network" into a path to host services (SSRF).
inline bool net_allows(const Subnet& local, uint32_t target_ip) {
  if (local.ip == 0u || local.mask == 0u) return false;
  if (!is_contiguous_mask(local.mask)) return false;
  if (prefix_len(local.mask) < 8) return false;

  if ((target_ip & local.mask) != (local.ip & local.mask)) return false;

  if (target_ip == local.ip) return false;                      // the hub itself
  if (target_ip == (local.ip & local.mask)) return false;       // network address
  if (target_ip == (local.ip | ~local.mask)) return false;      // broadcast
  if ((target_ip & 0xFF000000u) == 0x7F000000u) return false;   // 127.0.0.0/8 loopback
  if ((target_ip & 0xFF000000u) == 0x00000000u) return false;   // 0.0.0.0/8
  if ((target_ip & 0xF0000000u) == 0xE0000000u) return false;   // 224.0.0.0/4 multicast
  if (target_ip == 0xFFFFFFFFu) return false;                   // limited broadcast
  return true;
}

// Gate an outgoing socket: Denied without the NetLocal capability, Denied
// for a non-local target even with it, Ok only for a local target with it.
inline KsErr check_net(const CapSet& caps, const Subnet& local,
                       uint32_t target_ip) {
  if (!caps.has(Cap::NetLocal)) return KsErr::Denied;
  return net_allows(local, target_ip) ? KsErr::Ok : KsErr::Denied;
}

// What entities a plugin may read/command. A plugin may always touch the
// entities it declared itself; any other entity must be on the allow-list
// (v1: empty, so a plugin sees only its own — cross-entity access is a
// later capability).
struct EntityAccess {
  bool own_only = true;  // reserved for a future "own + explicit list" split
  const uint16_t* allow = nullptr;
  size_t allow_n = 0;
};

inline bool entity_allowed(const EntityAccess& acc, uint16_t entity_id,
                           bool is_owned_by_plugin) {
  if (is_owned_by_plugin) return true;
  for (size_t i = 0; i < acc.allow_n; ++i) {
    if (acc.allow[i] == entity_id) return true;
  }
  return false;
}

}  // namespace core::plugin
