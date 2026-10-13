// Attribute ids, units and enum values shared by every chip and the brain (SPEC §6.1).
//
// The wire carries only attr_id + value; what a number *means* is fixed here so a Zigbee
// thermometer and a future Thread one look identical to rules, scenes and the app.
#pragma once

#include <cstdint>

namespace hubproto::attr {

// ---- light / switch ----
inline constexpr std::uint8_t kOnOff = 0x01;       // bool
inline constexpr std::uint8_t kBrightness = 0x02;  // u8, 0..254 (Zigbee/Matter CurrentLevel)
inline constexpr std::uint8_t kColorXy = 0x03;     // xy, CIE 1931 x/y as Zigbee ColorX/ColorY (0..65279)
inline constexpr std::uint8_t kColorTemp = 0x04;   // u16, mireds (1,000,000 / kelvin)

// ---- sensors ----
inline constexpr std::uint8_t kTemperature = 0x10;  // i16, 0.1 °C
inline constexpr std::uint8_t kHumidity = 0x11;     // u16, 0.1 % relative humidity
inline constexpr std::uint8_t kIlluminance = 0x12;  // u16, lux (saturates at 65535)
inline constexpr std::uint8_t kPowerW = 0x13;       // u16, W
inline constexpr std::uint8_t kEnergyWh = 0x14;     // u32, Wh
inline constexpr std::uint8_t kBattery = 0x1F;      // u8, percent 0..100

// ---- thermostat ----
inline constexpr std::uint8_t kSetpoint = 0x40;  // i16, 0.1 °C, occupied heating setpoint
inline constexpr std::uint8_t kMode = 0x41;      // enum8, Zigbee/Matter SystemMode (see ThermoMode)

// ---- presence / contact / button ----
inline constexpr std::uint8_t kOccupied = 0x50;  // bool, true = someone is there
inline constexpr std::uint8_t kContact = 0x51;   // bool, true = closed (Matter BooleanState StateValue)
inline constexpr std::uint8_t kEvent = 0x60;     // enum8, see ButtonEvent; returns to kIdle after ~0.4 s

// Thermostat SystemMode values we support: the same numbers in ZCL and Matter, so no translation.
enum class ThermoMode : std::uint8_t { Off = 0, Auto = 1, Cool = 3, Heat = 4 };

// A button press is a momentary STATE(event=N) followed by STATE(event=0): an edge, so a rule
// "event == 1" fires on every press even though two presses carry the same code.
enum class ButtonEvent : std::uint8_t {
  Idle = 0,
  Press = 1,    // single press / toggle
  Double = 2,
  Hold = 3,     // long press started
  Release = 4,  // released after a hold
  On = 5,       // "on" button of an on/off remote
  Off = 6,
  Up = 7,       // brighter (dim up)
  Down = 8,     // dimmer
};

// How long the event stays up before returning to Idle.
inline constexpr std::uint32_t kButtonEventMs = 400;

}  // namespace hubproto::attr
