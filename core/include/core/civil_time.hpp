// Calendar helpers shared by the schedule table and the sun calculation.
#pragma once

#include <cstdint>

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

}  // namespace core
