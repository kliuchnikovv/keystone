#include "status_led.hpp"

#include "esp_log.h"
#include "led_strip.h"

namespace status_led {
namespace {

constexpr int kGpio = 8;
constexpr int kMaxBrightness = 12;

led_strip_handle_t g_strip = nullptr;
State g_state = State::Booting;
std::uint32_t g_last_ms = 0;

struct Rgb { std::uint8_t r, g, b; };

Rgb base_colour(State s) {
  switch (s) {
    case State::Booting: return {0, 0, kMaxBrightness};
    case State::Ready:   return {0, kMaxBrightness, 0};
    case State::Pairing: return {0, kMaxBrightness, 0};
    case State::Off:     return {0, 0, 0};
  }
  return {0, 0, 0};
}

bool pulsing(State s) { return s == State::Pairing; }

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
