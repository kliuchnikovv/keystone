// Calendar helpers shared by the schedule table and the sun calculation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace core {

// Local wall-clock time. weekday: 0 = Sunday … 6 = Saturday (struct tm tm_wday).
struct LocalTime {
  int year;     // e.g. 2026
  int month;    // 1..12
  int day;      // 1..31
  int hour;     // 0..23
  int minute;   // 0..59
  int weekday;  // 0..6
};

// Days since 1970-01-01 for a civil date (proleptic Gregorian, H. Hinnant).
inline std::int32_t civil_day_number(int year, int month, int day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const int yoe = year - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int32_t>(era * 146097 + doe - 719468);
}

// The civil date of a day number (inverse of civil_day_number; H. Hinnant).
inline void civil_from_days(std::int32_t z, int& year, int& month, int& day) noexcept {
  z += 719468;
  const std::int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int32_t doe = z - era * 146097;
  const std::int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::int32_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

// "YYYY-MM-DD" -> day number. Real calendar dates only, years 2000..2099 (a one-off schedule has no use for more).
inline bool parse_date(const char* s, std::int32_t& day_number) noexcept {
  if (s == nullptr) return false;
  for (int i = 0; i < 10; ++i) {
    const char c = s[i];
    if (i == 4 || i == 7 ? c != '-' : c < '0' || c > '9') return false;
  }
  if (s[10] != '\0') return false;
  const int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
  const int m = (s[5] - '0') * 10 + (s[6] - '0');
  const int d = (s[8] - '0') * 10 + (s[9] - '0');
  if (y < 2000 || y > 2099 || m < 1 || m > 12 || d < 1) return false;
  const std::int32_t n = civil_day_number(y, m, d);
  int ry = 0, rm = 0, rd = 0;
  civil_from_days(n, ry, rm, rd);  // 31 Feb comes back as 3 Mar: not a date
  if (ry != y || rm != m || rd != d) return false;
  day_number = n;
  return true;
}

// day number -> "YYYY-MM-DD" (`cap` >= 11).
inline void format_date(std::int32_t day_number, char* out, std::size_t cap) noexcept {
  int y = 0, m = 0, d = 0;
  civil_from_days(day_number, y, m, d);
  std::snprintf(out, cap, "%04d-%02d-%02d", y, m, d);
}

}  // namespace core
