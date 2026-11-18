// Presence: the phones that report where they are.
//
// A phone is not a radio device on the hub — it has no chip, no Zigbee endpoint, no
// Matter accessory. But it can say "I am home" or "I am away", and a rule should be
// able to react to that exactly as it reacts to a motion sensor.
//
// The trick: a phone is a VIRTUAL ENTITY. The hub gives it an entity id and one
// boolean attribute ("home"), and it looks exactly like a device that reports
// `occupied`. A rule refers to it like any other device, so the rule engine does not
// change at all.
//
// Each phone is its own entity, with its own name ("Anna's phone"), so a house with
// two people has two entities and two rules.
//
// What lives here (portable, no radio, host-testable):
//   - the table of phones: entity id, name, last report, the value it reported
//   - staleness: a phone that stopped reporting is UNKNOWN, not "away". Otherwise a
//     crashed app would switch the lights off
//   - persistence: the phones survive a reboot (so the rules that point at them
//     still point at something)
//
// The attribute id lives in hubproto::attr::kHome, so the app and the hub agree on
// what "home" is without inventing a second table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/endian.hpp"
#include "hubproto/attrs.hpp"

namespace core {

// The virtual phones. Entity ids are allocated by the hub in a range of its own, so
// they cannot collide with a radio device (those come from the chips).
inline constexpr std::uint16_t kPhoneEntityBase = 0x0F80;  // above the Matter inject range
inline constexpr std::uint16_t kPhoneEntityLimit = 0x0FB0;  // exclusive

// How long a phone's report stays true. After this the phone is UNKNOWN again: the
// hub stops believing "home" and the rules that watch it stay quiet rather than
// acting on a stale answer.
inline constexpr std::uint32_t kPresenceStaleS = 15 * 60;  // 15 minutes

// The presence of one phone.
struct PhonePresence {
  std::uint16_t entity_id = 0;
  bool in_use = false;
  bool home = false;          // what the phone last said
  bool known = false;         // false until the first report
  std::uint32_t last_report_s = 0;  // the hub's uptime when it last reported
  char name[24] = {0};        // "Anna's phone" (short: it is a label)

  // Has the phone gone quiet for too long to believe?
  bool stale(std::uint32_t now_s) const noexcept {
    if (!in_use || !known) return true;
    return static_cast<std::uint32_t>(now_s - last_report_s) >= kPresenceStaleS;
  }

  // What the phone's presence is right now: home, away, or unknown.
  enum class State : std::uint8_t { Unknown, Home, Away };
  State state(std::uint32_t now_s) const noexcept {
    if (stale(now_s)) return State::Unknown;
    return home ? State::Home : State::Away;
  }
};

template <std::size_t MaxPhones = 4>
class PresenceTable {
 public:
  using Phone = PhonePresence;

  /** The phone with this entity id, or nullptr. */
  const Phone* find(std::uint16_t entity_id) const noexcept {
    for (const auto& p : phones_) {
      if (p.in_use && p.entity_id == entity_id) return &p;
    }
    return nullptr;
  }

  Phone* find(std::uint16_t entity_id) noexcept {
    return const_cast<Phone*>(static_cast<const PresenceTable*>(this)->find(entity_id));
  }

  /** The phone with this name, or nullptr. Names are unique. */
  const Phone* find_by_name(const char* name) const noexcept {
    if (name == nullptr) return nullptr;
    for (const auto& p : phones_) {
      if (p.in_use && std::strcmp(p.name, name) == 0) return &p;
    }
    return nullptr;
  }

  std::size_t count() const noexcept {
    std::size_t n = 0;
    for (const auto& p : phones_) {
      if (p.in_use) ++n;
    }
    return n;
  }

  /** The phone at index i (in table order), or nullptr. */
  const Phone* at(std::size_t i) const noexcept {
    std::size_t n = 0;
    for (const auto& p : phones_) {
      if (!p.in_use) continue;
      if (n == i) return &p;
      ++n;
    }
    return nullptr;
  }

  /**
   * A phone reports where it is. Creates the phone if it is new (by name), updates it
   * if it is known.
   *
   * Returns false when the table is full of other phones, or the name is unusable.
   * `now_s` is the hub's uptime in seconds — the hub has no wall clock to trust yet
   * when the phone reports for the first time.
   */
  bool report(const char* name, bool home, std::uint32_t now_s) noexcept {
    if (name == nullptr || name[0] == '\0') return false;
    std::size_t len = 0;
    while (name[len] != '\0') {
      if (++len >= sizeof(phones_[0].name)) return false;  // too long for the field
    }

    Phone* slot = nullptr;
    for (auto& p : phones_) {
      if (p.in_use && std::strcmp(p.name, name) == 0) {
        slot = &p;
        break;
      }
    }
    if (slot == nullptr) {
      for (auto& p : phones_) {
        if (!p.in_use) {
          slot = &p;
          break;
        }
      }
    }
    if (slot == nullptr) return false;  // table full

    if (!slot->in_use) {
      slot->in_use = true;
      slot->entity_id = next_entity_id();
      slot->known = false;
      slot->home = false;
      std::memset(slot->name, 0, sizeof(slot->name));
      std::memcpy(slot->name, name, len);
    }
    slot->home = home;
    slot->known = true;
    slot->last_report_s = now_s;
    return true;
  }

  /** Forget a phone (the person removed it in the app). */
  bool remove(std::uint16_t entity_id) noexcept {
    for (auto& p : phones_) {
      if (p.in_use && p.entity_id == entity_id) {
        p.in_use = false;
        return true;
      }
    }
    return false;
  }

  /** Give the phone a different name. */
  bool rename(std::uint16_t entity_id, const char* name) noexcept {
    Phone* p = find(entity_id);
    if (p == nullptr || name == nullptr || name[0] == '\0') return false;
    std::size_t len = 0;
    while (name[len] != '\0') {
      if (++len >= sizeof(p->name)) return false;
    }
    for (const auto& other : phones_) {
      if (other.in_use && other.entity_id != entity_id && std::strcmp(other.name, name) == 0) {
        return false;  // the name is taken
      }
    }
    std::memset(p->name, 0, sizeof(p->name));
    std::memcpy(p->name, name, len);
    return true;
  }

  /** The attribute value a phone has right now: unknown -> no value at all. */
  bool value_of(std::uint16_t entity_id, std::uint32_t now_s, bool& out) const noexcept {
    const Phone* p = find(entity_id);
    if (p == nullptr) return false;
    const auto st = p->state(now_s);
    if (st == Phone::State::Unknown) return false;
    out = (st == Phone::State::Home);
    return true;
  }

  /**
   * The phone whose presence went stale since the last look: it stopped reporting,
   * so the hub must stop believing it. The caller publishes "not home" for it, so
   * a rule that watches it stops acting on an answer that is no longer true.
   *
   * Returns the phone, or nullptr when nothing went stale.
   */
  const Phone* take_stale(std::uint32_t now_s) noexcept {
    for (auto& p : phones_) {
      if (!p.in_use || !p.known) continue;
      if (!p.stale(now_s)) continue;
      // Marked as unknown, so this happens once and not on every pass.
      p.known = false;
      p.home = false;
      return &p;
    }
    return nullptr;
  }

  // ---- persistence ---------------------------------------------------------
  //
  // The phones survive a reboot, so a rule that points at "Anna's phone" still
  // points at something after the hub restarts. The last report time is NOT kept:
  // the hub's uptime starts at zero, so a saved timestamp would be meaningless
  // (and could even look "fresh" for a while).

  static constexpr std::uint16_t kMagic = 0x5052;  // 'PR'
  static constexpr std::uint8_t kVersion = 1;
  static constexpr std::size_t kHeaderSize = 6;
  static constexpr std::size_t kEntryWireSize = 2 + 24;  // entity id + name

  std::size_t serialized_size() const noexcept {
    return kHeaderSize + count() * kEntryWireSize;
  }

  /** Writes the table into `out`; returns the bytes written, or 0 if `cap` is too small. */
  std::size_t save(void* out, std::size_t cap) const noexcept {
    if (out == nullptr || cap < serialized_size()) return 0;
    auto* p = static_cast<std::uint8_t*>(out);
    detail::put_u16_le(p, kMagic);
    p[2] = kVersion;
    p[3] = 0;
    detail::put_u16_le(p + 4, static_cast<std::uint16_t>(count()));
    std::size_t off = kHeaderSize;
    for (const auto& phone : phones_) {
      if (!phone.in_use) continue;
      detail::put_u16_le(p + off, phone.entity_id);
      std::memcpy(p + off + 2, phone.name, 24);
      off += kEntryWireSize;
    }
    return off;
  }

  /**
   * Replaces the table with the phones in the buffer. All-or-nothing: a corrupt or
   * oversized buffer changes nothing and returns false.
   */
  bool load(const void* in, std::size_t len) noexcept {
    if (in == nullptr || len < kHeaderSize) return false;
    const auto* p = static_cast<const std::uint8_t*>(in);
    if (detail::get_u16_le(p) != kMagic || p[2] != kVersion) return false;
    const std::size_t n = detail::get_u16_le(p + 4);
    if (len < kHeaderSize + n * kEntryWireSize) return false;
    if (n > MaxPhones) return false;

    Phone loaded[MaxPhones];
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint8_t* e = p + kHeaderSize + i * kEntryWireSize;
      loaded[i].in_use = true;
      loaded[i].entity_id = detail::get_u16_le(e);
      std::memcpy(loaded[i].name, e + 2, 24);
      loaded[i].name[23] = '\0';
      loaded[i].known = false;   // a fresh boot: the phone has not reported yet
      loaded[i].home = false;
      loaded[i].last_report_s = 0;
      if (loaded[i].entity_id < kPhoneEntityBase || loaded[i].entity_id >= kPhoneEntityLimit) {
        return false;  // not one of ours
      }
    }
    for (std::size_t i = 0; i < MaxPhones; ++i) phones_[i] = loaded[i];
    return true;
  }

 private:
  /** The next free entity id in the phone range, skipping the ones already taken. */
  std::uint16_t next_entity_id() const noexcept {
    for (std::uint16_t id = kPhoneEntityBase; id < kPhoneEntityLimit; ++id) {
      bool taken = false;
      for (const auto& p : phones_) {
        if (p.in_use && p.entity_id == id) {
          taken = true;
          break;
        }
      }
      if (!taken) return id;
    }
    return 0;
  }

  Phone phones_[MaxPhones] = {};
};

}  // namespace core
