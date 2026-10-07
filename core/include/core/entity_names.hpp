// EntityNames: entity_id <-> human-readable name mapping.
//
// Allocation-free, fixed capacity. Used by the brain console and higher-level
// interfaces to refer to entities by name (e.g. "livingroom_plug" instead of
// "0x0100").
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/endian.hpp"

namespace core {

template <std::size_t MaxEntities = 64, std::size_t MaxNameLen = 24>
class EntityNames {
 public:
  static constexpr std::uint16_t kMagic = 0x4E4D;  // 'NM'
  static constexpr std::uint8_t  kVersion = 1;
  static constexpr std::size_t   kHeaderSize = 6;
  static constexpr std::size_t   kEntryWireSize = 2 + MaxNameLen;

  struct Entry {
    std::uint16_t id;
    const char*   name;
  };

  // Associate `name` with `id`.
  // Fails if:
  // - `name` is null or empty
  // - `name` length >= MaxNameLen
  // - another entity already has this name (names are unique)
  // - table is full and `id` is not already present
  bool set_name(std::uint16_t id, const char* name) noexcept {
    if (!name || name[0] == '\0') return false;
    std::size_t len = 0;
    while (name[len] != '\0') {
      if (++len >= MaxNameLen) return false;
    }

    // Check if name is already used by a DIFFERENT entity.
    for (std::size_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].name, name) == 0) {
        if (entries_[i].id != id) return false;
        return true;  // already set to this name
      }
    }

    // Check if id already exists -> update name
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].id == id) {
        std::memcpy(entries_[i].name, name, len + 1);
        return true;
      }
    }

    // New entry
    if (count_ >= MaxEntities) return false;
    entries_[count_].id = id;
    std::memcpy(entries_[count_].name, name, len + 1);
    ++count_;
    return true;
  }

  // Clear name for `id`. Returns true if it was found and removed.
  bool clear_name(std::uint16_t id) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].id == id) {
        entries_[i] = entries_[count_ - 1];
        --count_;
        return true;
      }
    }
    return false;
  }

  // Clear all names.
  void clear() noexcept {
    count_ = 0;
  }

  // Number of named entities.
  std::size_t size() const noexcept { return count_; }

  // Get name by entity id, or nullptr if unnamed.
  const char* get_name(std::uint16_t id) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].id == id) return entries_[i].name;
    }
    return nullptr;
  }

  // Find entity id by name. Returns true on success and populates `out_id`.
  bool get_id(const char* name, std::uint16_t& out_id) const noexcept {
    if (!name || name[0] == '\0') return false;
    for (std::size_t i = 0; i < count_; ++i) {
      if (std::strcmp(entries_[i].name, name) == 0) {
        out_id = entries_[i].id;
        return true;
      }
    }
    return false;
  }

  // Find entity id by name. Returns 0 if not found.
  std::uint16_t find_id(const char* name) const noexcept {
    std::uint16_t id = 0;
    if (get_id(name, id)) return id;
    return 0;
  }

  // Resolve a string (either an entity name or a numeric/hex literal) to an id.
  // Returns true on success and populates `out_id`.
  bool resolve(const char* str, std::uint16_t& out_id) const noexcept {
    if (!str || str[0] == '\0') return false;

    // 1. Try matching by name first
    if (get_id(str, out_id)) return true;

    // 2. Try parsing as integer (hex e.g. 0x0100 or dec e.g. 256)
    char* end = nullptr;
    unsigned long val = std::strtoul(str, &end, 0);
    if (end != str && *end == '\0' && val <= 0xFFFF) {
      out_id = static_cast<std::uint16_t>(val);
      return true;
    }

    return false;
  }

  // Enumerate entry at index [0, size()).
  bool entry_at(std::size_t index, Entry& out) const noexcept {
    if (index >= count_) return false;
    out.id = entries_[index].id;
    out.name = entries_[index].name;
    return true;
  }

  // Total byte size needed to serialize current names.
  std::size_t serialized_size() const noexcept {
    return kHeaderSize + count_ * kEntryWireSize;
  }

  // Serialize names table to buffer. Returns bytes written, or 0 if capacity too small.
  std::size_t save(void* out_buf, std::size_t out_cap) const noexcept {
    if (!out_buf) return 0;
    const std::size_t required = serialized_size();
    if (out_cap < required) return 0;

    auto* p = static_cast<std::uint8_t*>(out_buf);
    detail::put_u16_le(p + 0, kMagic);
    p[2] = kVersion;
    p[3] = 0;  // reserved
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(count_));

    std::size_t off = kHeaderSize;
    for (std::size_t i = 0; i < count_; ++i) {
      detail::put_u16_le(p + off, entries_[i].id);
      off += 2;
      std::memset(p + off, 0, MaxNameLen);
      std::strncpy(reinterpret_cast<char*>(p + off), entries_[i].name, MaxNameLen - 1);
      off += MaxNameLen;
    }
    return required;
  }

  // Load names from serialized buffer. Clears current names and loads from buffer.
  // Returns true on success, false if format is invalid or corrupted.
  bool load(const void* in_buf, std::size_t in_len) noexcept {
    if (!in_buf || in_len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in_buf);

    if (detail::get_u16_le(p + 0) != kMagic) return false;
    if (p[2] != kVersion) return false;

    const std::uint16_t num_entries = detail::get_u16_le(p + 4);
    const std::size_t expected_len =
        kHeaderSize + static_cast<std::size_t>(num_entries) * kEntryWireSize;
    if (in_len < expected_len) return false;
    if (num_entries > MaxEntities) return false;

    clear();
    std::size_t off = kHeaderSize;
    for (std::size_t i = 0; i < num_entries; ++i) {
      const std::uint16_t id = detail::get_u16_le(p + off);
      off += 2;
      char name_buf[MaxNameLen + 1] = {};
      std::memcpy(name_buf, p + off, MaxNameLen);
      name_buf[MaxNameLen] = '\0';  // guarantee null termination
      off += MaxNameLen;

      if (!set_name(id, name_buf)) {
        return false;
      }
    }
    return true;
  }

 private:
  struct StorageEntry {
    std::uint16_t id;
    char          name[MaxNameLen];
  };

  StorageEntry entries_[MaxEntities] = {};
  std::size_t  count_ = 0;
};

}  // namespace core
