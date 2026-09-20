// EntityRegistry: entity_id -> { chip_id, device_type, attributes[], state }.
//
// Populated by DECLARE, cleared per-chip on a boot_id change (SPEC §7, §4.4).
// Fixed-capacity and allocation-free so it runs on the brain MCU; the caps are
// template parameters (SPEC: tens to low hundreds of entities, a few attrs each).
#pragma once

#include <cstddef>
#include <cstdint>

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

template <std::size_t MaxEntities = 64, std::size_t MaxAttrs = 8>
class EntityRegistry {
 public:
  using Entity = EntityT<MaxAttrs>;

  // Add or replace an entity from a decoded DECLARE, owned by `chip_id`. Returns
  // false if the entity has more than MaxAttrs attributes or the table is full.
  bool declare(std::uint8_t chip_id, const hubproto::DeclareView& view) noexcept;

  // Number of live entities.
  std::size_t size() const noexcept;

  // Find a live entity by id, or nullptr.
  const Entity* find(std::uint16_t entity_id) const noexcept;

  // Resolve an attribute's declared value_type (needed to decode STATE/COMMAND).
  bool value_type_of(std::uint16_t entity_id, std::uint8_t attr_id,
                     hubproto::ValueType& out) const noexcept;

  // Remove every entity owned by `chip_id` (boot_id resync). Returns the count.
  std::size_t clear_chip(std::uint8_t chip_id) noexcept;

 private:
  Entity* slot_for(std::uint16_t entity_id) noexcept;  // existing slot, or a free one

  Entity entities_[MaxEntities] = {};
};

// ---- implementation -------------------------------------------------------

template <std::size_t MaxEntities, std::size_t MaxAttrs>
typename EntityRegistry<MaxEntities, MaxAttrs>::Entity*
EntityRegistry<MaxEntities, MaxAttrs>::slot_for(std::uint16_t entity_id) noexcept {
  for (auto& e : entities_) {
    if (e.in_use && e.entity_id == entity_id) return &e;
  }
  for (auto& e : entities_) {
    if (!e.in_use) return &e;
  }
  return nullptr;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs>
bool EntityRegistry<MaxEntities, MaxAttrs>::declare(
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

template <std::size_t MaxEntities, std::size_t MaxAttrs>
std::size_t EntityRegistry<MaxEntities, MaxAttrs>::size() const noexcept {
  std::size_t n = 0;
  for (const auto& e : entities_) {
    if (e.in_use) ++n;
  }
  return n;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs>
auto EntityRegistry<MaxEntities, MaxAttrs>::find(std::uint16_t entity_id)
    const noexcept -> const Entity* {
  for (const auto& e : entities_) {
    if (e.in_use && e.entity_id == entity_id) return &e;
  }
  return nullptr;
}

template <std::size_t MaxEntities, std::size_t MaxAttrs>
bool EntityRegistry<MaxEntities, MaxAttrs>::value_type_of(
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

template <std::size_t MaxEntities, std::size_t MaxAttrs>
std::size_t EntityRegistry<MaxEntities, MaxAttrs>::clear_chip(
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
