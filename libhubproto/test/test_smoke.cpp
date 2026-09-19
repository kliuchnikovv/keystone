// Phase 0 smoke test: proves the host test harness builds, links against
// libhubproto, and runs under ctest. Real protocol tests (COBS round-trip,
// CRC16, header/payload codec) arrive in Phase 1.
#include <doctest/doctest.h>

#include "hubproto/version.hpp"

TEST_CASE("protocol version is v1 per SPEC 4.2") {
  CHECK(hubproto::kProtocolVersion == 0x01);
}

TEST_CASE("library exposes a non-empty version string") {
  const char* v = hubproto::library_version();
  REQUIRE(v != nullptr);
  CHECK(v[0] != '\0');
}
