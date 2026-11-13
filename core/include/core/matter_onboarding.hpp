// Matter onboarding payloads -> the two numbers commissioning needs.
//
// The app cannot commission anything (it has no Matter stack): it reads a device's
// Bluetooth advertisement or its QR code and hands the *bytes* to the hub. Those bytes
// are the packed onboarding payload, and the hub needs exactly two fields out of it to
// start BLE commissioning: the setup passcode (PASE needs it) and the discriminator
// (the advertisement to look for).
//
// Layout, Matter 1.0 §5.1.2/§5.1.4, packed least-significant bit first into 11 bytes:
//
//   version(3) | vendor id(16) | product id(16) | commissioning flow(2) |
//   rendezvous information(8) | discriminator(12) | setup passcode(27) | padding(4)
//
// Pure and host-tested: the firmware does the CHIP calls around it, the app does the
// decoding for its own display, and neither has to agree with the other about bit order.
#pragma once

#include <cstddef>
#include <cstdint>

#include "hubproto/payload.hpp"

namespace core::matter {

// The smallest onboarding payload: the 11 packed bytes, no optional TLV.
inline constexpr std::size_t kOnboardingBytes = 11;

// CHIP's own limits for a setup passcode (kSetupPINCodeMaximumValue).
inline constexpr std::uint32_t kMaxSetupPasscode = 99999998;

struct Onboarding {
  bool valid = false;
  std::uint8_t version = 0;
  std::uint16_t discriminator = 0;
  std::uint32_t passcode = 0;
};

// Parse the packed payload itself (no kind prefix, no trailing TLV).
inline Onboarding parse_onboarding_bytes(const std::uint8_t* bytes, std::size_t len) noexcept {
  Onboarding out;
  if (bytes == nullptr || len < kOnboardingBytes) return out;

  // The fields are packed LSB first, so a bit-by-bit read is both the clearest and the
  // exact description of the format (a 64-bit shift would need two words for 84 bits).
  std::size_t bit = 0;
  auto take = [&](int n) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++bit) {
      v |= static_cast<std::uint32_t>((bytes[bit / 8] >> (bit % 8)) & 1u) << i;
    }
    return v;
  };

  out.version = static_cast<std::uint8_t>(take(3));
  (void)take(16);  // vendor id — not needed to commission
  (void)take(16);  // product id
  (void)take(2);   // commissioning flow
  (void)take(8);   // rendezvous information
  out.discriminator = static_cast<std::uint16_t>(take(12));
  out.passcode = take(27);

  // Version 1 is the only payload version Matter 1.0 defines; a passcode outside CHIP's
  // range cannot open a PASE session, so it is a bad payload rather than a device error.
  out.valid = out.version == 1 && out.discriminator <= 0xFFF && out.passcode >= 1 &&
              out.passcode <= kMaxSetupPasscode;
  return out;
}

// Parse what the app handed over, in the shape of `matter.commission`'s `kind`.
//
// Only the QR payload carries a setup passcode. The commissioning service data (kind
// 0: discriminator + setup hash) and the enhanced advertisement (kind 1: version,
// vendor/product, discriminator, setup hash, attestation) identify the device but hold
// no passcode — PASE cannot start without one, so the person has to scan the QR code
// (or type the code printed on the label). That is the protocol's `no_setup_hash` case,
// not a parser bug.
inline Onboarding parse_onboarding(std::uint8_t kind, const std::uint8_t* bytes,
                                   std::size_t len) noexcept {
  if (kind != hubproto::kMatterKindQr) return {};
  return parse_onboarding_bytes(bytes, len);
}

}  // namespace core::matter
