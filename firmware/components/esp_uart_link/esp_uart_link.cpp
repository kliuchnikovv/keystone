#include "esp_uart_link.hpp"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

namespace fw {

namespace {
const char* kTag = "esp_uart_link";
}

esp_err_t EspUartLink::begin() noexcept {
  uart_config_t uc = {};  // zero-init keeps us robust across IDF field additions
  uc.baud_rate = cfg_.baud;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;

  esp_err_t err = uart_driver_install(cfg_.port, cfg_.rx_buffer, cfg_.tx_buffer,
                                      0, nullptr, 0);
  if (err != ESP_OK) return err;
  err = uart_param_config(cfg_.port, &uc);
  if (err != ESP_OK) return err;
  err = uart_set_pin(cfg_.port, cfg_.tx_gpio, cfg_.rx_gpio, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE);
  if (err != ESP_OK) return err;
  // The peer's TX is high-Z while it resets or boots (or is being flashed), so an
  // undriven RX line picks up noise and shows as bad frames (crc_errors). A UART
  // idles high — pull RX up so a silent peer reads as idle, not as garbage.
  return gpio_set_pull_mode(static_cast<gpio_num_t>(cfg_.rx_gpio), GPIO_PULLUP_ONLY);
}

void EspUartLink::start() noexcept {
  xTaskCreate(&EspUartLink::rx_task, "uart_rx", static_cast<uint32_t>(cfg_.task_stack),
              this, cfg_.task_prio, nullptr);
}

bool EspUartLink::write(const std::uint8_t* data, std::size_t len) noexcept {
  const int written = uart_write_bytes(cfg_.port, data, len);
  return written == static_cast<int>(len);
}

void EspUartLink::set_receiver(ReceiveFn fn, void* ctx) noexcept {
  rx_ = fn;
  rx_ctx_ = ctx;
}

void EspUartLink::rx_task(void* arg) {
  static_cast<EspUartLink*>(arg)->run_rx();
}

void EspUartLink::run_rx() {
  uint8_t buf[256];
  const int chunk = cfg_.rx_chunk < static_cast<int>(sizeof(buf))
                        ? cfg_.rx_chunk
                        : static_cast<int>(sizeof(buf));
  ESP_LOGI(kTag, "UART%d link up: tx=%d rx=%d @ %d baud",
           static_cast<int>(cfg_.port), cfg_.tx_gpio, cfg_.rx_gpio, cfg_.baud);
  for (;;) {
    // uart_read_bytes(len) only returns early on timeout, so asking for a full
    // chunk made every short frame (an ack, the tail of a long frame) wait out
    // the whole timeout: ~20 ms per message. Wait for the first byte, then take
    // whatever else has already arrived without blocking.
    int n = uart_read_bytes(cfg_.port, buf, 1, pdMS_TO_TICKS(20));
    if (n > 0) {
      std::size_t avail = 0;
      uart_get_buffered_data_len(cfg_.port, &avail);
      if (avail > 0) {
        const std::size_t room = static_cast<std::size_t>(chunk - 1);
        const int m = uart_read_bytes(cfg_.port, buf + 1,
                                      static_cast<uint32_t>(avail < room ? avail : room), 0);
        if (m > 0) n += m;
      }
      if (rx_ != nullptr) rx_(rx_ctx_, buf, static_cast<std::size_t>(n));
    }
    if (tick_ != nullptr) tick_(tick_ctx_);
  }
}

}  // namespace fw
