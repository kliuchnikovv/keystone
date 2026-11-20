// Plugin manifest: what a plugin declares about itself before it is loaded —
// its id, version, the ABI it was built against, the capabilities it wants,
// and an optional allow-list of other entities it may touch.
//
// Portable, header-only, zero radio/ESP-IDF deps. Parsed with the project's
// strict bounded JSON reader (core/json.hpp); hostile input is rejected, not
// trusted. A manifest built against another ABI, or naming a capability the
// hub does not know, is refused — the plugin never loads.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/json.hpp"
#include "core/plugin_host_api.hpp"

namespace core::plugin {

struct Manifest {
  static constexpr std::size_t kMaxId = 24;  // includes the NUL terminator
  static constexpr std::size_t kMaxAllow = 8;

  char id[kMaxId] = {};
  uint32_t version = 0;
  uint32_t abi = 0;
  CapSet caps;
  uint16_t allow[kMaxAllow] = {};
  std::size_t allow_n = 0;
};

enum class ManifestError {
  Ok,
  BadJson,
  MissingField,
  AbiIncompatible,
  BadCapability,
  TooLong,
};

// Map a capability name token to its Cap bit. Returns false for an unknown
// name — the one place the manifest's capability vocabulary is defined.
inline bool cap_from_token(const JsonReader& r, int tok, Cap& out) {
  struct Entry {
    const char* name;
    Cap cap;
  };
  static constexpr Entry table[] = {
      {"net_local", Cap::NetLocal},        {"bus_read", Cap::BusRead},
      {"bus_command", Cap::BusCommand},    {"entity_declare", Cap::EntityDeclare},
      {"kv", Cap::Kv},                     {"aes", Cap::Aes},
      {"json", Cap::Json},                 {"timer", Cap::Timer},
      {"log", Cap::Log},
  };
  for (const auto& e : table) {
    if (r.string_equals(tok, e.name)) {
      out = e.cap;
      return true;
    }
  }
  return false;
}

inline ManifestError parse_manifest(const char* json, std::size_t len,
                                    Manifest& out) {
  out = Manifest{};
  JsonReader r;
  if (!r.parse(json, len)) return ManifestError::BadJson;
  const int root = r.root();
  if (!r.is(root, JsonReader::Type::Object)) return ManifestError::BadJson;

  // id — required string, must fit the fixed buffer.
  const int id_tok = r.find(root, "id");
  if (id_tok < 0 || !r.is(id_tok, JsonReader::Type::String)) {
    return ManifestError::MissingField;
  }
  if (!r.get_string(id_tok, out.id, sizeof(out.id))) return ManifestError::TooLong;

  // version — required non-negative number.
  const int ver_tok = r.find(root, "version");
  if (ver_tok < 0) return ManifestError::MissingField;
  std::int64_t ver = 0;
  if (!r.get_i64(ver_tok, ver) || ver < 0) return ManifestError::BadJson;
  out.version = static_cast<uint32_t>(ver);

  // abi — required, must equal the hub's ABI version.
  const int abi_tok = r.find(root, "abi");
  if (abi_tok < 0) return ManifestError::MissingField;
  std::int64_t abi = 0;
  if (!r.get_i64(abi_tok, abi) || abi < 0) return ManifestError::BadJson;
  out.abi = static_cast<uint32_t>(abi);
  if (out.abi != kAbiVersion) return ManifestError::AbiIncompatible;

  // caps — optional array of known capability names.
  const int caps_tok = r.find(root, "caps");
  if (caps_tok >= 0) {
    if (!r.is(caps_tok, JsonReader::Type::Array)) return ManifestError::BadJson;
    const int n = r.count(caps_tok);
    for (int i = 0; i < n; ++i) {
      Cap c{};
      if (!cap_from_token(r, r.at(caps_tok, i), c)) {
        return ManifestError::BadCapability;
      }
      out.caps.add(c);
    }
  }

  // allow — optional array of entity ids (u16), bounded.
  const int allow_tok = r.find(root, "allow");
  if (allow_tok >= 0) {
    if (!r.is(allow_tok, JsonReader::Type::Array)) return ManifestError::BadJson;
    const int n = r.count(allow_tok);
    if (n > static_cast<int>(Manifest::kMaxAllow)) return ManifestError::TooLong;
    for (int i = 0; i < n; ++i) {
      std::int64_t id = 0;
      if (!r.get_i64(r.at(allow_tok, i), id) || id < 0 || id > 0xFFFF) {
        return ManifestError::BadJson;
      }
      out.allow[out.allow_n++] = static_cast<uint16_t>(id);
    }
  }

  return ManifestError::Ok;
}

}  // namespace core::plugin
