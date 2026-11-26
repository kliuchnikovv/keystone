#include "status_led.hpp"

#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

namespace status_led {
namespace {

// The WS2812 pin is board-specific: 21 on the S3-Zero (original brain), 48 on
// ESP32-S3-DevKitC-1 v1.1 / WeAct / N16R8 boards. Picked in sdkconfig via
// main/Kconfig.projbuild → CONFIG_KS_STATUS_LED_GPIO.
constexpr int kGpio = CONFIG_KS_STATUS_LED_GPIO;
constexpr int kMaxBrightness = 12;

led_strip_handle_t g_strip = nullptr;
State g_state = State::Booting;
std::uint32_t g_last_ms = 0;

struct Rgb { std::uint8_t r, g, b; };

// espressif/led_strip (>=3.x) takes honest RGB and does the GRB reorder for
// WS2812 itself (see led_strip_rmt_set_pixel + LED_STRIP_COLOR_COMPONENT_FMT_GRB
// default). Before 3.x some bundled drivers sent bytes as-is and the hub
// pre-swapped R and G; that pre-swap now shows up as inverted colours
// (Healthy came out RED, Offline came out GREEN). Swap removed.
Rgb base_colour(State s) {
  switch (s) {
    case State::Booting:  return {0, 0, kMaxBrightness};                                                   // blue
    case State::Healthy:  return {0, kMaxBrightness, 0};                                                   // green
    case State::Degraded: return {kMaxBrightness, static_cast<std::uint8_t>(kMaxBrightness / 3), 0};       // amber
    case State::Offline:  return {kMaxBrightness, 0, 0};                                                   // red
    case State::Setup:    return {0, 0, kMaxBrightness};                                                   // blue pulse
    case State::Pairing:  return {0, kMaxBrightness, 0};                                                   // green pulse
    case State::Off:      return {0, 0, 0};
  }
  return {0, 0, 0};
}

bool pulsing(State s) {
  return s == State::Setup || s == State::Pairing;
}

void apply(Rgb c) {
  if (g_strip == nullptr) return;
  led_strip_set_pixel(g_strip, 0, c.r, c.g, c.b);
  led_strip_refresh(g_strip);
}

}  // namespace

void init() {
  led_strip_config_t cfg = {};
  cfg.strip_gpio_num = kGpio;
  cfg.max_leds = 1;
  cfg.led_model = LED_MODEL_WS2812;

  led_strip_rmt_config_t rmt = {};
  rmt.resolution_hz = 10 * 1000 * 1000;

  if (led_strip_new_rmt_device(&cfg, &rmt, &g_strip) != ESP_OK) {
    ESP_LOGW("led", "led_strip init failed");
    g_strip = nullptr;
    return;
  }
  apply(base_colour(g_state));
}

void set(State s) {
  if (s == g_state) return;
  g_state = s;
  if (!pulsing(s)) apply(base_colour(s));
}

State current() { return g_state; }

void tick(std::uint32_t now_ms) {
  if (g_strip == nullptr || !pulsing(g_state)) return;
  if (now_ms - g_last_ms < 50) return;
  g_last_ms = now_ms;

  // 2 s cycle: 0..1000 ms ramp up, 1000..2000 ramp down
  const std::uint32_t phase = now_ms % 2000;
  const std::uint32_t level = phase < 1000 ? phase : 2000 - phase;
  const std::uint8_t brightness = static_cast<std::uint8_t>(level * kMaxBrightness / 1000);

  Rgb base = base_colour(g_state);
  Rgb out = {
    static_cast<std::uint8_t>(base.r * brightness / kMaxBrightness),
    static_cast<std::uint8_t>(base.g * brightness / kMaxBrightness),
    static_cast<std::uint8_t>(base.b * brightness / kMaxBrightness),
  };
  apply(out);
}

}  // namespace status_led
