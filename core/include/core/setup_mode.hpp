// First-time setup of a hub that has no network (or lost its network for good), with no console.
//
// The hub opens its own Wi-Fi access point, `Keystone-XXXX`, protected by a *setup code* that was
// generated with the hub's TLS identity (scripts/gen-hub-cert.sh) and is meant to be printed on a
// label. The phone joins that access point and speaks the usual app protocol to the hub over wss,
// authenticating with the setup code. A client that did so can do exactly two things: scan for
// networks and send `setup.apply` (the home network's name and password, plus the long-term app
// token). It cannot touch devices, rules or anything else.
//
// This file is the part that needs no radio, so it can be tested on the host: what a code looks
// like, the access point's name, the operations a setup client may use, and when the access point
// is up (SetupPolicy). The brain does the esp_wifi calls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace core {

inline constexpr std::size_t kSetupCodeLen = 12;
/** 31 letters and digits that cannot be mistaken for each other (no 0/O, 1/I/L). ~59 bits in 12. */
inline constexpr char kSetupAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

/** Exactly kSetupCodeLen characters of kSetupAlphabet. */
inline bool setup_code_valid(const char* code) noexcept {
  if (code == nullptr || std::strlen(code) != kSetupCodeLen) return false;
  for (std::size_t i = 0; i < kSetupCodeLen; ++i) {
    if (std::strchr(kSetupAlphabet, code[i]) == nullptr || code[i] == '\0') return false;
  }
  return true;
}

/** What a person types or reads off a label ("abcd-efgh-jkmn", spaces, any case) -> the code as stored.
 *  `out` needs kSetupCodeLen + 1 bytes. False (and `out` empty) if it does not read as a code. */
inline bool setup_code_normalize(const char* in, char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap < kSetupCodeLen + 1) return false;
  out[0] = '\0';
  if (in == nullptr) return false;
  std::size_t n = 0;
  for (; *in != '\0'; ++in) {
    char c = *in;
    if (c == '-' || c == ' ') continue;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (n == kSetupCodeLen) return false;  // too long
    out[n++] = c;
  }
  out[n] = '\0';
  if (!setup_code_valid(out)) {
    out[0] = '\0';
    return false;
  }
  return true;
}

/** "Keystone-" + the last two bytes of the MAC in hex: tells two hubs apart. `cap` >= 14. */
inline void setup_ap_ssid(const std::uint8_t mac[6], char* out, std::size_t cap) noexcept {
  std::snprintf(out, cap, "Keystone-%02X%02X", static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]));
}

/** The only operations a client that authenticated with the setup code may use. */
inline bool setup_op_allowed(const char* op) noexcept {
  return op != nullptr && (std::strcmp(op, "wifi.scan") == 0 || std::strcmp(op, "setup.apply") == 0);
}

/**
 * When the access point is up. It opens at once when the hub has no network credentials at all, and
 * after kDownMs without a connection when it has some (the router was replaced, the hub moved house).
 * Once open it stays until the hub has had its network for kLingerMs, which is long enough for the app
 * to find out that the setup worked. `force` is the console's switch.
 */
class SetupPolicy {
 public:
  static constexpr std::uint32_t kDownMs = 10u * 60u * 1000u;
  static constexpr std::uint32_t kLingerMs = 60u * 1000u;

  void update(std::uint32_t now, bool sta_up, bool have_creds) noexcept {
    if (!seen_) {
      seen_ = true;
      since_ = now;
      up_ = sta_up;
    }
    if (sta_up != up_) {
      up_ = sta_up;
      since_ = now;
    }
    const std::uint32_t age = static_cast<std::uint32_t>(now - since_);
    if (sta_up) {
      if (active_ && age >= kLingerMs) active_ = false;
    } else if (!have_creds || age >= kDownMs) {
      active_ = true;
    }
  }

  /** The console's switch: on = open now (until it is turned off), off = closed and forgotten. */
  void force(bool on) noexcept {
    forced_ = on;
    if (!on) active_ = false;
  }

  bool ap_wanted() const noexcept { return active_ || forced_; }

 private:
  bool seen_ = false;
  bool up_ = false;
  bool active_ = false;
  bool forced_ = false;
  std::uint32_t since_ = 0;
};

}  // namespace core
