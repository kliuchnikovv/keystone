#include <doctest/doctest.h>

#include <cstdlib>
#include <initializer_list>

#include "hubproto/units.hpp"

using namespace hubproto::units;

TEST_CASE("temperature: centi-degrees to 0.1 degC, rounded half away from zero") {
  std::int16_t d = 0;
  CHECK(temp_from_centi(2150, d));
  CHECK(d == 215);
  CHECK(temp_from_centi(2154, d));
  CHECK(d == 215);
  CHECK(temp_from_centi(2155, d));
  CHECK(d == 216);
  CHECK(temp_from_centi(-5, d));
  CHECK(d == -1);
  CHECK(temp_from_centi(-4, d));
  CHECK(d == 0);
  CHECK(temp_from_centi(-1999, d));
  CHECK(d == -200);
  CHECK_FALSE(temp_from_centi(INT16_MIN, d));  // 0x8000 = invalid
}

TEST_CASE("temperature: back to centi-degrees, clamped") {
  CHECK(temp_to_centi(215) == 2150);
  CHECK(temp_to_centi(-200) == -2000);
  CHECK(temp_to_centi(32767) == 32767);
  CHECK(temp_to_centi(-32768) == -32767);  // never the invalid marker
}

TEST_CASE("humidity: centi-percent to 0.1 percent") {
  std::uint16_t h = 0;
  CHECK(humidity_from_centi(5512, h));
  CHECK(h == 551);
  CHECK(humidity_from_centi(10000, h));
  CHECK(h == 1000);
  CHECK(humidity_from_centi(0, h));
  CHECK(h == 0);
  CHECK_FALSE(humidity_from_centi(0xFFFF, h));
  CHECK_FALSE(humidity_from_centi(10001, h));  // not a humidity
  CHECK(humidity_to_centi(551) == 5510);
  CHECK(humidity_to_centi(5000) == 10000);  // clamped to 100 %
}

TEST_CASE("illuminance: ZCL log scale to lux and back") {
  std::uint16_t lux = 99;
  CHECK(lux_from_measured(0, lux));  // too dark to measure
  CHECK(lux == 0);
  CHECK(lux_from_measured(1, lux));  // 10^0
  CHECK(lux == 1);
  CHECK(lux_from_measured(10001, lux));
  CHECK(lux == 10);
  CHECK(lux_from_measured(30001, lux));
  CHECK(lux == 1000);
  CHECK(lux_from_measured(0xFFFE, lux));  // far above the u16 range
  CHECK(lux == 65535);
  CHECK_FALSE(lux_from_measured(0xFFFF, lux));

  CHECK(lux_to_measured(0) == 0);
  CHECK(lux_to_measured(1) == 1);
  CHECK(lux_to_measured(1000) == 30001);
  // Round trip stays within the log scale's resolution for every decade.
  for (int v : {2, 7, 50, 333, 4000, 40000}) {
    std::uint16_t back = 0;
    REQUIRE(lux_from_measured(lux_to_measured(static_cast<std::uint16_t>(v)), back));
    const int diff = std::abs(static_cast<int>(back) - v);
    const bool close = diff <= 1 || diff * 1000 / v < 3;  // within 1 lux or 0.3 %
    CHECK(close);
  }
}

TEST_CASE("battery: half-percent to percent") {
  std::uint8_t p = 0;
  CHECK(battery_from_half_percent(200, p));
  CHECK(p == 100);
  CHECK(battery_from_half_percent(101, p));
  CHECK(p == 51);
  CHECK(battery_from_half_percent(0, p));
  CHECK(p == 0);
  CHECK_FALSE(battery_from_half_percent(0xFF, p));  // unknown
  CHECK_FALSE(battery_from_half_percent(201, p));
  CHECK(battery_to_half_percent(100) == 200);
  CHECK(battery_to_half_percent(37) == 74);
  CHECK(battery_to_half_percent(250) == 200);
}

TEST_CASE("thermostat mode / level / IAS helpers") {
  CHECK(mode_valid(0));
  CHECK(mode_valid(1));
  CHECK_FALSE(mode_valid(2));
  CHECK(mode_valid(3));
  CHECK(mode_valid(4));
  CHECK_FALSE(mode_valid(5));
  CHECK(level_valid(254));
  CHECK_FALSE(level_valid(255));
  CHECK(ias_alarm(0x0001));
  CHECK(ias_alarm(0x0009));    // alarm1 + low battery
  CHECK_FALSE(ias_alarm(0x0008));  // low battery only
  CHECK_FALSE(ias_alarm(0x0004));  // tamper only
}
