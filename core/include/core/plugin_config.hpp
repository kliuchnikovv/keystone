// Generic plugin config storage: the core keeps a plugin's config as an
// OPAQUE versioned blob keyed by plugin id. It never parses the body — if
// it did, the core would grow a schema per integration (design anti-goal B).
// The Tuya plugin defines and parses its own device schema; the hub only
// stores, versions and hands back the bytes.
//
// Portable, header-only, zero radio/ESP-IDF deps. Bounded, all-or-nothing:
// a truncated or wrong-version blob (flash rot) fails cleanly rather than
// yielding garbage — same discipline as scene_table / entity_names.
//
// Layout: [version u8=1][id_len u8][id bytes][body_len u16 LE][body ...]
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"

namespace core::plugin {

inline constexpr uint8_t kConfigVersion = 1;

enum class ConfigError {
  Ok,
  BadVersion,
  Truncated,
  IdMismatch,
  TooLong,
};

// Encode (plugin_id, body) into buf. Returns the number of bytes written,
// or 0 if it does not fit or the inputs exceed the format's bounds.
inline std::size_t encode_plugin_config(const char* plugin_id,
                                        const uint8_t* body,
                                        std::size_t body_len, uint8_t* buf,
                                        std::size_t cap) {
  const std::size_t id_len = std::strlen(plugin_id);
  if (id_len > 0xFF) return 0;
  if (body_len > 0xFFFF) return 0;
  const std::size_t need = 1 + 1 + id_len + 2 + body_len;
  if (need > cap) return 0;

  std::size_t o = 0;
  buf[o++] = kConfigVersion;
  buf[o++] = static_cast<uint8_t>(id_len);
  std::memcpy(buf + o, plugin_id, id_len);
  o += id_len;
  core::detail::put_u16_le(buf + o, static_cast<uint16_t>(body_len));
  o += 2;
  if (body_len != 0) std::memcpy(buf + o, body, body_len);
  o += body_len;
  return o;
}

// Decode a blob and, if it belongs to want_id, hand back a pointer INTO buf
// (no copy) at the body and its length. Fails closed on any short read.
inline ConfigError decode_plugin_config(const uint8_t* buf, std::size_t len,
                                        const char* want_id,
                                        const uint8_t** body_out,
                                        std::size_t* body_len_out) {
  if (body_out != nullptr) *body_out = nullptr;
  if (body_len_out != nullptr) *body_len_out = 0;

  if (len < 2) return ConfigError::Truncated;
  std::size_t o = 0;
  if (buf[o++] != kConfigVersion) return ConfigError::BadVersion;
  const std::size_t id_len = buf[o++];
  if (o + id_len + 2 > len) return ConfigError::Truncated;

  const char* id = reinterpret_cast<const char*>(buf + o);
  o += id_len;
  const std::size_t want_len = std::strlen(want_id);
  if (want_len != id_len || std::memcmp(id, want_id, id_len) != 0) {
    return ConfigError::IdMismatch;
  }

  const std::size_t body_len = core::detail::get_u16_le(buf + o);
  o += 2;
  if (o + body_len > len) return ConfigError::Truncated;

  if (body_out != nullptr) *body_out = buf + o;
  if (body_len_out != nullptr) *body_len_out = body_len;
  return ConfigError::Ok;
}

}  // namespace core::plugin
