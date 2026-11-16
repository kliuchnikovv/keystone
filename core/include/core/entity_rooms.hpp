// EntityRooms: entity_id -> the room a device lives in.
//
// The names table (core/entity_names.hpp) says how a device is *called*; this one
// says *where it is*. A room is free text a person writes ("Kitchen"), and as
// many devices as the person likes share one — unlike the names, which are
// unique per device.
//
// Allocation-free, fixed capacity, portable: the brain keeps it in NVS and the
// app protocol carries it in the snapshot.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"

namespace core {

template <std::size_t MaxEntities = 64, std::size_t MaxRoomLen = 32>
class EntityRooms {
 public:
  static constexpr std::uint16_t kMagic = 0x524D;  // 'RM'
  static constexpr std::uint8_t  kVersion = 1;
  static constexpr std::size_t   kHeaderSize = 6;
  static constexpr std::size_t   kEntryWireSize = 2 + MaxRoomLen;
  static constexpr std::size_t   kMaxEntities = MaxEntities;
  static constexpr std::size_t   kMaxRoomLen = MaxRoomLen;

  // Put a device in a room. Fails when the room is empty, too long, or the
  // table is full and this device is not in it yet.
  bool set_room(std::uint16_t id, const char* room) noexcept {
    if (room == nullptr || room[0] == '\0') return false;
    std::size_t len = 0;
    while (room[len] != '\0') {
      if (++len >= MaxRoomLen) return false;
    }

    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].id == id) {
        std::memcpy(entries_[i].room, room, len + 1);
        return true;
      }
    }
    if (count_ >= MaxEntities) return false;
    entries_[count_].id = id;
    std::memcpy(entries_[count_].room, room, len + 1);
    ++count_;
    return true;
  }

  // Take a device out of its room. True when it was in one.
  bool clear_room(std::uint16_t id) noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].id == id) {
        entries_[i] = entries_[count_ - 1];
        --count_;
        return true;
      }
    }
    return false;
  }

  // The room of a device, or nullptr when it has none.
  const char* get_room(std::uint16_t id) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (entries_[i].id == id) return entries_[i].room;
    }
    return nullptr;
  }

  // How many devices are in a room.
  std::size_t size() const noexcept { return count_; }

  // Every room in the table, once each (the caller gets the names, not the ids).
  template <typename Fn>
  void each_room(Fn fn) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      bool seen = false;
      for (std::size_t j = 0; j < i && !seen; ++j) {
        if (std::strcmp(entries_[j].room, entries_[i].room) == 0) seen = true;
      }
      if (!seen) fn(entries_[i].room);
    }
  }

  // Enumerate the entry at index [0, size()).
  bool entry_at(std::size_t index, std::uint16_t& id, const char*& room) const noexcept {
    if (index >= count_) return false;
    id = entries_[index].id;
    room = entries_[index].room;
    return true;
  }

  // The bytes the table needs when it is saved.
  std::size_t serialized_size() const noexcept {
    return kHeaderSize + count_ * kEntryWireSize;
  }

  // Save into a buffer. Returns the bytes written, or 0 when it does not fit.
  std::size_t save(void* out_buf, std::size_t out_cap) const noexcept {
    if (out_buf == nullptr) return 0;
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
      std::memset(p + off, 0, MaxRoomLen);
      std::strncpy(reinterpret_cast<char*>(p + off), entries_[i].room, MaxRoomLen - 1);
      off += MaxRoomLen;
    }
    return required;
  }

  // Load from a saved buffer. A corrupt blob leaves the table empty (the caller
  // keeps the old one and says so).
  bool load(const void* in_buf, std::size_t in_len) noexcept {
    if (in_buf == nullptr || in_len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in_buf);
    if (detail::get_u16_le(p + 0) != kMagic) return false;
    if (p[2] != kVersion) return false;

    const std::uint16_t num_entries = detail::get_u16_le(p + 4);
    if (num_entries > MaxEntities) return false;
    if (in_len < kHeaderSize + static_cast<std::size_t>(num_entries) * kEntryWireSize) return false;

    clear();
    std::size_t off = kHeaderSize;
    for (std::size_t i = 0; i < num_entries; ++i) {
      const std::uint16_t id = detail::get_u16_le(p + off);
      off += 2;
      char room[MaxRoomLen + 1] = {};
      std::memcpy(room, p + off, MaxRoomLen);
      room[MaxRoomLen] = '\0';
      off += MaxRoomLen;
      if (!set_room(id, room)) {
        clear();  // half a table is worse than none: the caller keeps what it had
        return false;
      }
    }
    return true;
  }

  void clear() noexcept { count_ = 0; }

 private:
  struct StorageEntry {
    std::uint16_t id;
    char          room[MaxRoomLen];
  };

  StorageEntry entries_[MaxEntities] = {};
  std::size_t  count_ = 0;
};

}  // namespace core
