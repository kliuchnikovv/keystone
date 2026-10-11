#include <doctest/doctest.h>

#include <string>

#include "core/wifi_policy.hpp"

TEST_CASE("credentials: SSID 1..32 bytes; password empty, 8..63 printable, or 64 hex") {
  const char* why = nullptr;
  CHECK(core::wifi_credentials_valid("home", "correct horse", &why));
  CHECK(core::wifi_credentials_valid("home", "", &why));                       // open network
  CHECK(core::wifi_credentials_valid("home", "12345678", &why));               // shortest passphrase
  CHECK(core::wifi_credentials_valid("home", std::string(63, 'x').c_str(), &why));
  CHECK(core::wifi_credentials_valid("home", std::string(64, 'a').c_str(), &why));  // raw PSK
  CHECK(core::wifi_credentials_valid("кухня", "пароль-ok", &why));             // UTF-8 SSID (bytes, not letters)
  CHECK(core::wifi_credentials_valid(std::string(32, 's').c_str(), "", &why));

  CHECK_FALSE(core::wifi_credentials_valid("", "12345678", &why));
  CHECK(std::string(why).find("ssid") == 0);
  CHECK_FALSE(core::wifi_credentials_valid(std::string(33, 's').c_str(), "", &why));
  CHECK_FALSE(core::wifi_credentials_valid("a\nb", "12345678", &why));
  CHECK_FALSE(core::wifi_credentials_valid("home", "short", &why));
  CHECK(std::string(why).find("password") == 0);
  CHECK_FALSE(core::wifi_credentials_valid("home", std::string(65, 'x').c_str(), &why));
  CHECK_FALSE(core::wifi_credentials_valid("home", std::string(64, 'z').c_str(), &why));  // 64 but not hex
  CHECK_FALSE(core::wifi_credentials_valid("home", "tab\there1", &why));
  CHECK_FALSE(core::wifi_credentials_valid(nullptr, "12345678", &why));
  CHECK_FALSE(core::wifi_credentials_valid("home", nullptr, &why));
}

TEST_CASE("reconnect back-off doubles from one second and stops at thirty") {
  CHECK(core::wifi_reconnect_delay_ms(0) == 1000);
  CHECK(core::wifi_reconnect_delay_ms(1) == 2000);
  CHECK(core::wifi_reconnect_delay_ms(2) == 4000);
  CHECK(core::wifi_reconnect_delay_ms(3) == 8000);
  CHECK(core::wifi_reconnect_delay_ms(4) == 16000);
  CHECK(core::wifi_reconnect_delay_ms(5) == 30000);
  CHECK(core::wifi_reconnect_delay_ms(1000000) == 30000);
}

TEST_CASE("a trial is kept once the hub is connected, and undone when the time runs out") {
  core::WifiTrial t;
  CHECK(t.poll(0) == core::WifiTrial::Decision::None);  // nothing going on
  t.begin(1000, 25000, /*can_revert=*/true);
  CHECK(t.active());
  CHECK(t.poll(1001) == core::WifiTrial::Decision::None);
  CHECK(t.poll(25999) == core::WifiTrial::Decision::None);
  t.on_connected();
  CHECK(t.poll(26000) == core::WifiTrial::Decision::Commit);  // connected before the deadline: keep
  CHECK_FALSE(t.active());
  CHECK(t.poll(60000) == core::WifiTrial::Decision::None);    // decided once

  t.begin(1000, 25000, true);
  CHECK(t.poll(25999) == core::WifiTrial::Decision::None);
  CHECK(t.poll(26000) == core::WifiTrial::Decision::Revert);  // exactly at the timeout
  CHECK(t.poll(26001) == core::WifiTrial::Decision::None);
}

TEST_CASE("without a working network to go back to, the new credentials are kept at once") {
  core::WifiTrial t;
  t.begin(0, 25000, /*can_revert=*/false);
  CHECK(t.poll(1) == core::WifiTrial::Decision::Commit);
}

TEST_CASE("a connect that arrives after the verdict, or with no trial, changes nothing; the clock may wrap") {
  core::WifiTrial t;
  t.on_connected();  // no trial: ignored
  CHECK(t.poll(5) == core::WifiTrial::Decision::None);
  t.begin(4294967000u, 25000, true);
  CHECK(t.poll(4294967200u) == core::WifiTrial::Decision::None);
  CHECK(t.poll(24000) == core::WifiTrial::Decision::None);   // 24000 - 4294967000 wraps to ~24296: still inside
  CHECK(t.poll(25000) == core::WifiTrial::Decision::Revert);  // wrapped difference is now 25296
  t.on_connected();  // too late
  CHECK(t.poll(25001) == core::WifiTrial::Decision::None);
}
