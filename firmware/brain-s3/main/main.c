// Phase 0 bring-up: blink the onboard addressable RGB LED on the ESP32-S3-Zero.
//
// Sole purpose is to confirm the ESP-IDF toolchain builds and flashes this
// board over its USB-C port. No protocol / core logic here yet.
//
// The Waveshare ESP32-S3-Zero has a single WS2812 on GPIO 21. Adjust
// BLINK_GPIO if your board differs.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "led_strip.h"

#define BLINK_GPIO 21
#define BLINK_PERIOD_MS 500

static const char *TAG = "brain-s3";

void app_main(void) {
  led_strip_config_t strip_config = {
      .strip_gpio_num = BLINK_GPIO,
      .max_leds = 1,
  };
  led_strip_rmt_config_t rmt_config = {
      .resolution_hz = 10 * 1000 * 1000,  // 10 MHz
  };
  led_strip_handle_t strip;
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip));

  ESP_LOGI(TAG, "brain-s3 blink up on GPIO %d", BLINK_GPIO);

  bool on = false;
  while (1) {
    if (on) {
      ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, 0, 16, 0));  // dim green
      ESP_ERROR_CHECK(led_strip_refresh(strip));
    } else {
      ESP_ERROR_CHECK(led_strip_clear(strip));
    }
    on = !on;
    vTaskDelay(pdMS_TO_TICKS(BLINK_PERIOD_MS));
  }
}
