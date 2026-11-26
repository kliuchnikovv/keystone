#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "core/plugin_host_api.hpp"
#include "core/plugin_manifest.hpp"

namespace {
core::plugin::ManifestError parse(const std::string& json,
                                  core::plugin::Manifest& out) {
  return core::plugin::parse_manifest(json.c_str(), json.size(), out);
}
}  // namespace

TEST_CASE("plugin manifest: parse, abi check, bad capability") {
  using namespace core::plugin;
  Manifest m;

  SUBCASE("a valid manifest parses, fields and caps populated") {
    const auto err = parse(
        R"({"id":"tuya","version":3,"abi":1,)"
        R"("caps":["net_local","entity_declare","aes","json"]})",
        m);
    REQUIRE(err == ManifestError::Ok);
    CHECK(std::string(m.id) == "tuya");
    CHECK(m.version == 3u);
    CHECK(m.abi == 1u);
    CHECK(m.caps.has(Cap::NetLocal));
    CHECK(m.caps.has(Cap::EntityDeclare));
    CHECK(m.caps.has(Cap::Aes));
    CHECK(m.caps.has(Cap::Json));
    CHECK_FALSE(m.caps.has(Cap::BusCommand));
  }

  SUBCASE("wrong abi version is rejected") {
    CHECK(parse(R"({"id":"x","version":1,"abi":2,"caps":[]})", m) ==
          ManifestError::AbiIncompatible);
  }

  SUBCASE("an unknown capability name is rejected") {
    CHECK(parse(R"({"id":"x","version":1,"abi":1,"caps":["net_local","teleport"]})",
                m) == ManifestError::BadCapability);
  }

  SUBCASE("a missing required field is rejected") {
    CHECK(parse(R"({"version":1,"abi":1,"caps":[]})", m) ==
          ManifestError::MissingField);
  }

  SUBCASE("malformed json is rejected") {
    CHECK(parse("{bad", m) == ManifestError::BadJson);
  }

  SUBCASE("a manifest with a duplicate known key is rejected") {
    // Parser differential: our reader takes the first "caps", another parser
    // might take the last. Capabilities are a security boundary, so the
    // ambiguity is refused rather than resolved silently.
    CHECK(parse(R"({"id":"x","version":1,"abi":1,)"
                R"("caps":["net_local"],"caps":["log"]})",
                m) == ManifestError::BadJson);
  }

  SUBCASE("a duplicate hidden behind a JSON escape is rejected") {
    // The second key is "caps", which DECODES to "caps". A raw-byte
    // compare misses it (the bytes differ); a decoding parser does not. The
    // escape must survive the C++ raw string, so it reaches the JSON reader.
    CHECK(parse("{\"id\":\"x\",\"version\":1,\"abi\":1,"
                "\"caps\":[\"net_local\"],\"ca\\u0070s\":[\"log\"]}",
                m) == ManifestError::BadJson);
  }

  SUBCASE("a duplicate of any key, not only known ones, is rejected") {
    CHECK(parse(R"({"id":"x","version":1,"abi":1,"foo":1,"foo":2})", m) ==
          ManifestError::BadJson);
  }

  SUBCASE("version/abi that overflow u32 are rejected, not wrapped") {
    // 2^32 + 1 wraps to 1 under a naive cast; an anti-rollback version check or
    // the ABI gate would then be bypassed. Both must be refused.
    CHECK(parse(R"({"id":"x","version":4294967297,"abi":1,"caps":[]})", m) !=
          ManifestError::Ok);
    CHECK(parse(R"({"id":"x","version":1,"abi":4294967297,"caps":[]})", m) ==
          ManifestError::AbiIncompatible);
  }

  SUBCASE("an empty id is rejected") {
    CHECK(parse(R"({"id":"","version":1,"abi":1,"caps":[]})", m) !=
          ManifestError::Ok);
  }

  SUBCASE("an id with illegal characters is rejected") {
    CHECK(parse(R"({"id":"has space","version":1,"abi":1,"caps":[]})", m) !=
          ManifestError::Ok);
  }

  SUBCASE("an id hiding a NUL is rejected, not truncated to a short id") {
    // "a\u0000b" decodes to a, NUL, b. strlen() would see only "a", so it could
    // spoof the id "a". A decoded NUL must make the whole value invalid.
    CHECK(parse("{\"id\":\"a\\u0000b\",\"version\":1,\"abi\":1,\"caps\":[]}",
                m) != ManifestError::Ok);
  }

  SUBCASE("an over-long id is rejected") {
    CHECK(parse(R"({"id":"abcdefghijklmnopqrstuvwxyz0123456789","version":1,)"
                R"("abi":1,"caps":[]})",
                m) == ManifestError::TooLong);
  }
}
