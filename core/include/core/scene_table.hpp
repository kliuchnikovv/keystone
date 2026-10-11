// SceneTable: fixed-capacity, allocation-free table of named scenes. A scene is a list of
// "set this attribute of this entity to this value" actions that are applied together
// ("evening": lamp on, plug off, brightness 40 %).
//
// The table stores and persists; it does not know the registry or the dispatcher.
// `apply` calls a send function per action and counts what it accepted, so the caller
// decides what a valid target is (an entity that no longer exists is skipped, not an error
// of the scene).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"
#include "hubproto/payload.hpp"

namespace core {

// Scene names: a lower-case letter, then lower-case letters, digits or '_' (at most 23),
// the same alphabet as entity names, so a name never looks like a number.
inline bool valid_scene_name(const char* s) noexcept {
  if (s == nullptr) return false;
  std::size_t n = 0;
  for (; s[n] != '\0'; ++n) {
    const char c = s[n];
    const bool ok = (c >= 'a' && c <= 'z') || (n > 0 && ((c >= '0' && c <= '9') || c == '_'));
    if (!ok || n >= 23) return false;
  }
  return n > 0;
}

// A schedule, timer or rule that should apply a scene stores an ordinary (entity, attr, value)
// action whose entity is this reserved id and whose attr is the scene's stable id. Real
// entities are 0x0100.. and chips 1..15, so it never collides.
inline constexpr std::uint16_t kSceneTarget = 0xFFFE;

template <std::size_t MaxScenes = 8, std::size_t MaxActions = 8>
class SceneTable {
 public:
  static constexpr std::uint16_t kMagic = 0x5344;  // 'SD'
  static constexpr std::uint8_t kVersion = 2;  // 2: every scene has a stable id
  static constexpr std::size_t kHeaderSize = 6;
  static constexpr std::size_t kNameWire = 24;       // NUL-padded
  static constexpr std::size_t kActionWire = 9;      // entity u16, attr u8, value (type u8, len u8, raw[4])
  static constexpr std::size_t kNameCap = 24;
  static constexpr std::size_t kMaxActions = MaxActions;
  static constexpr std::size_t kMaxScenes = MaxScenes;

  struct Action {
    std::uint16_t entity_id = 0;
    std::uint8_t attr_id = 0;
    hubproto::Value value{};
  };

  struct Scene {
    bool active = false;
    char name[kNameCap] = {};
    std::uint8_t id = 0;  // stable while the scene exists (1..255); what schedules and rules refer to
    std::uint8_t count = 0;
    Action actions[MaxActions] = {};
  };

  struct Applied {
    bool found = false;
    std::size_t sent = 0;
    std::size_t skipped = 0;
  };

  // Create the scene, or replace the one with that name. Returns its slot, or -1 when the
  // name is invalid, there are no actions or too many, or the table is full.
  int put(const char* name, const Action* actions, std::size_t n) noexcept {
    if (!valid_scene_name(name) || actions == nullptr || n == 0 || n > MaxActions) return -1;
    for (std::size_t i = 0; i < n; ++i) {
      if (hubproto::value_size(actions[i].value.type) == 0) return -1;
    }
    int slot = find_slot(name);
    if (slot < 0) {
      for (std::size_t i = 0; i < MaxScenes; ++i) {
        if (!items_[i].active) {
          slot = static_cast<int>(i);
          break;
        }
      }
    }
    if (slot < 0) return -1;
    Scene& s = items_[slot];
    const std::uint8_t keep_id = s.active ? s.id : free_id();  // replacing keeps the id
    s = Scene{};
    s.id = keep_id;
    s.active = true;
    std::strncpy(s.name, name, kNameCap - 1);
    s.count = static_cast<std::uint8_t>(n);
    for (std::size_t i = 0; i < n; ++i) s.actions[i] = actions[i];
    return slot;
  }

  bool remove(const char* name) noexcept {
    const int slot = find_slot(name);
    if (slot < 0) return false;
    items_[slot].active = false;
    return true;
  }

  void clear() noexcept {
    for (auto& s : items_) s.active = false;
  }

  const Scene* find(const char* name) const noexcept {
    const int slot = find_slot(name);
    return slot < 0 ? nullptr : &items_[slot];
  }

  const Scene* find_by_id(std::uint8_t id) const noexcept {
    for (const auto& s : items_) if (s.active && s.id == id) return &s;
    return nullptr;
  }

  const Scene* get(std::size_t slot) const noexcept {
    return (slot < MaxScenes && items_[slot].active) ? &items_[slot] : nullptr;
  }

  std::size_t active_count() const noexcept {
    std::size_t n = 0;
    for (const auto& s : items_) if (s.active) ++n;
    return n;
  }

  // Run every action of the scene through `send(const Action&) -> bool` (true = accepted).
  template <typename Send>
  Applied apply(const char* name, Send&& send) const noexcept {
    Applied r;
    const Scene* s = find(name);
    if (s == nullptr) return r;
    r.found = true;
    for (std::size_t i = 0; i < s->count; ++i) {
      if (send(s->actions[i])) ++r.sent;
      else ++r.skipped;
    }
    return r;
  }

  // ---- persistence ---------------------------------------------------------
  // Header (magic u16, version u8, reserved u8, scene count u16); per scene: name[24],
  // id u8, action count u8, then the actions.
  std::size_t serialized_size() const noexcept {
    std::size_t n = kHeaderSize;
    for (const auto& s : items_) if (s.active) n += kNameWire + 2 + s.count * kActionWire;
    return n;
  }

  // Returns bytes written, or 0 if `cap` is too small.
  std::size_t save(void* out, std::size_t cap) const noexcept {
    if (out == nullptr || cap < serialized_size()) return 0;
    auto* p = static_cast<std::uint8_t*>(out);
    detail::put_u16_le(p, kMagic);
    p[2] = kVersion;
    p[3] = 0;
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(active_count()));
    std::size_t off = kHeaderSize;
    for (const auto& s : items_) {
      if (!s.active) continue;
      std::memset(p + off, 0, kNameWire);
      std::memcpy(p + off, s.name, std::strlen(s.name));
      p[off + kNameWire] = s.id;
      p[off + kNameWire + 1] = s.count;
      off += kNameWire + 2;
      for (std::size_t i = 0; i < s.count; ++i) {
        const Action& a = s.actions[i];
        detail::put_u16_le(p + off, a.entity_id);
        p[off + 2] = a.attr_id;
        p[off + 3] = static_cast<std::uint8_t>(a.value.type);
        p[off + 4] = a.value.len;
        std::memcpy(p + off + 5, a.value.raw, 4);
        off += kActionWire;
      }
    }
    return off;
  }

  // Replace all scenes with those in the buffer. All-or-nothing: a corrupt or oversized
  // buffer changes nothing and returns false.
  bool load(const void* in, std::size_t len) noexcept {
    if (in == nullptr || len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in);
    if (detail::get_u16_le(p) != kMagic || p[2] != kVersion) return false;
    const std::size_t n = detail::get_u16_le(p + 4);
    if (n > MaxScenes) return false;

    Scene loaded[MaxScenes];
    std::size_t off = kHeaderSize;
    for (std::size_t i = 0; i < n; ++i) {
      if (len < off + kNameWire + 2) return false;
      Scene& s = loaded[i];
      std::memcpy(s.name, p + off, kNameCap - 1);
      s.name[kNameCap - 1] = '\0';
      if (p[off + kNameWire - 1] != 0) return false;  // not NUL-terminated within the field
      if (!valid_scene_name(s.name)) return false;
      for (std::size_t k = 0; k < i; ++k) if (std::strcmp(loaded[k].name, s.name) == 0) return false;
      s.id = p[off + kNameWire];
      if (s.id == 0) return false;
      for (std::size_t k = 0; k < i; ++k) if (loaded[k].id == s.id) return false;
      const std::size_t count = p[off + kNameWire + 1];
      off += kNameWire + 2;
      if (count == 0 || count > MaxActions || len < off + count * kActionWire) return false;
      for (std::size_t a = 0; a < count; ++a, off += kActionWire) {
        const std::uint8_t* e = p + off;
        const auto vt = static_cast<hubproto::ValueType>(e[3]);
        if (hubproto::value_size(vt) == 0 || e[4] != hubproto::value_size(vt)) return false;
        Action& act = s.actions[a];
        act.entity_id = detail::get_u16_le(e);
        act.attr_id = e[2];
        act.value.type = vt;
        act.value.len = e[4];
        std::memcpy(act.value.raw, e + 5, 4);
      }
      s.active = true;
      s.count = static_cast<std::uint8_t>(count);
    }
    clear();
    for (std::size_t i = 0; i < n; ++i) items_[i] = loaded[i];
    return true;
  }

 private:
  // The smallest id (1..255) no scene uses. Safe to reuse after a delete: a scene that something
  // refers to cannot be deleted.
  std::uint8_t free_id() const noexcept {
    for (unsigned id = 1; id <= 255; ++id) {
      if (find_by_id(static_cast<std::uint8_t>(id)) == nullptr) return static_cast<std::uint8_t>(id);
    }
    return 0;  // unreachable: at most MaxScenes <= 255 scenes exist
  }

  int find_slot(const char* name) const noexcept {
    if (name == nullptr) return -1;
    for (std::size_t i = 0; i < MaxScenes; ++i) {
      if (items_[i].active && std::strcmp(items_[i].name, name) == 0) return static_cast<int>(i);
    }
    return -1;
  }

  Scene items_[MaxScenes] = {};
};

}  // namespace core
