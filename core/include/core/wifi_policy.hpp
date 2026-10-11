// Wi-Fi policy that does not need a radio: which credentials are acceptable, how long to wait
// before the next reconnect attempt, and the "try the new network, go back if it does not work"
// decision. The brain owns the actual esp_wifi calls; this keeps the rules host-testable.
//
// Why a trial: changing the network over the app means the hub leaves the network the app is
// talking through. A typo would strand it with no way to be reached. So the new credentials are
// only kept once the hub really got an address on that network; otherwise it goes back.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace core {

inline constexpr std::size_t kMaxSsidLen = 32;
inline constexpr std::size_t kMinPasswordLen = 8;   // WPA2-PSK passphrase
inline constexpr std::size_t kMaxPasswordLen = 63;  // (64 hex digits is a raw PSK, also accepted)

/** SSID 1..32 bytes without control characters; password empty (an open network), 8..63
 *  printable characters, or exactly 64 hex digits. `why` names the problem. */
inline bool wifi_credentials_valid(const char* ssid, const char* password, const char** why = nullptr) noexcept {
  auto fail = [&](const char* m) {
    if (why != nullptr) *why = m;
    return false;
  };
  if (ssid == nullptr || password == nullptr) return fail("missing");
  const std::size_t sl = std::strlen(ssid);
  if (sl < 1 || sl > kMaxSsidLen) return fail("ssid: 1..32 bytes");
  for (std::size_t i = 0; i < sl; ++i) {
    if (static_cast<unsigned char>(ssid[i]) < 0x20 || ssid[i] == 0x7F) return fail("ssid: no control characters");
  }
  const std::size_t pl = std::strlen(password);
  if (pl == 0) return true;
  if (pl == 64) {
    for (std::size_t i = 0; i < pl; ++i) {
      const char c = password[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
        return fail("password: 64 characters must be hex digits");
      }
    }
    return true;
  }
  if (pl < kMinPasswordLen || pl > kMaxPasswordLen) return fail("password: empty (open network), or 8..63 characters");
  for (std::size_t i = 0; i < pl; ++i) {
    if (static_cast<unsigned char>(password[i]) < 0x20 || password[i] == 0x7F) return fail("password: no control characters");
  }
  return true;
}

/** Delay before reconnect attempt number `attempt` (0 = the first retry): 1 s, 2 s, 4 s … capped at 30 s. */
inline std::uint32_t wifi_reconnect_delay_ms(unsigned attempt) noexcept {
  if (attempt >= 5) return 30000;  // 1,2,4,8,16 then the cap
  const std::uint32_t d = 1000u << attempt;
  return d > 30000 ? 30000 : d;
}

/**
 * One attempt to move to new credentials. `begin` when the new ones are applied, `on_connected` when
 * the hub got an address, `poll` on every pass of the main loop: it answers Commit (keep them) or
 * Revert (go back to the old ones) exactly once.
 */
class WifiTrial {
 public:
  enum class Decision : std::uint8_t { None, Commit, Revert };

  /** `can_revert`: there was a working network to go back to. Without one the new credentials are kept at
   *  once (nothing is lost by trying them for good). */
  void begin(std::uint32_t now_ms, std::uint32_t timeout_ms, bool can_revert) noexcept {
    active_ = true;
    connected_ = false;
    can_revert_ = can_revert;
    start_ = now_ms;
    timeout_ = timeout_ms;
  }
  void on_connected() noexcept {
    if (active_) connected_ = true;
  }
  bool active() const noexcept { return active_; }

  Decision poll(std::uint32_t now_ms) noexcept {
    if (!active_) return Decision::None;
    if (connected_ || !can_revert_) {
      active_ = false;
      return Decision::Commit;
    }
    if (static_cast<std::uint32_t>(now_ms - start_) >= timeout_) {
      active_ = false;
      return Decision::Revert;
    }
    return Decision::None;
  }

 private:
  bool active_ = false;
  bool connected_ = false;
  bool can_revert_ = false;
  std::uint32_t start_ = 0;
  std::uint32_t timeout_ = 0;
};

}  // namespace core
