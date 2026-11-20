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

  SUBCASE("an over-long id is rejected") {
    CHECK(parse(R"({"id":"abcdefghijklmnopqrstuvwxyz0123456789","version":1,)"
                R"("abi":1,"caps":[]})",
                m) == ManifestError::TooLong);
  }
}
