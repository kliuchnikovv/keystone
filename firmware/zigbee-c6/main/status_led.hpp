// Status LED on the C6-DevKitC's on-board WS2812 (GPIO 8).
#pragma once

#include <cstdint>

namespace status_led {

enum class State : std::uint8_t {
  Booting,   // blue, steady
  Ready,     // green, steady
  Pairing,   // green, slow pulse
  Off,       // dark
};

void init();
void set(State s);
State current();
void tick(std::uint32_t now_ms);

}  // namespace status_led
