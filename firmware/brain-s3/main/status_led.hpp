// Status LED on the ESP32-S3-Zero's on-board WS2812 (GPIO 21).
// Colour reflects hub health; updated from the main loop.
#pragma once

#include <cstdint>

namespace status_led {

enum class State : std::uint8_t {
  Booting,   // blue, steady
  Healthy,   // green, steady
  Degraded,  // amber/yellow, steady
  Offline,   // red, steady
  Setup,     // blue, slow pulse
  Pairing,   // green, slow pulse
  Off,       // user turned it off
};

void init();
void set(State s);
State current();
void tick(std::uint32_t now_ms);

}  // namespace status_led
