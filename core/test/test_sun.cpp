#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <string>

#include "core/sun.hpp"

namespace {

// Golden values from the `astral` library (an independent implementation): lat, lon, UTC offset in
// minutes, local date, sunrise and sunset as local minutes of the day. The two algorithms agree to
// about a minute; 3 is the tolerance.
struct Golden {
  double lat, lon;
  int offset, y, m, d, rise, set;
};
const Golden kGolden[] = {
    {50.08, 14.43, 120, 2026, 3, 20, 425, 1154}, // Prague
    {50.08, 14.43, 120, 2026, 6, 21, 292, 1275},
    {50.08, 14.43, 120, 2026, 9, 23, 410, 1138},
    {50.08, 14.43, 120, 2026, 12, 21, 538, 1021},
    {50.08, 14.43, 120, 2026, 10, 11, 437, 1099},
    {51.48, 0.00, 0, 2026, 3, 20, 363, 1092}, // Greenwich
    {51.48, 0.00, 0, 2026, 6, 21, 223, 1220},
    {51.48, 0.00, 0, 2026, 9, 23, 347, 1075},
    {51.48, 0.00, 0, 2026, 12, 21, 483, 952},
    {51.48, 0.00, 0, 2026, 10, 11, 377, 1035},
    {-33.87, 151.21, 660, 2026, 3, 20, 418, 1146}, // Sydney
    {-33.87, 151.21, 660, 2026, 6, 21, 480, 1073},
    {-33.87, 151.21, 660, 2026, 9, 23, 404, 1131},
    {-33.87, 151.21, 660, 2026, 12, 21, 340, 1205},
    {-33.87, 151.21, 660, 2026, 10, 11, 379, 1144},
    {40.71, -74.00, -240, 2026, 3, 20, 419, 1147}, // New York
    {40.71, -74.00, -240, 2026, 6, 21, 325, 1230},
    {40.71, -74.00, -240, 2026, 9, 23, 404, 1131},
    {40.71, -74.00, -240, 2026, 12, 21, 496, 1051},
    {40.71, -74.00, -240, 2026, 10, 11, 423, 1101},
    {1.35, 103.82, 480, 2026, 3, 20, 429, 1155}, // Singapore
    {1.35, 103.82, 480, 2026, 6, 21, 420, 1152},
    {1.35, 103.82, 480, 2026, 9, 23, 414, 1140},
    {1.35, 103.82, 480, 2026, 12, 21, 421, 1143},
    {1.35, 103.82, 480, 2026, 10, 11, 409, 1133},
    {69.65, 18.96, 120, 2026, 3, 20, 404, 1141}, // Tromso
    {69.65, 18.96, 120, 2026, 9, 23, 388, 1122},
    {69.65, 18.96, 120, 2026, 10, 11, 459, 1041},
    {-36.85, 174.76, 780, 2026, 3, 20, 443, 1172}, // Auckland (+13: the UTC date differs from the local one)
    {-36.85, 174.76, 780, 2026, 6, 21, 513, 1091},
    {-36.85, 174.76, 780, 2026, 9, 23, 429, 1157},
    {-36.85, 174.76, 780, 2026, 12, 21, 358, 1239},
    {-36.85, 174.76, 780, 2026, 10, 11, 403, 1173},
    {21.31, -157.86, -600, 2026, 3, 20, 395, 1122}, // Honolulu (-10)
    {21.31, -157.86, -600, 2026, 6, 21, 350, 1156},
    {21.31, -157.86, -600, 2026, 9, 23, 380, 1106},
    {21.31, -157.86, -600, 2026, 12, 21, 424, 1074},
    {21.31, -157.86, -600, 2026, 10, 11, 386, 1089},
    {27.72, 85.32, 345, 2026, 6, 21, 308, 1142}, // Kathmandu (+5:45)
    {27.72, 85.32, 345, 2026, 9, 23, 352, 1079},
    {27.72, 85.32, 345, 2026, 12, 21, 410, 1033},
    {27.72, 85.32, 345, 2026, 10, 11, 361, 1058},
};

int circular_diff(int a, int b) {
  int d = std::abs(a - b) % 1440;
  return d > 720 ? 1440 - d : d;
}

}  // namespace

TEST_CASE("sunrise and sunset match an independent implementation worldwide, in every season") {
  for (const auto& g : kGolden) {
    CAPTURE(g.lat);
    CAPTURE(g.lon);
    CAPTURE(g.m);
    CAPTURE(g.d);
    const auto t = core::sun_times_local(g.y, g.m, g.d, g.lat, g.lon, g.offset);
    REQUIRE(t.valid);
    CHECK(circular_diff(t.rise, g.rise) <= 3);
    CHECK(circular_diff(t.set, g.set) <= 3);
  }
}

TEST_CASE("polar day and polar night have no sunrise or sunset; bad coordinates are not a location") {
  CHECK_FALSE(core::sun_times_local(2026, 6, 21, 69.65, 18.96, 120).valid);   // midnight sun
  CHECK_FALSE(core::sun_times_local(2026, 12, 21, 69.65, 18.96, 120).valid);  // polar night
  CHECK(core::sun_times_local(2026, 3, 20, 69.65, 18.96, 120).valid);         // equinox: both exist
  CHECK_FALSE(core::sun_times_local(2026, 6, 21, 91.0, 0.0, 0).valid);
  CHECK_FALSE(core::sun_times_local(2026, 6, 21, 0.0, 181.0, 0).valid);
  CHECK_FALSE(core::sun_times_local(2026, 6, 21, -90.5, 0.0, 0).valid);
}

TEST_CASE("the answer is always a minute of the day, also across the date line and for odd offsets") {
  for (int off : {-720, -570, 0, 345, 765, 840}) {
    for (int month = 1; month <= 12; ++month) {
      const auto t = core::sun_times_local(2026, month, 15, 10.0, 170.0, off);
      REQUIRE(t.valid);
      CHECK((t.rise >= 0 && t.rise < 1440));
      CHECK((t.set >= 0 && t.set < 1440));
    }
  }
}

TEST_CASE("daylight is longer in summer than in winter, and the other way round in the south") {
  auto day_len = [](int month, double lat) {
    const auto t = core::sun_times_local(2026, month, 21, lat, 10.0, 0);
    return (t.set - t.rise + 1440) % 1440;
  };
  CHECK(day_len(6, 50.0) > day_len(12, 50.0) + 240);
  CHECK(day_len(12, -35.0) > day_len(6, -35.0) + 120);
  CHECK(std::abs(day_len(3, 0.0) - 720) <= 8);  // equator: about 12 h all year
}

TEST_CASE("time points: fixed minutes and sun-relative ones round-trip through text") {
  struct Case {
    const char* text;
    bool ok;
  };
  const Case cases[] = {
      {"00:00", true}, {"07:30", true}, {"23:59", true}, {"sunrise", true}, {"sunset", true},
      {"sunset+30", true}, {"sunrise-15", true}, {"sunset+180", true}, {"sunrise-180", true},
      {"sunset+0", true},
      {"24:00", false}, {"7:30", false}, {"07:60", false}, {"sunset+181", false}, {"sunrise-181", false},
      {"sunset+", false}, {"sunset30", false}, {"sunset+3x", false}, {"sunsetx", false},
      {"Sunset", false}, {"", false}, {"noon", false}, {"sunrise+-5", false}, {"sunrise 30", false},
  };
  for (const auto& c : cases) {
    CAPTURE(c.text);
    std::uint16_t p = 0;
    CHECK(core::parse_time_point(c.text, p) == c.ok);
    if (c.ok) {
      CHECK(core::valid_point(p));
      char back[24];
      core::format_time_point(p, back, sizeof(back));
      std::uint16_t again = 0;
      REQUIRE(core::parse_time_point(back, again));
      CHECK(again == p);
    }
  }
  std::uint16_t p = 0;
  REQUIRE(core::parse_time_point("sunset+0", p));
  char buf[24];
  core::format_time_point(p, buf, sizeof(buf));
  CHECK(std::string(buf) == "sunset");  // the canonical spelling has no "+0"
  REQUIRE(core::parse_time_point("sunrise-15", p));
  core::format_time_point(p, buf, sizeof(buf));
  CHECK(std::string(buf) == "sunrise-15");
  REQUIRE(core::parse_time_point("22:05", p));
  CHECK(p == 22 * 60 + 5);
  core::format_time_point(p, buf, sizeof(buf));
  CHECK(std::string(buf) == "22:05");
  CHECK_FALSE(core::parse_time_point(nullptr, p));
}

TEST_CASE("only well-formed points are valid stored values; 0xFFFF stays reserved") {
  CHECK(core::valid_point(0));
  CHECK(core::valid_point(1439));
  CHECK_FALSE(core::valid_point(1440));
  CHECK_FALSE(core::valid_point(0xFFFF));
  CHECK_FALSE(core::valid_point(0x4000));  // solar bit missing
  CHECK(core::valid_point(core::solar_point(true, 30)));
  CHECK(core::valid_point(core::solar_point(false, -180)));
  CHECK_FALSE(core::valid_point(core::solar_point(true, 181)));
  CHECK_FALSE(core::valid_point(static_cast<std::uint16_t>(core::solar_point(true, 0) | 0x1000)));  // reserved bit
  for (int off = -180; off <= 180; ++off) {
    CHECK(core::valid_point(core::solar_point(true, off)));
    CHECK(core::point_offset(core::solar_point(true, off)) == off);
    CHECK(core::point_is_sunset(core::solar_point(true, off)));
    CHECK_FALSE(core::point_is_sunset(core::solar_point(false, off)));
    CHECK(core::solar_point(true, off) != 0xFFFF);
  }
}

TEST_CASE("point_minute: fixed points need nothing, solar ones need today's sun") {
  core::SunTimes sun;
  sun.valid = true;
  sun.rise = 6 * 60;
  sun.set = 20 * 60;
  int m = -1;
  std::uint16_t p = 0;
  REQUIRE(core::parse_time_point("07:30", p));
  CHECK((core::point_minute(p, nullptr, m) && m == 450));
  REQUIRE(core::parse_time_point("sunset+30", p));
  CHECK((core::point_minute(p, &sun, m) && m == 20 * 60 + 30));
  REQUIRE(core::parse_time_point("sunrise-15", p));
  CHECK((core::point_minute(p, &sun, m) && m == 5 * 60 + 45));
  CHECK_FALSE(core::point_minute(p, nullptr, m));  // no location
  core::SunTimes none;
  CHECK_FALSE(core::point_minute(p, &none, m));     // polar day/night
  sun.set = 23 * 60 + 50;
  REQUIRE(core::parse_time_point("sunset+30", p));
  CHECK((core::point_minute(p, &sun, m) && m == 24 * 60 + 20));  // the raw sum: the caller decides about wrapping
}
