// EntityRegistry: entity_id -> { chip_id, device_type, attributes[], state }.
//
// Populated by DECLARE, cleared per-chip on a boot_id change (SPEC §7, §4.4).
// Fixed-capacity and allocation-free so it runs on the brain MCU; the caps are
// template parameters (SPEC: tens to low hundreds of entities, a few attrs each).
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/entity_names.hpp"
#include "hubproto/payload.hpp"

namespace core {

// One attribute: its declared descriptor plus the latest state value.
struct AttrState {
  std::uint8_t         attr_id;
  hubproto::ValueType  value_type;
  std::uint8_t         flags;
  hubproto::Value      value;      // valid only when has_value
  bool                 has_value;  // false until the first STATE (Phase 4)
};

template <std::size_t MaxAttrs>
struct EntityT {
  std::uint16_t       entity_id;
  std::uint8_t        chip_id;
  hubproto::DeviceType device_type;
  std::uint8_t        attr_count;
  AttrState           attrs[MaxAttrs];
  bool                in_use;
};

template <std::size_t MaxEntities = 64, std::size_t MaxAttrs = 8,
          std::size_t MaxNameLen = 32>
class EntityRegistry {
 public:
  using Entity = EntityT<MaxAttrs>;
  using Names = EntityNames<MaxEntities, MaxNameLen>;

  /** How long a name may be, in bytes (without the terminator). Buffers that hold a
   * name have to be at least this big - the compiler checks that, too. */
  static constexpr std::size_t kMaxNameLen = MaxNameLen;

  // Add or replace an entity from a decoded DECLARE, owned by `chip_id`. Returns
  // false if the entity has more than MaxAttrs attributes or the table is full.
  bool declare(std::uint8_t chip_id, const hubproto::DeclareView& view) noexcept;

  // Number of live entities.
  std::size_t size() const noexcept;

  // Find a live entity by id, or nullptr.
  const Entity* find(std::uint16_t entity_id) const noexcept;

  // Find a live entity by name, or nullptr.
  const Entity* find_by_name(const char* name) const noexcept {
    std::uint16_t id = 0;
    if (!names_.get_id(name, id)) return nullptr;
    return find(id);
  }

  // Resolve a string (name or hex/dec literal) to an entity id.
  bool resolve(const char* str, std::uint16_t& out_id) const noexcept {
    return names_.resolve(str, out_id);
  }

  // Names management
  Names& names() noexcept { return names_; }
  const Names& names() const noexcept { return names_; }

  const char* name_of(std::uint16_t entity_id) const noexcept {
    return names_.get_name(entity_id);
  }

  bool set_name(std::uint16_t entity_id, const char* name) noexcept {
    return names_.set_name(entity_id, name);
  }

  bool clear_name(std::uint16_t entity_id) noexcept {
    return names_.clear_name(entity_id);
  }

  // Enumerate live entities by compacted index in [0, size()): returns the i-th
  // in-use entity, or nullptr if index is out of range. Lets callers (a console,
  // the future Matter bridge) list everything without exposing the storage.
  const Entity* entity_at(std::size_t index) const noexcept;

  // Resolve an attribute's declared value_type (needed to decode STATE/COMMAND).
  bool value_type_of(std::uint16_t entity_id, std::uint8_t attr_id,
                     hubproto::ValueType& out) const noexcept;

  // Store the latest state value of an attribute. Returns false if the entity or
  // attribute is unknown, or the value's type differs from the declared type.
  bool set_value(std::uint16_t entity_id, std::uint8_t attr_id,
                 const hubproto::Value& v) noexcept;

  // Read the latest state value. Returns false if unknown or not yet reported.
  bool get_value(std::uint16_t entity_id, std::uint8_t attr_id,
                 hubproto::Value& out) const noexcept;

  // Remove every entity owned by `chip_id` (boot_id resync). Returns the count.
  // Note: user-assigned entity names are NOT cleared on chip resync.
  std::size_t clear_chip(std::uint8_t chip_id) noexcept;

 private:
  Entity* slot_for(std::uint16_t entity_id) noexcept;  // existing slot, or a free one

  Entity entities_[MaxEntities] = {};
  Names  names_{};
};

// ---- implementation -------------------------------------------------------

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
typename EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::Entity*
EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::slot_for(std::uint16_t entity_id) noexcept {
  for (auto& e : entities_) {
    if (e.in_use && e.entity_id == entity_id) return &e;
  }
  for (auto& e : entities_) {
    if (!e.in_use) return &e;
  }
  return nullptr;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
bool EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::declare(
    std::uint8_t chip_id, const hubproto::DeclareView& view) noexcept {
  if (view.attr_count > MaxAttrs) return false;
  Entity* e = slot_for(view.entity_id);
  if (e == nullptr) return false;

  e->entity_id = view.entity_id;
  e->chip_id = chip_id;
  e->device_type = view.device_type;
  e->attr_count = view.attr_count;
  for (std::uint8_t i = 0; i < view.attr_count; ++i) {
    hubproto::AttrDecl a = hubproto::declare_attr_at(view, i);
    e->attrs[i].attr_id = a.attr_id;
    e->attrs[i].value_type = a.value_type;
    e->attrs[i].flags = a.flags;
    e->attrs[i].has_value = false;
  }
  e->in_use = true;
  return true;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
std::size_t EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::size() const noexcept {
  std::size_t n = 0;
  for (const auto& e : entities_) {
    if (e.in_use) ++n;
  }
  return n;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
auto EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::find(std::uint16_t entity_id)
    const noexcept -> const Entity* {
  for (const auto& e : entities_) {
    if (e.in_use && e.entity_id == entity_id) return &e;
  }
  return nullptr;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
auto EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::entity_at(std::size_t index)
    const noexcept -> const Entity* {
  std::size_t n = 0;
  for (const auto& e : entities_) {
    if (!e.in_use) continue;
    if (n == index) return &e;
    ++n;
  }
  return nullptr;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
bool EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::value_type_of(
    std::uint16_t entity_id, std::uint8_t attr_id,
    hubproto::ValueType& out) const noexcept {
  const Entity* e = find(entity_id);
  if (e == nullptr) return false;
  for (std::uint8_t i = 0; i < e->attr_count; ++i) {
    if (e->attrs[i].attr_id == attr_id) {
      out = e->attrs[i].value_type;
      return true;
    }
  }
  return false;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
bool EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::set_value(
    std::uint16_t entity_id, std::uint8_t attr_id,
    const hubproto::Value& v) noexcept {
  for (auto& e : entities_) {
    if (!e.in_use || e.entity_id != entity_id) continue;
    for (std::uint8_t i = 0; i < e.attr_count; ++i) {
      if (e.attrs[i].attr_id != attr_id) continue;
      if (v.type != e.attrs[i].value_type) return false;  // type safety
      e.attrs[i].value = v;
      e.attrs[i].has_value = true;
      return true;
    }
    return false;  // entity found, attr not
  }
  return false;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
bool EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::get_value(
    std::uint16_t entity_id, std::uint8_t attr_id,
    hubproto::Value& out) const noexcept {
  const Entity* e = find(entity_id);
  if (e == nullptr) return false;
  for (std::uint8_t i = 0; i < e->attr_count; ++i) {
    if (e->attrs[i].attr_id == attr_id) {
      if (!e->attrs[i].has_value) return false;
      out = e->attrs[i].value;
      return true;
    }
  }
  return false;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs, std::size_t MaxNameLen>
std::size_t EntityRegistry<MaxEntities, MaxAttrs, MaxNameLen>::clear_chip(
    std::uint8_t chip_id) noexcept {
  std::size_t removed = 0;
  for (auto& e : entities_) {
    if (e.in_use && e.chip_id == chip_id) {
      e.in_use = false;
      ++removed;
    }
  }
  return removed;
}

}  // namespace core
