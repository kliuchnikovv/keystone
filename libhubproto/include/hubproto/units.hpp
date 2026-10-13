// Unit conversions between the hub's attribute units (attrs.hpp) and the units Zigbee (ZCL) and
// Matter use for the same quantity. Pure functions, no radio headers, host-tested.
//
// ZCL and Matter agree on most encodings (level, mireds, system mode, the temperature/humidity
// "centi" units, the log illuminance scale, half-percent battery), so one set of functions
// serves both chips. A reading the device marks invalid (0x8000, 0xFFFF, 0xFF) is rejected so it
// never reaches a rule as if it were a measurement.
#pragma once

#include <cmath>
#include <cstdint>

namespace hubproto::units {

namespace detail {
// Divide by ten rounding half away from zero (so -0.05 °C -> -0.1 and 0.05 -> 0.1, like display).
constexpr std::int32_t div10_round(std::int32_t v) noexcept {
  return (v >= 0 ? v + 5 : v - 5) / 10;
}
}  // namespace detail

// ---- temperature: hub i16 0.1 °C <-> ZCL/Matter int16 0.01 °C -------------------------------

inline bool temp_from_centi(std::int16_t centi, std::int16_t& deci) noexcept {
  if (centi == INT16_MIN) return false;  // 0x8000 = invalid
  deci = static_cast<std::int16_t>(detail::div10_round(centi));
  return true;
}
inline std::int16_t temp_to_centi(std::int16_t deci) noexcept {
  const std::int32_t c = static_cast<std::int32_t>(deci) * 10;
  return static_cast<std::int16_t>(c > INT16_MAX ? INT16_MAX : (c <= INT16_MIN ? INT16_MIN + 1 : c));
}

// ---- humidity: hub u16 0.1 % <-> ZCL/Matter uint16 0.01 % -----------------------------------

inline bool humidity_from_centi(std::uint16_t centi, std::uint16_t& deci) noexcept {
  if (centi == 0xFFFF || centi > 10000) return false;
  deci = static_cast<std::uint16_t>(detail::div10_round(centi));
  return true;
}
inline std::uint16_t humidity_to_centi(std::uint16_t deci) noexcept {
  return deci > 1000 ? std::uint16_t{10000} : static_cast<std::uint16_t>(deci * 10);
}

// ---- illuminance: hub u16 lux <-> ZCL/Matter MeasuredValue = 10000 * log10(lux) + 1 ---------
// MeasuredValue 0 means "too dark to measure" (reads as 0 lux); 0xFFFF is invalid.

inline bool lux_from_measured(std::uint16_t m, std::uint16_t& lux) noexcept {
  if (m == 0xFFFF) return false;
  if (m == 0) {
    lux = 0;
    return true;
  }
  const double v = std::pow(10.0, (static_cast<double>(m) - 1.0) / 10000.0);
  lux = v >= 65535.0 ? std::uint16_t{65535} : static_cast<std::uint16_t>(v + 0.5);
  return true;
}
inline std::uint16_t lux_to_measured(std::uint16_t lux) noexcept {
  if (lux == 0) return 0;
  const double m = 10000.0 * std::log10(static_cast<double>(lux)) + 1.0;
  return static_cast<std::uint16_t>(m + 0.5);
}

// ---- battery: hub u8 percent <-> ZCL/Matter half-percent (0..200) ---------------------------

inline bool battery_from_half_percent(std::uint8_t half, std::uint8_t& pct) noexcept {
  if (half > 200) return false;  // 0xFF = unknown
  pct = static_cast<std::uint8_t>((half + 1) / 2);
  return true;
}
inline std::uint8_t battery_to_half_percent(std::uint8_t pct) noexcept {
  return pct > 100 ? std::uint8_t{200} : static_cast<std::uint8_t>(pct * 2);
}

// ---- thermostat mode: hub enum8 == SystemMode; only the values we model are accepted --------

inline bool mode_valid(std::uint8_t m) noexcept {
  return m == 0 || m == 1 || m == 3 || m == 4;
}

// ---- brightness: hub u8 == ZCL/Matter CurrentLevel (0..254); 255 is "invalid" in ZCL -------

inline bool level_valid(std::uint8_t l) noexcept { return l <= 254; }

// ---- IAS zone status (ZCL attribute 0x0002 / status-change notification) --------------------
// bit0 alarm1: for a contact switch "opened", for a motion sensor "motion".

inline bool ias_alarm(std::uint16_t zone_status) noexcept { return (zone_status & 0x0001u) != 0; }

}  // namespace hubproto::units
