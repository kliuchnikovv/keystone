// Status LED on the brain's on-board WS2812. The GPIO is board-specific
// (CONFIG_KS_STATUS_LED_GPIO, defaults to 21 for the S3-Zero; 48 for
// ESP32-S3-DevKitC-1 v1.1 and WeAct Core S3 / N16R8 boards).
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
