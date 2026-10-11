// Sunrise and sunset (NOAA general solar position algorithm, good to about a minute away from
// the polar circles) and "time points": a minute of the day that is either fixed ("22:30") or
// relative to the sun ("sunset+30", "sunrise-15"). Portable and allocation-free; the brain
// supplies the location and the UTC offset, so the core stays clock- and zone-free.
//
// A time point is a u16 so that schedules and rules keep their stored formats:
//   0..1439            a fixed minute of the (local) day
//   0x8000 | ...       sun-relative: bit 14 = sunset (else sunrise), low 12 bits = offset + 2048
//   0xFFFF             reserved ("none", kNoWindow in the rule engine)
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "core/civil_time.hpp"

namespace core {

inline constexpr int kMaxSunOffsetMin = 180;

/** Sunrise and sunset as LOCAL minutes of the day; `valid` is false when the sun does not rise or
 *  set that day (polar day/night) or the location is unknown. */
struct SunTimes {
  bool valid = false;
  int rise = 0;
  int set = 0;
};

namespace sun_detail {

constexpr double kPi = 3.14159265358979323846;
inline double rad(double d) { return d * kPi / 180.0; }
inline double deg(double r) { return r * 180.0 / kPi; }

// One NOAA evaluation for the Julian century `t`: UTC minutes of the event, or NaN when the sun does
// not cross the horizon. `sunset` selects which crossing.
inline double event_utc_min(double t, double lat, double lon, bool sunset) {
  const double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
  const double m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
  const double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
  const double mr = rad(m);
  const double c = std::sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                   std::sin(2 * mr) * (0.019993 - 0.000101 * t) + std::sin(3 * mr) * 0.000289;
  const double omega = 125.04 - 1934.136 * t;
  const double app = l0 + c - 0.00569 - 0.00478 * std::sin(rad(omega));
  const double obl = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0 +
                     0.00256 * std::cos(rad(omega));
  const double dec = std::asin(std::sin(rad(obl)) * std::sin(rad(app)));
  const double y = std::tan(rad(obl) / 2.0) * std::tan(rad(obl) / 2.0);
  const double l0r = rad(l0);
  const double eq = 4.0 * deg(y * std::sin(2 * l0r) - 2.0 * e * std::sin(mr) +
                              4.0 * e * y * std::sin(mr) * std::cos(2 * l0r) -
                              0.5 * y * y * std::sin(4 * l0r) - 1.25 * e * e * std::sin(2 * mr));
  const double lr = rad(lat);
  const double cosha = std::cos(rad(90.833)) / (std::cos(lr) * std::cos(dec)) - std::tan(lr) * std::tan(dec);
  if (!(cosha >= -1.0 && cosha <= 1.0)) return std::nan("");
  const double ha = deg(std::acos(cosha));  // degrees
  return 720.0 - 4.0 * (lon + (sunset ? -ha : ha)) - eq;
}

inline double julian_century(double jd) { return (jd - 2451545.0) / 36525.0; }

}  // namespace sun_detail

/**
 * Sunrise/sunset for the LOCAL calendar date, as local minutes of the day. `lat` north-positive,
 * `lon` east-positive (degrees); `utc_offset_min` is the zone's offset on that date (DST included).
 */
inline SunTimes sun_times_local(int year, int month, int day, double lat, double lon, int utc_offset_min) {
  SunTimes out;
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return out;
  // The UTC day that contains this local day's noon: the events happen around local solar noon.
  const std::int64_t noon_utc_min =
      static_cast<std::int64_t>(civil_day_number(year, month, day)) * 1440 + 720 - utc_offset_min;
  std::int64_t d = noon_utc_min / 1440;
  if (noon_utc_min < 0 && noon_utc_min % 1440 != 0) --d;  // floor
  const double jd0 = static_cast<double>(d) + 2440587.5;  // 0h UT of that day

  double ev[2];
  for (int k = 0; k < 2; ++k) {
    const bool sunset = k == 1;
    double min = sun_detail::event_utc_min(sun_detail::julian_century(jd0 + 0.5), lat, lon, sunset);
    if (std::isnan(min)) return out;
    min = sun_detail::event_utc_min(sun_detail::julian_century(jd0 + min / 1440.0), lat, lon, sunset);  // refine
    if (std::isnan(min)) return out;
    ev[k] = min;
  }
  auto local = [&](double utc_min) {
    long m = std::lround(utc_min) + utc_offset_min;
    m %= 1440;
    if (m < 0) m += 1440;
    return static_cast<int>(m);
  };
  out.valid = true;
  out.rise = local(ev[0]);
  out.set = local(ev[1]);
  return out;
}

// ---- time points -------------------------------------------------------------------------

inline constexpr std::uint16_t kSolarFlag = 0x8000;

inline constexpr bool is_solar_point(std::uint16_t p) noexcept { return (p & kSolarFlag) != 0 && p != 0xFFFF; }
inline constexpr bool is_fixed_point(std::uint16_t p) noexcept { return p < 1440; }
/** Valid as a stored value: a fixed minute or a well-formed sun-relative point. */
inline constexpr bool valid_point(std::uint16_t p) noexcept {
  if (is_fixed_point(p)) return true;
  if (!is_solar_point(p)) return false;
  if ((p & 0x3000) != 0) return false;  // bits 13..12 are reserved
  const int off = static_cast<int>(p & 0x0FFF) - 2048;
  return off >= -kMaxSunOffsetMin && off <= kMaxSunOffsetMin;
}
inline constexpr std::uint16_t solar_point(bool sunset, int offset_min) noexcept {
  return static_cast<std::uint16_t>(kSolarFlag | (sunset ? 0x4000 : 0) | static_cast<unsigned>(offset_min + 2048));
}
inline constexpr bool point_is_sunset(std::uint16_t p) noexcept { return (p & 0x4000) != 0; }
inline constexpr int point_offset(std::uint16_t p) noexcept { return static_cast<int>(p & 0x0FFF) - 2048; }

/**
 * The minute of the day a point means today: a fixed point is itself; a sun-relative point needs
 * `sun`. Returns false when it cannot be known (no location, no sunrise today). The result is the
 * raw sum (may be <0 or >=1440); callers decide whether to wrap (rule windows) or skip (schedules).
 */
inline bool point_minute(std::uint16_t p, const SunTimes* sun, int& minute) noexcept {
  if (is_fixed_point(p)) {
    minute = p;
    return true;
  }
  if (!is_solar_point(p) || sun == nullptr || !sun->valid) return false;
  minute = (point_is_sunset(p) ? sun->set : sun->rise) + point_offset(p);
  return true;
}

/** "HH:MM" or "sunrise" / "sunset" with an optional signed offset in minutes ("sunset+30"). */
inline bool parse_time_point(const char* s, std::uint16_t& out) noexcept {
  if (s == nullptr) return false;
  const bool rise = std::strncmp(s, "sunrise", 7) == 0;
  const bool set = std::strncmp(s, "sunset", 6) == 0;
  if (rise || set) {
    const char* p = s + (rise ? 7 : 6);
    int off = 0;
    if (*p != '\0') {
      if (*p != '+' && *p != '-') return false;
      const int sign = *p == '-' ? -1 : 1;
      ++p;
      if (*p < '0' || *p > '9') return false;
      int n = 0;
      for (; *p >= '0' && *p <= '9'; ++p) {
        n = n * 10 + (*p - '0');
        if (n > kMaxSunOffsetMin) return false;
      }
      if (*p != '\0') return false;
      off = sign * n;
    }
    out = solar_point(set, off);
    return true;
  }
  if (std::strlen(s) != 5 || s[2] != ':') return false;
  for (int i : {0, 1, 3, 4}) if (s[i] < '0' || s[i] > '9') return false;
  const int h = (s[0] - '0') * 10 + (s[1] - '0');
  const int m = (s[3] - '0') * 10 + (s[4] - '0');
  if (h > 23 || m > 59) return false;
  out = static_cast<std::uint16_t>(h * 60 + m);
  return true;
}

/** The inverse of parse_time_point ("22:30", "sunset+30", "sunrise-15", "sunset"). Returns the length. */
inline std::size_t format_time_point(std::uint16_t p, char* buf, std::size_t cap) noexcept {
  if (buf == nullptr || cap == 0) return 0;
  int n;
  if (is_fixed_point(p)) {
    n = std::snprintf(buf, cap, "%02u:%02u", static_cast<unsigned>(p / 60), static_cast<unsigned>(p % 60));
  } else if (valid_point(p)) {
    const int off = point_offset(p);
    const char* name = point_is_sunset(p) ? "sunset" : "sunrise";
    n = off == 0 ? std::snprintf(buf, cap, "%s", name) : std::snprintf(buf, cap, "%s%+d", name, off);
  } else {
    n = std::snprintf(buf, cap, "?");
  }
  return n < 0 ? 0 : static_cast<std::size_t>(n);
}

}  // namespace core
