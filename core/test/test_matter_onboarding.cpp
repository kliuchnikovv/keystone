#include <doctest/doctest.h>

#include <cstdint>

#include "core/matter_onboarding.hpp"
#include "hubproto/payload.hpp"

using core::matter::Onboarding;
using core::matter::parse_onboarding;
using core::matter::parse_onboarding_bytes;

namespace {

// The inverse of the parser: pack the Matter onboarding fields LSB first.
struct Payload {
  std::uint8_t bytes[11] = {};
  std::size_t bit = 0;

  void put(std::uint32_t value, int n) {
    for (int i = 0; i < n; ++i, ++bit) {
      if ((value >> i) & 1u) bytes[bit / 8] = static_cast<std::uint8_t>(bytes[bit / 8] | (1u << (bit % 8)));
    }
  }
};

Payload make_payload(std::uint32_t version, std::uint16_t discriminator, std::uint32_t passcode) {
  Payload p;
  p.put(version, 3);
  p.put(0xFFF1, 16);  // vendor id
  p.put(0x8000, 16);  // product id
  p.put(0, 2);        // commissioning flow
  p.put(0x04, 8);     // rendezvous: BLE
  p.put(discriminator, 12);
  p.put(passcode, 27);
  return p;
}

}  // namespace

TEST_CASE("an onboarding payload yields the passcode and the discriminator") {
  const Payload p = make_payload(1, 0x0FA, 20202021);
  const Onboarding o = parse_onboarding_bytes(p.bytes, sizeof(p.bytes));
  CHECK(o.valid);
  CHECK(o.version == 1);
  CHECK(o.discriminator == 0x0FA);
  CHECK(o.passcode == 20202021u);
}

TEST_CASE("the QR kind is the packed payload exactly") {
  const Payload p = make_payload(1, 3840, 20202021);
  const Onboarding o = parse_onboarding(hubproto::kMatterKindQr, p.bytes, sizeof(p.bytes));
  CHECK(o.valid);
  CHECK(o.discriminator == 3840);
  CHECK(o.passcode == 20202021u);
}

TEST_CASE("the enhanced advertisement identifies the device but carries no passcode") {
  // opcode(1) | version(1) | vendor(2) | product(2) | discriminator(2) | setup hash(4)
  const std::uint8_t advert[12] = {0x00, 0x01, 0xF1, 0xFF, 0x00, 0x80,
                                   0x39, 0x02, 0x11, 0x22, 0x33, 0x44};
  CHECK_FALSE(parse_onboarding(hubproto::kMatterKindManufacturer, advert, sizeof(advert)).valid);
}

TEST_CASE("a trailing TLV after the packed payload is ignored") {
  const Payload p = make_payload(1, 7, 1234567);
  std::uint8_t longer[20] = {};
  for (std::size_t i = 0; i < sizeof(p.bytes); ++i) longer[i] = p.bytes[i];
  const Onboarding o = parse_onboarding(hubproto::kMatterKindQr, longer, sizeof(longer));
  CHECK(o.valid);
  CHECK(o.passcode == 1234567u);
}

TEST_CASE("the commissioning service data is not enough to commission") {
  const std::uint8_t service[7] = {0, 0xFA, 0x00, 1, 2, 3, 4};
  CHECK_FALSE(parse_onboarding(hubproto::kMatterKindService, service, sizeof(service)).valid);
}

TEST_CASE("a payload that cannot open PASE is rejected") {
  const Payload short_one = make_payload(1, 1, 1);
  CHECK_FALSE(parse_onboarding_bytes(short_one.bytes, 10).valid);  // truncated

  const Payload wrong_version = make_payload(2, 1, 1234567);
  CHECK_FALSE(parse_onboarding_bytes(wrong_version.bytes, sizeof(wrong_version.bytes)).valid);

  const Payload zero_pin = make_payload(1, 1, 0);
  CHECK_FALSE(parse_onboarding_bytes(zero_pin.bytes, sizeof(zero_pin.bytes)).valid);

  const Payload too_big = make_payload(1, 1, core::matter::kMaxSetupPasscode + 1);
  CHECK_FALSE(parse_onboarding_bytes(too_big.bytes, sizeof(too_big.bytes)).valid);
}

TEST_CASE("the last valid passcode is accepted") {
  const Payload p = make_payload(1, 0xFFF, core::matter::kMaxSetupPasscode);
  const Onboarding o = parse_onboarding_bytes(p.bytes, sizeof(p.bytes));
  CHECK(o.valid);
  CHECK(o.discriminator == 0xFFF);
  CHECK(o.passcode == core::matter::kMaxSetupPasscode);
}
