#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>

#include "core/plugin_config.hpp"

TEST_CASE("plugin config: opaque blob round-trip and corruption") {
  using namespace core::plugin;

  const uint8_t body[] = {1, 2, 3, 4, 5, 6, 7};
  uint8_t buf[64];
  const std::size_t n =
      encode_plugin_config("tuya", body, sizeof(body), buf, sizeof(buf));
  REQUIRE(n > 0);

  SUBCASE("round-trips an opaque body the core never parses") {
    const uint8_t* out = nullptr;
    std::size_t out_len = 0;
    CHECK(decode_plugin_config(buf, n, "tuya", &out, &out_len) ==
          ConfigError::Ok);
    REQUIRE(out_len == sizeof(body));
    CHECK(std::memcmp(out, body, out_len) == 0);
  }

  SUBCASE("a truncated buffer is rejected, not read past") {
    const uint8_t* out = nullptr;
    std::size_t out_len = 0;
    CHECK(decode_plugin_config(buf, n - 1, "tuya", &out, &out_len) ==
          ConfigError::Truncated);
  }

  SUBCASE("a wrong version byte is rejected") {
    uint8_t bad[64];
    std::memcpy(bad, buf, n);
    bad[0] = 2;
    const uint8_t* out = nullptr;
    std::size_t out_len = 0;
    CHECK(decode_plugin_config(bad, n, "tuya", &out, &out_len) ==
          ConfigError::BadVersion);
  }

  SUBCASE("a blob stored under another plugin id is refused") {
    const uint8_t* out = nullptr;
    std::size_t out_len = 0;
    CHECK(decode_plugin_config(buf, n, "other", &out, &out_len) ==
          ConfigError::IdMismatch);
  }

  SUBCASE("encoding into a too-small buffer returns 0") {
    uint8_t tiny[4];
    CHECK(encode_plugin_config("tuya", body, sizeof(body), tiny, sizeof(tiny)) ==
          0);
  }
}
